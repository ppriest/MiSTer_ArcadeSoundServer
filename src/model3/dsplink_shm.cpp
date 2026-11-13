// SPDX-License-Identifier: GPL-3.0-or-later
// dsplink_shm.h's link.
#include "dsplink_shm.h"

#include <cstdio>

// n words from cached memory to the block, as few transactions as the A9 makes of them: 32-byte bursts
// (VST1 of four D registers, aligned) where the block is 32-byte aligned, 64-bit stores (STRD) on
// 8-byte boundaries, else words. Each is one AXI write, and the A9 waits for its response.
void dsplink_shm::copy_out(volatile uint32_t *d, const uint32_t *s, uint32_t n)
{
	while (n && (uintptr_t(d) & 31))
	{
		if (!(uintptr_t(d) & 7) && n >= 2) { *reinterpret_cast<volatile uint64_t *>(d) = uint64_t(s[1]) << 32 | s[0]; d += 2; s += 2; n -= 2; }
		else { *d++ = *s++; n--; }
		io_tx++;
	}
	for (; n >= 8; d += 8, s += 8, n -= 8)
	{
#if defined(__arm__) && defined(__ARM_NEON)
		asm volatile("vld1.32 {d16-d19}, [%1]\n\tvst1.64 {d16-d19}, [%0:256]"
		             :: "r"(d), "r"(s) : "d16", "d17", "d18", "d19", "memory");
#else
		for (int i = 0; i < 8; i++) d[i] = s[i];
#endif
		io_tx++;
	}
	for (; n >= 2; d += 2, s += 2, n -= 2, io_tx++) *reinterpret_cast<volatile uint64_t *>(d) = uint64_t(s[1]) << 32 | s[0];
	if (n) { *d = *s; io_tx++; }
}

void dsplink_shm::put(const dsplink_cmd &c)
{
	const uint32_t hi = c.kind << 29 | (c.addr & 0xFFFF) << 9;
	if (word_io)
	{
		volatile uint32_t *e = cmd + 2 * (cmd_n % CMD_RING);
		e[0] = c.data;
		e[1] = hi;
		cmd_n++; cmd_out = cmd_n; io_tx += 2;
		return;
	}
	cbuf[2 * cbuf_n] = c.data;
	cbuf[2 * cbuf_n + 1] = hi;
	cmd_n++;
	if (++cbuf_n == CMD_BUF) flush();
}

void dsplink_shm::flush()
{
	// the waiting entries are cmd_out .. cmd_n - 1; the ring may wrap inside them
	int k = 0;
	while (k < cbuf_n)
	{
		const uint32_t at = cmd_out % CMD_RING, room = CMD_RING - at;
		const uint32_t m = uint32_t(cbuf_n - k) < room ? uint32_t(cbuf_n - k) : room;
		copy_out(cmd + 2 * at, cbuf + 2 * k, 2 * m);
		cmd_out += m; k += int(m);
	}
	cbuf_n = 0;
}

void dsplink_shm::command(int chip, int kind, UINT32 addr, UINT16 data)
{
	if (kind == 3)
	{
		// every 68000 word write into sound RAM comes here: the ring's words go on, the rest stop
		addr = (addr - ring_lo[chip]) & 0x7FFFF;              // the word's offset in the chip's ring
		if (addr >= ring_words) return;                       // outside it: not the DSP's
	}
	else if (!guard.command(chip, kind, addr, data)) unsupported(guard.why);
	if (kind == 1) ring_lo[chip] = (data & 0x7Fu) << 12;
	if (kind == 5) dac18[chip & 1] = data & 1;
	prog.emit(chip, kind, addr, data, [&](const dsplink_cmd &c) { put(c); });
}

void dsplink_shm::gain(int chip, int slot, INT32 l, INT32 r)
{
	// the left pan, then the right (0..16,384); none while this process runs the DSP (fallback),
	// whose effect return is in the dry sums
	if (fall) return;
	dsplink_cmd c;
	if (dsplink_map(chip, 4, uint32_t(slot), uint32_t(l), c)) put(c);
	if (dsplink_map(chip, 4, uint32_t(slot + 16), uint32_t(r), c)) put(c);
}

