// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboardmania's XT446 stood in for by a SoundFont GM synth (TinySoundFont, tsf.h): the MIDI
// bytes the game sends are parsed as a MIDI in port would and played at 48 kHz.
//
// What is followed: notes, program changes, bank select as XG uses it (MSB 127 a drum kit, MSB 0
// a normal voice whose LSB variation falls back to the GM voice), volume, expression, pan,
// sustain, pitch bend and its range (RPN 0), all notes / sounds off, reset controllers; GM On and
// XG System On reset every part. Not followed: XG SysEx parameter changes and NRPN part edits
// (filter, envelope, effects sends), aftertouch, the effects themselves; tsf has no reverb or
// chorus.
#pragma once

#include <cstdint>
#include <vector>

struct tsf;

class fb_synth
{
public:
	static constexpr int RATE = 48000;

	// false when the SoundFont cannot be read. The file is mapped, not copied: its samples are read
	// where they lie, held in memory (locked on Linux) until the synth goes.
	bool load(const char *sf2_path);
	~fb_synth();

	// the parser and every part to power-on, sounding voices cut
	void reset();
	// every sounding note released (the board paused or held in reset)
	void release_all();
	void byte(uint8_t b);
	// frames of stereo s16 appended to out
	void render(int frames, std::vector<int16_t> &out);
	int voices() const;

	uint32_t notes = 0, sysex = 0, dropped = 0;    // counts, for the status line

private:
	void message();
	void part_reset(int ch);
	void program(int ch);

	tsf *f = nullptr;
	void *base = nullptr;                         // the mapped SoundFont
	size_t size = 0;
	uint8_t status = 0;                            // running status, 0 none
	uint8_t data[2] = {};
	int have = 0;
	bool in_sysex = false;
	std::vector<uint8_t> sx;
	uint8_t bank_msb[16] = {}, prog[16] = {};
};
