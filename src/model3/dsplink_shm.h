// SPDX-License-Identifier: GPL-3.0-or-later
// The HPS's half of the DSP split (study D-53): each sample's record and the DSP commands, written
// into rings the FPGA's bridge reads (docs/PROTOCOL.md, Model 3 version 2). The FPGA runs both
// chips' DSP programs (m3_scsp_dsp), adds the effect return to the dry sums, clips and plays.
//
// Version 2's rings (docs/PROTOCOL.md), in the FPGA's sample count fs (the board's sample n is
// fs = base + n, and the bridge takes it up at p = base + n + D - 1). An entry is 2 words, {data
// 24} {kind 3, 0 3, address 16, 0 9}, the engine's command as the bridge hands it on (below):
//   record ring: REC_RING records of REC_STRIDE words, record p at p % REC_RING: 36 entries, MIXS
//     of chip 0 and of chip 1, then the four dry sums, and its tag {0 14, epoch 2, p 16}
//     {commands written}, written last: the bridge keeps the record only if the tag is its own, and
//     then takes the commands up to that count, all written before it
//   command ring: CMD_RING entries
// epoch is the bridge's (it moves at every core reset), so no record of an earlier session passes.
#pragma once

#include <cstdint>
#include "SCSP.h"

// The engine's commands (m3_scsp_dsp): {kind 3, address 16, data 24}, the SCSP's writes laid out as its
// arrays hold them, so that the FPGA decodes nothing. Kinds: 0 both lanes of TEMP / MEMS / MIXS / the
// sums (24 bits), 1 the high lane (bits 23..8), 2 the low (7..0), 3 the ring {chip, offset 15}, 4 COEF
// / MADRS / the pans, 5 MPRO {chip, step 7, word 2}, 6 a chip's setting {chip, which 2}: 0 RBL, 1 Start
// {steps run 8, LastStep 8}, 2 DAC18B. Array addresses: TEMP {0, chip, index 7}, MEMS {10, chip, 0,
// index 5}, MIXS {10, chip, 10, index 4}, EFREG {11, chip, 00, index 4}, the channels' sums 0x190 + k;
// COEF {0, chip, index 6}, MADRS {10, chip, index 5}, pans {11, chip, right, slot 4}.
struct dsplink_cmd { uint32_t kind, addr, data; };
constexpr uint32_t DSPLINK_SUM = 0x190;
constexpr int32_t DSPLINK_DRY_MAX = (1 << 22) - 1;   // the dry sums held to +-2^22: the sums fit 24 bits,
                                                      // and past it the total is on a rail either way

// one SCSP_DSPLink command (chip, kind, addr, data) as the engine's: how many (0 or 1)
inline int dsplink_map(int chip, int kind, uint32_t addr, uint32_t data, dsplink_cmd &o)
{
	const uint32_t c = uint32_t(chip & 1);
	switch (kind)
	{
	case 0:
		if (addr >= 0x380 && addr < 0x3C0) { o = {4, c << 6 | (addr & 0x3F), data & 0xFFFF}; return 1; }
		if (addr >= 0x3C0 && addr < 0x3E0) { o = {4, 0x80 | c << 5 | (addr & 0x1F), data & 0xFFFF}; return 1; }
		if (addr >= 0x400 && addr < 0x600) { o = {5, c << 9 | (addr & 0x1FF), data & 0xFFFF}; return 1; }
		if (addr >= 0x600 && addr < 0x700) { o = {(addr & 1) ? 1u : 2u, c << 7 | ((addr >> 1) & 0x7F), data & 0xFFFF}; return 1; }
		if (addr >= 0x700 && addr < 0x740) { o = {(addr & 1) ? 1u : 2u, 0x100 | c << 6 | ((addr >> 1) & 0x1F), data & 0xFFFF}; return 1; }
		if (addr >= 0x760 && addr < 0x770) { o = {1, 0x180 | c << 6 | (addr & 0xF), data & 0xFFFF}; return 1; }
		return 0;
	case 1: o = {6, c << 2 | 0, (data >> 7) & 3}; return 1;
	case 2:
	{
		const uint32_t last = data & 0xFF, steps = last >= 127 ? 128 : last + 2;
		o = {6, c << 2 | 1, steps << 8 | last};
		return 1;
	}
	case 3: o = {3, c << 15 | (addr & 0x7FFF), data & 0xFFFF}; return 1;
	case 4: o = {4, 0xC0 | c << 5 | (addr & 0x1F), data & 0xFFFF}; return 1;
	case 5: o = {6, c << 2 | 2, data & 1}; return 1;
	}
	return 0;
}
// The engine runs a chip's steps past LastStep from MPRO as it stands, so the rows there must be zero:
// LastStep is the step after the last non-zero row, so they are, unless the 68000 writes one after its
// Start. This keeps the program and sends rows from LastStep on as zeros, and at a Start the whole
// program again under the new LastStep (Supermodel's tail: all-zero steps).
struct dsplink_prog
{
	uint16_t mpro[2][512] = {};
	unsigned last[2] = {0, 0};
	// SCSP_DSPLink's command as the engine's, through out(cmd)
	template <class F> void emit(int chip, int kind, uint32_t addr, uint32_t data, F out)
	{
		const int c = chip & 1;
		dsplink_cmd o;
		if (kind == 0 && addr >= 0x400 && addr < 0x600)
		{
			const uint32_t i = addr & 0x1FF;
			mpro[c][i] = uint16_t(data);
			o = {5, uint32_t(c) << 9 | i, (i >> 2) < last[c] ? data & 0xFFFF : 0};
			out(o);
			return;
		}
		if (kind == 2)
		{
			last[c] = data & 0xFF;
			for (uint32_t i = 0; i < 512; i++) out(dsplink_cmd{5, uint32_t(c) << 9 | i, (i >> 2) < last[c] ? mpro[c][i] : 0u});
		}
		if (dsplink_map(chip, kind, addr, data, o)) out(o);
	}
};

