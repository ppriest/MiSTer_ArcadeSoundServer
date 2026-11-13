// SPDX-License-Identifier: GPL-3.0-or-later
// The Sega Model 3 sound board's session: runs while a Model 3 set is loaded, against the block the
// core's sound bridge (SegaModel3_MiSTer m3_sndbridge.sv) shares in DDR3. Version 2: the 68000 and
// the SCSPs' slots run here; both SCSPs' DSPs, their effect return and the clip run in the FPGA
// (m3_scsp_dsp), which takes a record a sample and the DSP's commands (dsplink_shm.h):
//
//   +0x00     {0 6, MIDI entries written 10, fs 16, D 16, "M" 8, version 4 (2), 0 2, epoch 2}
//                                                                         FPGA, every 32 samples
//   +0x40     {heartbeat 16, 0xA5 8, running 8 (bit 0); samples rendered, 32}   server
//   +0x48     {late wake-ups, 32; MIDI entries applied, 32}                 server
//   +0x1000   MIDI ring, 512 entries {0 16, fs 16, 0 23, data port 1, byte 8}       FPGA
//   +0x40000  record ring, 512 records of 512 bytes                        server
//   +0x100000 command ring, 131,072 entries of 8 bytes                     server
//
// One clock, the FPGA's 44.1 kHz count fs (16 bits in the block, widened here). A byte the
// PowerPC writes at fs = s reaches the master SCSP's MIDI input at board sample s + MIDI_LAT (the
// uPD71051's 10 bits at 31.25 kbaud, 320 us). The server renders the board's sample n once fs has
// passed it and files it as record n + D - 1, the fs at which the bridge takes it up; the bridge
// plays it at the next tick, D samples after it could first be rendered. The FPGA reads nothing
// of this side but the rings: a record carries its sample, the bridge's epoch and the count of
// commands written before it. A session starts once fs is seen to move, and starts again when the
// epoch moves (a core reset); it stops when fs stalls for 200 ms or the core changes. The sound
// ROMs come from the set's zip (roms.cpp), read as soon as the set is named, while Main is still
// loading it into the core: the core loads nothing for sound.
//
// The Digital Sound Board (dsb.h), on the sets that have one (Scud Race: DSB1; Daytona 2, Sega Rally 2: DSB2), runs
// here too, beside the SCSPs: it hears every byte the SCSP does, at the same sample, and its music,
// run ahead of the SCSP's samples a piece at a time, goes into each record's front dry sums
// (dsplink_shm::sample), which the bridge's engine clips with the SCSP's sound. Its ROMs come from the zip with the SCSP's.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "board.h"
#include "dsb.h"
#include "daemon.h"
#include "dsplink_shm.h"
#include "roms.h"
#include "SCSP.h"