void dsplink_shm::sample(const INT32 dry[4], const INT32 mixs[2][16], float out[4])
{
	const uint32_t pt = sample_n + delay - 1;                 // the fs at which the bridge takes it up
	volatile uint32_t *r = rec + REC_STRIDE * (pt % REC_RING);
	const bool timed = clock && (sample_n & 255) == 0;
	int64_t t0 = 0, t1 = 0;
	if (timed) { t0 = clock(); t1 = clock(); }                 // the clock's own cost, taken off below
	// the 36 entries' data words (the address words are constant: rec_hi): MIXS of both chips, then the
	// dry sums, the music in the front pair (towards zero, as Supermodel's float mix converts)
	uint32_t d[36];
	for (int c = 0; c < 2; c++)
		for (int i = 0; i < 16; i++) d[16 * c + i] = dsplink_mixs(c, i, mixs[c][i]).data;
	if (dsb_q8)
	{
		const int sh = dac18[0] ? 0 : 2;
		const INT32 x[4] = {dry[0] + (dsb_q8[0] / 256) * (1 << sh), dry[1] + (dsb_q8[1] / 256) * (1 << sh), dry[2], dry[3]};
		for (int i = 0; i < 4; i++) d[32 + i] = dsplink_dry(i, x[i]).data;
		dsb_q8 += 2;
	}
	else
		for (int i = 0; i < 4; i++) d[32 + i] = dsplink_dry(i, dry[i]).data;
	const uint32_t tag = epoch << 16 | (pt & 0xFFFF);
	if (word_io)
	{
		// the old writes, a word at a time, to compare
		for (int i = 0; i < 36; i++) { r[2 * i] = d[i]; r[2 * i + 1] = rec_hi(i); }
		r[73] = cmd_n;
		__sync_synchronize();
		r[72] = tag;
		io_tx += 74;
	}
	else
	{
		flush();                                              // every command the count below takes in
		const uint32_t slot = pt % REC_RING;
		uint32_t z = 0, zd = 0;                               // the data words that are zero: MIXS, dry sums
		for (int i = 0; i < 32; i++) z |= uint32_t(d[i] == 0) << i;
		for (int i = 0; i < 4; i++) zd |= uint32_t(d[32 + i] == 0) << i;
		if (full_io || !slot_ok[slot] || ((pt >> 9) & 127) == (slot & 127))
		{
			for (int i = 0; i < 36; i++) { rbuf[2 * i] = d[i]; rbuf[2 * i + 1] = rec_hi(i); }
			copy_out(r, rbuf, 72);
			slot_ok[slot] = 1;
		}
		else
		{
			// only what changes: a data word not zero now, or not known to be zero in the slot
			for (uint32_t w = ~(z & zmask[slot]); w; w &= w - 1)
			{
				const int i = __builtin_ctz(w);
				r[2 * i] = d[i];
				io_tx++;
			}
			for (uint32_t w = ~(zd & zdry[slot]) & 15u; w; w &= w - 1)
			{
				const int i = 32 + __builtin_ctz(w);
				r[2 * i] = d[i];
				io_tx++;
			}
		}
		zmask[slot] = z;
		zdry[slot] = uint8_t(zd);
		// the count and the tag in one 64-bit store, last: the block is strongly-ordered memory, so every
		// store before it has reached DDR3 first, and an aligned 64-bit store is one write (DESIGN.md §19.12)
		*reinterpret_cast<volatile uint64_t *>(r + 72) = uint64_t(cmd_n) << 32 | tag;
		io_tx++;
	}
	if (timed)
	{
		const int64_t t2 = clock();
		io_ns += ((t2 - t1) - (t1 - t0)) * 256;
		io_timed++;
	}
	sample_n++;
	out[0] = out[1] = out[2] = out[3] = 0.0f;    // the FPGA has the output
}

void dsplink_shm::unsupported(const char *why)
{
	if (!unsup) fprintf(stderr, "arcsnd model3: the FPGA DSP cannot follow %s; the DSP runs here from now on\n", why);
	if (!fall)
	{
		// the fallback: the FPGA's effect return off (every pan zero); SCSP.cpp then runs the DSP
		// here and sends finished sums
		for (int c = 0; c < 2; c++)
			for (int i = 0; i < 32; i++) { dsplink_cmd z; if (dsplink_map(c, 4, uint32_t(i), 0, z)) put(z); }
		fall = true;
	}
	unsup = true;
}