inline dsplink_cmd dsplink_mixs(int chip, int i, int32_t m) { return {0, 0x100 | uint32_t(chip & 1) << 6 | 0x20 | uint32_t(i & 15), (uint32_t(m) & 0xFFFFF) << 4}; }
inline dsplink_cmd dsplink_dry(int k, int32_t v)
{
	v = v < -(DSPLINK_DRY_MAX + 1) ? -(DSPLINK_DRY_MAX + 1) : v > DSPLINK_DRY_MAX ? DSPLINK_DRY_MAX : v;
	return {0, DSPLINK_SUM + uint32_t(k & 3), uint32_t(v) & 0xFFFFFF};
}

// What the FPGA's DSP does not do, seen in the commands the SCSP sends: a TABLE step inside the
// program, an RBL of 64 K words (the FPGA's ring is 32 K a chip), and RBP moved once the program has
// started (the FPGA's ring holds the old window). Any of them and the HPS has to run the DSP.
struct dsplink_guard
{
	uint8_t table[2][128] = {};            // MPRO word 2's bit 15, a step
	unsigned last[2] = {0, 0};
	bool started[2] = {false, false};
	int rbp[2] = {-1, -1};
	const char *why = nullptr;

	// false once the FPGA cannot follow
	bool command(int chip, int kind, uint32_t addr, uint16_t data)
	{
		if (kind == 0 && addr >= 0x400 && addr < 0x600 && (addr & 3) == 2)
		{
			const unsigned s = (addr - 0x400) >> 2;
			table[chip][s] = uint8_t(data >> 15);
			if (started[chip] && s < last[chip] && table[chip][s]) why = "a TABLE step";
		}
		else if (kind == 2)
		{
			last[chip] = data & 0xFF; started[chip] = true;
			for (unsigned s = 0; s < last[chip] && s < 128; s++) if (table[chip][s]) why = "a TABLE step";
		}
		else if (kind == 1)
		{
			if (((data >> 7) & 3) == 3) why = "a ring of 64 K words";
			const int p = data & 0x7F;
			if (started[chip] && rbp[chip] >= 0 && p != rbp[chip]) why = "RBP moved after the program started";
			rbp[chip] = p;
		}
		return why == nullptr;
	}
};

struct dsplink_shm : SCSP_DSPLink
{
	static constexpr int REC_ENTRIES = 37, REC_STRIDE = 128;
	static constexpr uint32_t REC_RING = 512, CMD_RING = 131072;   // the attach alone is about 67,000 entries
	volatile uint32_t *rec = nullptr;      // REC_RING records of REC_STRIDE words
	volatile uint32_t *cmd = nullptr;      // CMD_RING entries of 2 words
	uint32_t sample_n = 0, cmd_n = 0;      // the next record's sample (fs), the entries written
	uint32_t epoch = 0;
	bool fall = false;                     // the DSP runs here (fallback)
	// the Digital Sound Board's music (dsb.h, model3_dsb::q8: 1/256ths), {left, right} a sample, the
	// next sample's first; null on the sets without one. It goes into the record's front dry sums,
	// at the level Supermodel adds it to its mix (the SCSP's own when the master's DAC18B is set, four
	// times it otherwise), so the FPGA adds the effect return and clips it with the SCSP's sound and
	// needs nothing new (sm3-builds/sound/DESIGN.md §18.6)
	const int32_t *dsb_q8 = nullptr;
	int dac18[2] = {0, 0};                 // each chip's DAC18B, as the commands give it

