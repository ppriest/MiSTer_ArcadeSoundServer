// SPDX-License-Identifier: GPL-3.0-or-later
// The Sega Digital Sound Board (MPEG audio), from Supermodel's Src/Model3/DSB.cpp and
// Src/Sound/MPEG/MpegAudio.cpp (GPL-3.0-or-later, the Supermodel Team; DSB from code by R. Belmont),
// PROVENANCE.md:
//   DSB1 (Scud Race): a Z80 at 4 MHz (Supermodel's, from Frank D. Cringle's), 32 KB of program
//     ROM read and 32 KB of RAM, the MPEG chip's registers on its I/O ports;
//   DSB2 (Daytona 2): a 68000 at 11 MHz (a second Musashi context: Musashi keeps one CPU in globals,
//     so the board's is saved and this one put in for each run, as Supermodel does), 128 KB of
//     program ROM, 128 KB of RAM, the MPEG chip's command FIFO at 0xE00003, a 1 kHz timer on IRQ 2.
// Both hear every byte the PowerPC sends to the sound board (Supermodel's WriteMIDIPort). The MPEG
// stream (Layer 2, 32 kHz in every set) is decoded with minimp3 (CC0, Supermodel's copy) and
// resampled to 44.1 kHz by Supermodel's linear interpolator, then added to the caller's two float
// channels as Supermodel's CDSB::RunFrame adds it to the front pair, at MusicVolume 100.
//
// run(n) is Supermodel's RunFrame for n samples: the commands received since the last run are
// taken at its start, the CPU then runs n samples' worth of cycles, and the MPEG is decoded for the
// n samples. With n the length of each video frame it is Supermodel's DSB sample for sample.
#pragma once

#include <cstdint>
#include <memory>

class model3_dsb
{
public:
	static constexpr uint32_t PROG_BYTES = 0x20000, MPEG_BYTES = 0x1000000;
	static constexpr int MAX_RUN = 44100 / 45 + 1;      // Supermodel's MAX_SAMPLES_PER_FRAME
	// type 1 or 2; prog: 128 KB as Supermodel holds it (DSB2: each 68000 word little-endian),
	// mpeg: 16 MB. Both must outlive the board. Reset as Supermodel's CDSBx::Reset.
	static std::unique_ptr<model3_dsb> make(int type, const uint8_t *prog, const uint8_t *mpeg);
	virtual ~model3_dsb() = default;
	virtual int type() const = 0;
	// a byte of the PowerPC's sound port (CDSB::SendCommand)
	virtual void send(uint8_t b) = 0;
	// n samples (1 .. MAX_RUN) at 44.1 kHz, added into l and r (CDSB::RunFrame)
	virtual void run(int n, float *l, float *r) = 0;
	virtual void reset() = 0;
	// whether an MPEG stream is loaded and not stopped (for the bench's report)
	virtual bool playing() const = 0;
	// the CPU's wait-loop turns counted rather than run (DSB2's spin skip, dsb.cpp), for the bench
	virtual uint64_t spin_turns() const { return 0; }
	// the CPU's whole state (registers, cycle counts, RAM) folded into h, for the bench's check that
	// the spin skip leaves every run where it would be
	virtual uint32_t state_hash(uint32_t h) const { return h; }

	// a sample of the music as the FPGA takes it: 1/256ths, towards zero, in 24 bits. Added to the
	// SCSP's integer channel and the sum taken towards zero, it gives Supermodel's float mix
	// (float(scsp) + music, converted towards zero) but in 0.12% of samples, by one (16 bits alone:
	// 29%)
	static int32_t q8(float x)
	{
		const int32_t v = int32_t(x * 256.0f);
		return v > 0x7FFFFF ? 0x7FFFFF : v < -0x7FFFFF ? -0x7FFFFF : v;
	}
	// n samples (1 .. MAX_RUN) as q8, {left, right} a sample
	void run_q8(int n, int32_t *lr)
	{
		float l[MAX_RUN], r[MAX_RUN];
		for (int i = 0; i < n; i++) l[i] = r[i] = 0.0f;
		run(n, l, r);
		for (int i = 0; i < n; i++) { lr[2 * i] = q8(l[i]); lr[2 * i + 1] = q8(r[i]); }
	}
};
