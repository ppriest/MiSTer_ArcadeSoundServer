// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboardmania's session: runs while a Firebeat Keyboardmania set is loaded, against the block
// Arcade-Firebeat_MiSTer's rtl/board/fb_midibridge.sv shares in DDR3 (its header has the layout).
// The XT446 synth module's MIDI in, the bytes the game sends it, played on a SoundFont GM synth
// (synth.h).
//
// The synth starts when the bridge's magic is there and its beat moves, and takes the ring from its
// current count. Each byte is applied at its microsecond stamp mapped onto the synth's output clock
// LAT later, never closer to the one before than the stamps were; the synth runs a millisecond at a
// time, ahead of the audio by audio_out's lead. The bridge's held flag (the board paused or in
// reset) releases every note; a new board reset count resets the synth. The mute flag (the core's
// OSD sends the notes to its MIDI port alone) releases every note and outputs silence, while the
// bytes are still applied, so the voices the game set are right when it clears. It stops (silence)
// when the beat stalls for 200 ms (the core reloaded), the magic goes, or the core changes.
//
// The SoundFont: arcsnd.ini's [firebeat_kbm] soundfont= if it loads, else games/arcsnd/firebeat.sf2,
// else the Yamaha XG Sound Set.sf2 or TimGM6mb.sf2 shipped beside arcsnd. Loaded when a set starts, freed when the core changes.

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "daemon.h"
#include "synth.h"

namespace {

using namespace arcsnd;

constexpr uint32_t SHM = 0x3F200000, SHM_SIZE = 0x2000;
constexpr uint32_t OFF_MAGIC = 0x00, OFF_COUNT = 0x10, OFF_BEAT = 0x18, OFF_STATE = 0x20, OFF_RING = 0x1000;
constexpr uint32_t RING = 512;
constexpr int64_t MS = fb_synth::RATE / 1000;           // output frames
constexpr int64_t LAT = 10 * MS;                       // a byte's delay past its stamp
constexpr const char *SF2[] = {"firebeat.sf2", "Yamaha XG Sound Set.sf2", "TimGM6mb.sf2"};

const char *const SETS[] = {"kbm", "kbh", "kbm2nd", "kbm3rd"};

// a stamp in output frames: us x 48 / 1000, wrapping with the stamp
int64_t frames_of(uint32_t us) { return int64_t(us) * fb_synth::RATE / 1000000; }

}  // namespace

void firebeat_sets(std::vector<const char *> &out)
{
	for (const char *s : SETS) out.push_back(s);
}

bool firebeat_claims(const char *name)
{
	for (const char *s : SETS)
		if (!strcmp(name, s)) return true;
	return false;
}