	// a session from the FPGA's sample count base and the bridge's epoch
	uint32_t delay = 256;                  // D, from the bridge's header

	void start(uint32_t base, uint32_t ep, uint32_t d)
	{
		sample_n = base; cmd_n = 0; epoch = ep & 3; delay = d; fall = false; unsup = false; guard = dsplink_guard(); prog = dsplink_prog();
		dac18[0] = dac18[1] = 0;
		cbuf_n = 0; cmd_out = 0; io_tx = 0; io_ns = 0; io_timed = 0;
		for (auto &x : slot_ok) x = 0;
		for (auto &x : zmask) x = 0;
		for (auto &x : zdry) x = 0;
	}

	// On the board the block is strongly-ordered memory: /dev/mem maps it so because it lies above the
	// kernel's RAM (mem=511M), and the A9 then waits for DDR3's write response at every store, about
	// 110 ns (DESIGN.md §19). The bench's rings are ordinary memory and never paid it. So a record is made
	// here in cached memory and goes out in 32-byte bursts, its count and then its tag last, and the
	// commands wait here until the next record (or a full buffer) and go out the same way: 11 transactions
	// a record where the word-at-a-time writes made 74. word_io keeps the old writes, to compare.
	// The A9 splits a strongly-ordered 32-byte store into 64-bit writes, each waited for (the board: the
	// records 17-18% of core 0, §19.9), so a record now writes only what changes in its slot: each slot in
	// full once a session (and once every 128 laps), then only the data words that are not zero now or
	// were not zero there. The kind and address words never change, and the bridge writes the ring only
	// in its clear after a reset, before any session starts, so the slot holds exactly the old record.
	// full_io writes every record whole in bursts (0a3cc7cb), to compare.
	bool full_io = false;
	bool word_io = false;
	void flush();                          // the waiting commands into the ring (before any tag that counts them)
	// the time spent writing records, sampled one record in 256, when the session gives a clock
	int64_t (*clock)() = nullptr;
	uint64_t io_tx = 0;                    // stores or bursts to the block
	int64_t io_ns = 0;                     // the sampled records' write time, scaled to every record
	uint32_t io_timed = 0;                 // records timed
	uint32_t ring_lo[2] = {0, 0};          // each chip's ring window (word address), from RBP
	uint32_t ring_words = 1u << 15;
	dsplink_guard guard;
	dsplink_prog prog;
	bool unsup = false;

	void command(int chip, int kind, UINT32 addr, UINT16 data) override;
	void gain(int chip, int slot, INT32 l, INT32 r) override;
	void sample(const INT32 dry[4], const INT32 mixs[2][16], float out[4]) override;
	void unsupported(const char *why) override;
	bool fallback() const override { return fall; }
private:
	static constexpr int CMD_BUF = 64;     // entries waiting
	alignas(32) uint32_t cbuf[2 * CMD_BUF] = {};
	int cbuf_n = 0;
	uint32_t cmd_out = 0;                  // the entries in the ring (cmd_n less those waiting)
	alignas(32) uint32_t rbuf[2 * 36] = {};
	uint8_t slot_ok[REC_RING] = {};        // the slot written in full this session
	uint32_t zmask[REC_RING] = {};         // its data words known to be zero: bit i, MIXS entry i (0-31)
	uint8_t zdry[REC_RING] = {};           // and bit k, dry sum k (entries 32-35); 32-bit masks, as the A9 has them
	void put(const dsplink_cmd &c);
	// a record entry's address word, the same in every record: MIXS of chip 0, of chip 1, the four dry sums
	static uint32_t rec_hi(int i)
	{
		const dsplink_cmd c = i < 32 ? dsplink_mixs(i >> 4, i & 15, 0) : dsplink_dry(i - 32, 0);
		return c.kind << 29 | (c.addr & 0xFFFF) << 9;
	}
	void copy_out(volatile uint32_t *d, const uint32_t *s, uint32_t n);
};
