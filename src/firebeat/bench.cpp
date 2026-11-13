// SPDX-License-Identifier: GPL-3.0-or-later
// The Keyboardmania synth against a MAME capture: chan1's transmitted bytes (the XT446's MIDI in)
// played through fb_synth into a WAV, for the ear, with the parser's counts.
//
//   firebeat_bench <midi.txt> <soundfont.sf2> <out.wav>
//
// midi.txt is Arcade-Firebeat_MiSTer's MIDI access list ("frame R|W address data", the UART
// channels at 0x70000x00; debug/kbm-play/midi.txt). Bytes written to chan1's THR (DLAB clear) are
// the MIDI out. The capture has frame resolution: a frame's bytes are spaced a byte time (320 us at
// 31250 baud) from the frame's start, frames 1/58.32 s apart.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "synth.h"

int main(int argc, char **argv)
{
	if (argc != 4) { fprintf(stderr, "usage: firebeat_bench <midi.txt> <soundfont.sf2> <out.wav>\n"); return 2; }
	std::ifstream in(argv[1]);
	if (!in) { perror(argv[1]); return 1; }
	struct ev { double us; uint8_t b; };
	std::vector<ev> evs;
	long fr, last_fr = -1;
	std::string rw;
	unsigned ad, d;
	bool dlab = false;
	double next = 0;
	unsigned direct = 0;
	while (in >> std::dec >> fr >> rw >> std::hex >> ad >> d)
	{
		if (rw != "W" || (ad >> 11 & 1) != 1) continue;
		const int reg = ad >> 8 & 7;
		const uint8_t v = d >> 24;
		if (reg == 3) dlab = v >> 7;
		if (reg != 0 || dlab) continue;
		const double t = fr * 1e6 / 58.32;
		if (fr != last_fr) { next = std::max(next, t); last_fr = fr; }
		evs.push_back({next, v});
		next += 320.0;
		if ((v & 0xF0) == 0x90) direct++;
	}
	fb_synth s;
	if (!s.load(argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
	std::vector<int16_t> pcm;
	double pos_us = evs.empty() ? 0 : evs.front().us;
	const double t0 = pos_us;
	int maxv = 0;
	for (const ev &e : evs)
	{
		const int frames = int((e.us - pos_us) * fb_synth::RATE / 1e6);
		if (frames > 0) { s.render(frames, pcm); pos_us += frames * 1e6 / fb_synth::RATE; }
		s.byte(e.b);
		maxv = std::max(maxv, s.voices());
	}
	s.render(fb_synth::RATE * 2, pcm);
	int peak = 0;
	for (int16_t x : pcm) peak = std::max(peak, std::abs(int(x)));

	FILE *w = fopen(argv[3], "wb");
	if (!w) { perror(argv[3]); return 1; }
	auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, w); };
	auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, w); };
	const uint32_t bytes = uint32_t(pcm.size() * 2);
	fwrite("RIFF", 1, 4, w); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, w);
	u32(16); u16(1); u16(2); u32(fb_synth::RATE); u32(fb_synth::RATE * 4); u16(4); u16(16);
	fwrite("data", 1, 4, w); u32(bytes);
	fwrite(pcm.data(), 2, pcm.size(), w);
	fclose(w);
	printf("%zu bytes over %.1f s: %u notes on (%u note-on status bytes), %u dropped, %u SysEx; "
	       "most voices %d, peak %d; %s\n", evs.size(), (pos_us - t0) / 1e6, s.notes, direct, s.dropped, s.sysex,
	       maxv, peak, argv[3]);
	return 0;
}