namespace {

using namespace arcsnd;

// What a store to the block costs, by width (DESIGN.md §19.9): the block is strongly-ordered memory, and
// the A9 waits for each write's response. Measured each time a set is loaded, before its first session, in the block's
// unused 512 KB at +0x80000 (the bridge reads only its header, MIDI ring, records and commands), and
// printed; nothing depends on it.
void measure_stores(volatile uint32_t *w)
{
#if defined(__arm__) && defined(__ARM_NEON)
	alignas(32) static uint32_t src[32] = {};
	volatile uint32_t *d = w + 0x80000 / 4;
	constexpr int N = 512;
	auto time = [&](auto fn) {
		const int64_t t0 = now_ns();
		for (int i = 0; i < N; i++) fn(d + 32 * (i & 255));
		return double(now_ns() - t0) / N;
	};
	const double t4 = time([&](volatile uint32_t *p) { *p = 0; });
	const double t8 = time([&](volatile uint32_t *p) { *reinterpret_cast<volatile uint64_t *>(p) = 0; });
	const double t16 = time([&](volatile uint32_t *p) {
		asm volatile("ldm %1, {r4-r7}\n\tstm %0, {r4-r7}" :: "r"(p), "r"(src) : "r4", "r5", "r6", "r7", "memory"); });
	const double t32 = time([&](volatile uint32_t *p) {
		asm volatile("vld1.32 {d16-d19}, [%1]\n\tvst1.64 {d16-d19}, [%0:256]" :: "r"(p), "r"(src) : "d16", "d17", "d18", "d19", "memory"); });
	const double t64 = time([&](volatile uint32_t *p) {
		asm volatile("vldm %1, {d16-d23}\n\tvstm %0, {d16-d23}" :: "r"(p), "r"(src) : "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "memory"); });
	const double t128 = time([&](volatile uint32_t *p) {
		asm volatile("vldm %1, {d16-d31}\n\tvstm %0, {d16-d31}" :: "r"(p), "r"(src) : "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
		             "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31", "memory"); });
	double tr = 0;
	{
		uint32_t x = 0;
		const int64_t t0 = now_ns();
		for (int i = 0; i < N; i++) x += d[32 * (i & 255)];
		tr = double(now_ns() - t0) / N;
		if (x == 0x12345678) fprintf(stderr, " ");
	}
	fprintf(stderr, "arcsnd model3: the block's stores, ns each: 4 B %.0f, 8 B %.0f, 16 B (stm) %.0f, 32 B (vst1) %.0f, "
	        "64 B (vstm) %.0f, 128 B (vstm) %.0f; a 4 B load %.0f\n", t4, t8, t16, t32, t64, t128, tr);
#else
	(void)w;
#endif
}

constexpr uint32_t SHM = 0x3F200000, SHM_SIZE = 0x200000;
constexpr uint32_t OFF_HDR = 0x00, OFF_STATUS = 0x40, OFF_STATS = 0x48, OFF_RING = 0x1000,
                   OFF_REC = 0x40000, OFF_CMD = 0x100000;
constexpr uint32_t RING = 512;
constexpr uint32_t MIDI_LAT = 14;               // samples: a MIDI byte's 10 bits at 31.25 kbaud
constexpr int CHUNK = 64;                       // samples the board runs between looks at the ring


}  // namespace

// the sets claimed: every set in the ROM table (roms_table.h, all of Supermodel's Games.xml with a sound board)
void model3_sets(std::vector<const char *> &out)
{
	model3_rom_sets(out);
}

bool model3_claims(const char *name)
{
	std::vector<const char *> sets;
	model3_rom_sets(sets);
	for (const char *s : sets)
		if (!strcmp(name, s)) return true;
	return false;
}

// runs until the core changes or the process is stopped; mem is /dev/mem
void model3_run(int mem, bool verbose)
{
	void *p = map(mem, SHM, SHM_SIZE);
	if (!p) { perror("mmap model3 block"); sleep_ns(1'000'000'000); return; }
	shared sh{static_cast<volatile uint32_t *>(p)};

	std::vector<uint8_t> prog, samples, dsb_prog, dsb_mpeg;
	int dsb_type = 0;
	std::unique_ptr<model3_board> board;
	std::unique_ptr<model3_dsb> dsb;
	std::vector<int32_t> dsb_q(2 * CHUNK);
	dsplink_shm link;
	// ARCSND_M3_WORDIO=1: the records and commands written a word at a time, as before DESIGN.md §19, to compare
	link.word_io = getenv("ARCSND_M3_WORDIO") != nullptr;
	// ARCSND_M3_FULL=1: every record written whole in bursts, as 0a3cc7cb did (§19.9), to compare
	link.full_io = getenv("ARCSND_M3_FULL") != nullptr;
	bool measured = false;
	link.clock = now_ns;
	std::vector<int32_t> out;
	bool running = false;
	uint16_t heartbeat = 0, fs16 = 0;
	uint32_t fs = 0, rd = 0, base = 0, applied = 0, late = 0, epoch = 0, seen = 0;
	bool fs_moved = false;
	int64_t fs_t = now_ns();
	core_name cname = read_core_name();
	int64_t name_t = now_ns(), rep_t = now_ns(), rep_cpu = cpu_ns(), busy_ns = 0, rep_io = 0;
	uint64_t rep_tx = 0;
	int32_t most_behind = 0;                    // the most samples due at a chunk's start, since the last status line
	int32_t midi_late = 0;                      // the most samples a MIDI byte went in after its stamp + MIDI_LAT, the same span

	auto publish = [&] {
		const uint32_t n = running ? uint32_t(board->samples()) + base : 0;
		sh.write32(OFF_STATUS, uint32_t(heartbeat++) << 16 | 0xA500u | (running ? 1u : 0u));
		sh.write32(OFF_STATUS + 4, n);
		sh.write32(OFF_STATS, late);
		sh.write32(OFF_STATS + 4, applied);
	};
	// the block is still this core's bridge's: nothing is written into it otherwise
	auto bridge_there = [&] {
		const uint64_t hb = sh.read64(OFF_HDR);
		return ((hb >> 8) & 0xFF) == 0x4D && ((hb >> 4) & 0xF) == 2;
	};
	auto stop = [&](const char *why) {
		if (!running) return;
		running = false;
		// every record's tag to the bridge's own clear word (the epoch's complement): a record left behind
		// would otherwise match the bridge's 16-bit p again 65,536 samples on (sooner, as it skips ahead
		// with no server), and take its stale count's commands into the engine (DESIGN.md §19.5)
		// only while the bridge is there and still in this session's epoch (a reset's own clear covers the rest);
		// epoch ^ 3 never equals the epoch
		const bool same = bridge_there() && (uint32_t(sh.read64(OFF_HDR)) & 3) == epoch;
		for (uint32_t k = 0; same && k < dsplink_shm::REC_RING; k++)
		{
			volatile uint32_t *r = sh.w + OFF_REC / 4 + dsplink_shm::REC_STRIDE * k;
			r[73] = 0;
			r[72] = ((epoch ^ 3u) & 3u) << 16;
		}
		SCSP_SetDSPLink(nullptr);
		link.dsb_q8 = nullptr;
		dsb.reset();
		board.reset();
		fprintf(stderr, "arcsnd model3: %s\n", why);
	};
	// the sound ROMs, from the set's zip, now: the core is still being loaded
	{
		std::string why;
		if (!model3_sound_roms(cname.name.c_str(), prog, samples, why) ||
		    !model3_dsb_roms(cname.name.c_str(), dsb_type, dsb_prog, dsb_mpeg, why))
		{
			fprintf(stderr, "arcsnd model3: %s: %s; no sound for this set\n", cname.name.c_str(), why.c_str());
			unmap(p, SHM_SIZE);
			while (!g_stop)
			{
				const core_name c = read_core_name();
				if (c.written != cname.written || !model3_claims(c.name.c_str())) return;
				sleep_ns(250'000'000);
			}
			return;
		}
	}

	while (!g_stop)
	{
		if (now_ns() - name_t >= 250'000'000)
		{
			name_t = now_ns();
			const core_name c = read_core_name();
			if (c.written != cname.written || !model3_claims(c.name.c_str())) break;
		}
		const uint64_t h = sh.read64(OFF_HDR);
		if (((h >> 8) & 0xFF) != 0x4D || ((h >> 4) & 0xF) != 2)
		{
			stop("the bridge's header is gone");
			sleep_ns(100'000'000);
			continue;
		}
		// fs, widened; the bridge is alive while it moves
		const uint16_t f16 = uint16_t(h >> 32);
		if (f16 != fs16) { fs += uint16_t(f16 - fs16); fs16 = f16; fs_t = now_ns(); fs_moved = seen++ > 0; }
		else if (now_ns() - fs_t >= 200'000'000)
		{
			stop("the core stopped");
			fs_moved = false; seen = 0;
			sleep_ns(20'000'000);
			continue;
		}
		if (running && (uint32_t(h) & 3) != epoch) stop("the core was reset");

		if (!running && fs_moved && !measured)
		{
			measure_stores(sh.w);                  // a few ms, before the session takes its base from the header
			measured = true;
			continue;
		}
		if (!running && fs_moved)
		{
			board = std::make_unique<model3_board>(prog.data(), samples.data());
			if (dsb_type) dsb = model3_dsb::make(dsb_type, dsb_prog.data(), dsb_mpeg.data());
			// the board's sample 0 is the FPGA's sample now; the ring from its current count
			epoch = uint32_t(h) & 3;
			base = fs;
			// the MIDI ring from its oldest entry (the CPU's first bytes come before the session)
			{
				const uint32_t cnt = uint32_t(h >> 48) & 0x3FF;
				rd = cnt >= RING ? (cnt - RING) & 0x3FF : 0;
			}
			link.rec = sh.w + OFF_REC / 4;
			link.cmd = sh.w + OFF_CMD / 4;
			link.start(base, epoch, uint32_t(h >> 16) & 0xFFFF);
			SCSP_SetDSPLink(&link);                // the DSP's state and ring windows go over now
			link.flush();
			board->link_ram(true);
			late = applied = 0;
			rep_tx = link.io_tx; rep_io = link.io_ns;
			running = true;
			fprintf(stderr, "arcsnd model3: %s started (D %u samples, epoch %u%s)\n", cname.name.c_str(), link.delay, epoch,
			        dsb_type == 1 ? ", DSB1" : dsb_type == 2 ? ", DSB2" : "");
		}
		if (!running) { sleep_ns(500'000); continue; }

		// render every sample the FPGA has passed, applying the ring's bytes on the way; the DSB's
		// music for a piece is made first, so that each record the board writes carries it
		auto render = [&](int32_t n) {
			if (dsb)
			{
				dsb->run_q8(n, dsb_q.data());
				link.dsb_q8 = dsb_q.data();
			}
			board->run(n, out);
		};
		const int64_t t0 = now_ns();
		bool behind = false;                   // the pass ended on its time bound with samples still due
		for (bool first = true;; first = false)
		{
			// fs from every chunk's look at the header, not only the loop's: a pass that ran on past
			// 65,536 samples of fs (1.5 s) would otherwise fold it and leave the session that far behind for good
			const uint64_t hc = first ? h : sh.read64(OFF_HDR);
			if (((hc >> 8) & 0xFF) != 0x4D || ((hc >> 4) & 0xF) != 2) break;
			{
				const uint16_t f = uint16_t(hc >> 32);
				if (f != fs16) { fs += uint16_t(f - fs16); fs16 = f; fs_t = now_ns(); }
			}
			const uint32_t now = uint32_t(board->samples()) + base;
			const int32_t todo = int32_t(fs - now);
			if (todo <= 0) break;
			if (todo > most_behind) most_behind = todo;
			if (!first && now_ns() - t0 >= 20'000'000) { behind = true; break; }   // the core's name and the status at least every 20 ms
			if (uint32_t(todo) > link.delay) late++;  // behind the bridge: it waits, then catches up
			const uint32_t end = now + uint32_t(todo < CHUNK ? todo : CHUNK);
			const uint16_t count = uint16_t(hc >> 48) & 0x3FF;
			if (((count - rd) & 0x3FF) > RING) rd = (count - RING) & 0x3FF;   // overrun: the oldest are lost
			while ((rd & 0x3FF) != count)
			{
				const uint64_t e = sh.read64(OFF_RING + (rd % RING) * 8);
				const uint32_t at = fs - uint16_t(fs16 - uint16_t(e >> 32)) + MIDI_LAT;   // its fs, widened
				if (int32_t(at - end) >= 0) break;      // not yet due in this chunk
				const int32_t lead = int32_t(at - (uint32_t(board->samples()) + base));
				if (-lead > midi_late) midi_late = -lead;   // 0 when every byte goes in at its own sample
				out.clear();
				if (lead > 0) render(lead);
				if (e & 0x100)
				{
					board->midi_in(uint8_t(e));
					if (dsb) dsb->send(uint8_t(e));
				}
				applied++;
				rd = (rd + 1) & 0x3FF;
			}
			out.clear();
			const int32_t rest = int32_t(end - (uint32_t(board->samples()) + base));
			if (rest > 0) render(rest);
			publish();
		}
		busy_ns += now_ns() - t0;
		if (verbose && now_ns() - rep_t >= 5'000'000'000)
		{
			const double span = double(now_ns() - rep_t);
			// the link's share: its writes to the block a second, and the sampled records' write time
			fprintf(stderr, "arcsnd model3: %s, %u bytes applied, %u late, busy %.0f%% (CPU %.0f%%), most behind %d, MIDI late %d, link %s %.0f k writes/s, records %.1f%%%s\n",
			        running ? "running" : "idle", applied, late, 100.0 * busy_ns / span, 100.0 * (cpu_ns() - rep_cpu) / span, most_behind, midi_late,
			        link.word_io ? "words" : link.full_io ? "whole records" : "changes", 1e6 * double(link.io_tx - rep_tx) / span, 100.0 * double(link.io_ns - rep_io) / span,
			        link.fallback() ? ", the DSP here (fallback)" : "");
			rep_t = now_ns(); rep_cpu = cpu_ns(); busy_ns = 0; rep_tx = link.io_tx; rep_io = link.io_ns; most_behind = 0; midi_late = 0;
		}
		if (!behind) sleep_ns(500'000);
	}
	stop("the core changed");
	if (bridge_there()) publish();
	unmap(p, SHM_SIZE);
}
