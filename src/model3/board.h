// SPDX-License-Identifier: GPL-3.0-or-later
// The Sega Model 3 sound board: a 68000 (Musashi) at 11.2896 MHz, two SCSPs (master and slave, each
// with 1 MB of sound RAM), the sample-bank latch at 0x400001, the program ROM at 0x600000 and the
// sample ROM at 0x800000. From Supermodel's Src/Model3/SoundBoard.cpp and Src/Sound/SCSP*.cpp
// (GPL-3.0-or-later, the Supermodel Team; SCSP by ElSemi, R. Belmont and Paul Prosser), PROVENANCE.md.
//
// Time is SCSP samples at 44.1 kHz (45.1584 MHz / 1024); the 68000 runs 256 of its clocks a sample,
// interleaved with the SCSPs sample by sample (Supermodel's SCSP_DoMasterSamples). The main CPU's
// side is one byte port: each MIDI byte the PowerPC writes to 0xF0080000 goes through the
// uPD71051 to the master SCSP's MIDI input.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "m68k_bus.h"

class model3_board : public m68k_bus
{
public:
	static constexpr int SAMPLE_RATE = 44100;
	static constexpr uint32_t PROG_BYTES = 0x80000;
	static constexpr uint32_t SAMPLE_BYTES = 0x1000000;

	// prog: 512 KB, samples: 16 MB (an 8 MB set mirrored into the top half), both as the set's
	// files hold them (each 68000 word little-endian). timer_mode: SCSP_SetTimerMode.
	model3_board(const uint8_t *prog, const uint8_t *samples, int timer_mode = 1);
	~model3_board();

	void midi_in(uint8_t b);
	// with the DSP outside (SCSP_SetDSPLink): sound RAM writes go through write8 / write16, which
	// hand the link every word written
	void link_ram(bool on);
	// runs n samples; their four channels (master L, R, slave L, R) are appended to out
	void run(int n, std::vector<int32_t> &out);
	uint64_t samples() const { return m_count; }

	// the bench's view of the 68000's accesses to the master SCSP's common registers:
	// (sample, 'R'/'W', register offset 0x400-0x42f, data)
	std::function<void(uint64_t, char, uint32_t, uint32_t)> m_log;

	uint32_t read8(uint32_t a) override;
	uint32_t read16(uint32_t a) override;
	void write8(uint32_t a, uint32_t d) override;
	void write16(uint32_t a, uint32_t d) override;

private:
	void update_banks();

	std::unique_ptr<uint8_t[]> m_ram1, m_ram2;
	const uint8_t *m_prog, *m_smp, *m_bank;
	uint8_t m_ctrl = 0;
	uint64_t m_count = 0;
	std::vector<float> m_buf[4];
};