// runs until the core changes or the process is stopped; mem is /dev/mem
void firebeat_run(int mem, bool verbose)
{
	void *p = map(mem, SHM, SHM_SIZE);
	if (!p) { perror("mmap firebeat block"); sleep_ns(1'000'000'000); return; }
	shared sh{static_cast<volatile uint32_t *>(p)};

	fb_synth synth;
	std::vector<std::string> tries;
	const std::string ini = ini_get("firebeat_kbm", "soundfont");
	if (!ini.empty()) tries.push_back(ini_path(ini));
	for (const char *s : SF2) tries.push_back(ini_path(s));
	std::string sf2;
	for (const std::string &s : tries)
	{
		if (access(s.c_str(), R_OK) == 0 && synth.load(s.c_str())) { sf2 = s; break; }
		if (!ini.empty() && &s == &tries[0]) fprintf(stderr, "arcsnd firebeat: cannot load %s\n", s.c_str());
	}
	if (sf2.empty())
	{
		fprintf(stderr, "arcsnd firebeat: no SoundFont (%s, %s or %s in %s)\n", SF2[0], SF2[1], SF2[2], ARCSND_DIR);
		unmap(p, SHM_SIZE);
		// silent until the core changes
		const core_name c0 = read_core_name();
		while (!g_stop)
		{
			const core_name c = read_core_name();
			if (c.written != c0.written || !firebeat_claims(c.name.c_str())) return;
			sleep_ns(250'000'000);
		}
		return;
	}
	fprintf(stderr, "arcsnd firebeat: SoundFont %s\n", sf2.c_str());

	audio_out audio;
	std::vector<int16_t> pcm;
	bool running = false, held = false, muted = false;
	uint32_t beat_seen = 0, rd = 0, resets = 0;
	int64_t beat_t = now_ns();
	bool mapped = false;
	int64_t pos = 0, offset = 0, last_at = 0;              // output frames
	uint32_t last_us = 0;
	core_name cname = read_core_name();
	int64_t name_t = now_ns();
	int64_t rep_t = now_ns(), busy_ns = 0, rep_cpu = cpu_ns();
	uint32_t rep_bytes = 0;

	auto stop = [&](const char *why) {
		if (!running) return;
		running = false;
		synth.reset();
		audio.fade();
		audio.close_dev();
		fprintf(stderr, "arcsnd firebeat: %s\n", why);
	};

	while (!g_stop)
	{
		if (now_ns() - name_t >= 250'000'000)
		{
			name_t = now_ns();
			const core_name c = read_core_name();
			if (c.written != cname.written || !firebeat_claims(c.name.c_str())) break;
		}
		if (uint32_t(sh.read64(OFF_MAGIC)) != 0x494D4246u)     // "FBMI"
		{
			stop("the bridge's magic is gone");
			sleep_ns(100'000'000);
			continue;
		}
		const uint32_t b = uint32_t(sh.read64(OFF_BEAT));
		if (b != beat_seen) { beat_seen = b; beat_t = now_ns(); }
		else if (now_ns() - beat_t >= 200'000'000)
		{
			stop("the core stopped");
			sleep_ns(20'000'000);
			continue;
		}
		const uint64_t st = sh.read64(OFF_STATE);
		const bool held_now = st & 1;
		const uint32_t resets_now = uint32_t(st >> 32) & 0xFFFF;

		if (!running)
		{
			synth.reset();
			rd = uint32_t(sh.read64(OFF_COUNT));
			resets = resets_now;
			held = held_now;
			mapped = false;
			pos = 0;
			audio.open_dev();
			running = true;
			fprintf(stderr, "arcsnd firebeat: started at entry %u\n", rd);
		}
		if (resets_now != resets)
		{
			// the board was reset: the game sends its setup again; the ring starts afresh
			resets = resets_now;
			synth.reset();
			rd = uint32_t(sh.read64(OFF_COUNT));
			mapped = false;
		}
		if (held_now && !held) synth.release_all();
		held = held_now;
		const bool muted_now = st & 2;
		if (muted_now != muted) {
			if (muted_now) {
				synth.release_all();
			}
			fprintf(stderr, "arcsnd firebeat: %s\n", muted_now ? "muted: the notes go to the core's MIDI port" : "unmuted");
		}
		muted = muted_now;

		audio.drain(running);
		const int64_t loop_end = now_ns() + 20'000'000;
		while (running && audio.wants_more() && !g_stop && now_ns() < loop_end)
		{
			const int64_t end = pos + MS;
			const int64_t t0 = now_ns();
			pcm.clear();
			uint32_t count = uint32_t(sh.read64(OFF_COUNT));
			if (count - rd > RING) rd = count - RING;         // overrun: the oldest are lost
			while (rd != count)
			{
				const uint64_t e = sh.read64(OFF_RING + (rd % RING) * 8);
				const uint32_t us = uint32_t(e);
				int64_t at = frames_of(us) + offset;
				const int32_t dus = int32_t(us - last_us);
				if (!mapped || dus < 0 || at < pos - 50 * MS || at > pos + 100 * MS)
				{
					// first byte, or the clocks have drifted apart: this stamp LAT ahead
					// last_us follows, or a stamp behind the last session's remaps every chunk and never plays
					offset = pos + LAT - frames_of(us);
					at = pos + LAT;
					mapped = true;
					last_us = us; last_at = at;
				}
				else at = std::max(at, last_at + frames_of(uint32_t(dus)));
				if (at >= end) break;
				if (at > pos) { synth.render(int(at - pos), pcm); pos = at; }
				synth.byte(uint8_t(e >> 40));
				last_at = at; last_us = us;
				rd++;
				rep_bytes++;
			}
			synth.render(int(end - pos), pcm);
			pos = end;
			busy_ns += now_ns() - t0;
			if (muted) {
				std::fill(pcm.begin(), pcm.end(), int16_t(0));
			}
			audio.put(pcm, false);
			audio.drain(running);
		}
		if (verbose && now_ns() - rep_t >= 5'000'000'000)
		{
			const double span = double(now_ns() - rep_t);
			fprintf(stderr, "arcsnd firebeat: %s%s, %u bytes, %u notes (%u dropped), %u SysEx, %d voices, "
			        "busy %.0f%% (CPU %.0f%%), %u gaps (%.0f ms)\n",
			        running ? "running" : "idle", held ? " held" : "", rep_bytes, synth.notes, synth.dropped,
			        synth.sysex, synth.voices(), 100.0 * busy_ns / span, 100.0 * (cpu_ns() - rep_cpu) / span,
			        audio.gaps, audio.gap_ns / 1e6);
			rep_t = now_ns(); rep_cpu = cpu_ns(); busy_ns = 0; rep_bytes = 0;
			synth.notes = synth.dropped = synth.sysex = 0;
			audio.gaps = 0; audio.gap_ns = 0;
		}
		if (!running || !audio.wants_more()) sleep_ns(1'000'000);
	}
	stop("the core changed");
	unmap(p, SHM_SIZE);
}
