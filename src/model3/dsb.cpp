// SPDX-License-Identifier: GPL-3.0-or-later
// The Digital Sound Board (dsb.h, PROVENANCE.md). Ported from Supermodel 036af1c's Src/Model3/DSB.cpp
// (CDSBResampler, CDSB1, CDSB2) and Src/Sound/MPEG/MpegAudio.cpp (the MpegDec player without its
// custom tracks), GPL-3.0-or-later, the Supermodel Team. What changed:
//   - MpegDec's one global decoder is a member of each board (one board runs at a time, as in
//     Supermodel);
//   - the Z80 is Supermodel's CZ80 through an IBus (sm_bus.h); the 68000 is arcsnd's Musashi
//     through m68k_bus, its context switched in for each run;
//   - MusicVolume is fixed at 100 and EmulateDSB on, Supermodel's defaults (the capture's settings);
//   - no save states and no debugger.
// The arithmetic (the resampler's 16.16 steps, the float volume products, the per-call cycle
// counts) is Supermodel's, operation for operation, so a run frame by frame is its output.

#include "dsb.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "m68k_bus.h"
#include "sm_bus.h"
#include "sm_shim.h"
#include "z80/Z80.h"

extern "C" {
#include "../hornet/musashi/m68k.h"
int dsb_m68k_runs_plain(void);         // dsb_m68k.c
}

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NEON_AS_SSE        // the ARM build's output the x86 one's (minimp3.h, arcsnd's change)
#include "minimp3/minimp3.h"

namespace {

// Supermodel's DSB.h
constexpr int FIFO_STACK_SIZE = 0x100, FIFO_STACK_SIZE_MASK = FIFO_STACK_SIZE - 1;
#define MPEG_SAMPLES_FOR(numSamples)  (((numSamples) * 32000 + 44099) / 44100 + 2)
constexpr int MPEG_BUF = MPEG_SAMPLES_FOR(model3_dsb::MAX_RUN);

// ------------------------------------------------------------------ MpegAudio.cpp's player
struct mpeg_player
{
	mp3dec_t			mp3d;
	mp3dec_frame_info_t	info;
	const uint8_t*		buffer = nullptr;
	int					size = 0, pos = 0;
	bool				loop = false;
	bool				stopped = false;
	int					numSamples = 0;
	int					pcmPos = 0;
	short				pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

	mpeg_player() { memset(&mp3d, 0, sizeof mp3d); memset(&info, 0, sizeof info); memset(pcm, 0, sizeof pcm); }

	void SetMemory(const uint8_t *data, int offset, int length, bool lp)
	{
		mp3dec_init(&mp3d);
		buffer = data + offset;
		size = length;
		pos = 0;
		numSamples = 0;
		pcmPos = 0;
		loop = lp;
		stopped = false;
	}

	void UpdateMemory(const uint8_t *data, int offset, int length, bool lp)
	{
		int diff;
		if ((data + offset) > buffer)
			diff = (int)(data + offset - buffer);
		else
			diff = -(int)(buffer - data - offset);
		buffer = data + offset;
		size = length;
		pos = pos - diff;	// update position relative to our new start location
		loop = lp;
	}

	int GetPosition() const { return pos; }
	void Stop() { stopped = true; }
	bool IsLoaded() const { return buffer != nullptr; }

	void FlushBuffer(int16_t *&left, int16_t *&right, int &numStereoSamples)
	{
		const int numChans = info.channels;
		int &i = pcmPos;
		for (; i < (numSamples * numChans) && numStereoSamples; i += numChans)
		{
			*left++ = pcm[i];
			*right++ = pcm[i + numChans - 1];
			numStereoSamples--;
		}
	}

	static void EndWithSilence(int16_t *&left, int16_t *&right, int &numStereoSamples)
	{
		while (numStereoSamples)
		{
			*left++ = 0;
			*right++ = 0;
			numStereoSamples--;
		}
	}

	bool EndOfBuffer() const { return pos >= size - HDR_SIZE; }

	void DecodeAudio(int16_t *left, int16_t *right, int numStereoSamples)
	{
		// if we are stopped return silence
		if (stopped || !buffer)
			EndWithSilence(left, right, numStereoSamples);

		// copy any left over samples first
		FlushBuffer(left, right, numStereoSamples);

		while (numStereoSamples)
		{
			numSamples = mp3dec_decode_frame(&mp3d, buffer + pos, size - pos, pcm, &info);
			pos += info.frame_bytes;
			pcmPos = 0;	// reset pos

			FlushBuffer(left, right, numStereoSamples);

			// check end of buffer handling
			if (EndOfBuffer())
			{
				if (loop)
					pos = 0;
				else
					EndWithSilence(left, right, numStereoSamples);
			}
		}
	}
};

// ------------------------------------------------------------------ DSB.cpp's resampler
struct resampler
{
	int nFrac = 0, pFrac = 1 << 16;

	void Reset()
	{
		nFrac = 0<<16;	// fraction of next sample to use (0->1.0 as x moves p->n)
		pFrac = 1<<16;	// previous sample (1.0->0 as x moves p->n)
	}

	// Mixes audio and returns number of samples copied back to start of buffer
	int UpSampleAndMix(float *outL, float *outR, INT16 *inL, INT16 *inR, UINT8 volumeL, UINT8 volumeR, int sizeOut, int sizeIn, int outRate, int inRate)
	{
		int 	delta = (int)(((INT64)inRate<<16)/outRate);	// (1/fout)/(1/fin)=fin/fout, 16.16 fixed point
		int		outIdx = 0;
		int		inIdx = 0;
		float	v[2], musicVol;

		// Supermodel's MusicVolume setting, at its default of 100
		musicVol = (float)std::max(0,std::min(200,100));
		musicVol = musicVol * (float) (1.0 / 100.0);

		v[0] = musicVol * (float) volumeL * (float) (1.0 / (255.0*65536.0)); // 65536 is there to correct for fixed point interpolation below
		v[1] = musicVol * (float) volumeR * (float) (1.0 / (255.0*65536.0));

		while (outIdx < sizeOut)
		{
			// nFrac, pFrac will never exceed 1.0 (0x10000) (only true if delta does not exceed 1)
			INT64 leftSample  = (INT64)inL[inIdx]*pFrac+(INT64)inL[inIdx+1]*nFrac;	// left channel
			INT64 rightSample = (INT64)inR[inIdx]*pFrac+(INT64)inR[inIdx+1]*nFrac;	// right channel

			outL[outIdx] += (float)leftSample  * v[0];
			outR[outIdx] += (float)rightSample * v[1];
			outIdx++;

			pFrac -= delta;
			nFrac += delta;

			if (pFrac <= 0)	// when pFrac becomes 0, advance samples, reset pFrac to 1
			{
				pFrac += (1<<16);
				nFrac -= (1<<16);
				inIdx++;
			}
		}

		// Copy remaining "active" input samples to start of buffer
		int i = 0;
		int j = inIdx;
		while (j < sizeIn)
		{
			inL[i] = inL[j];
			inR[i] = inR[j];
			i++;
			j++;
		}
		return i;	// first free position in input buffer to copy next MPEG update to
	}
};

// ------------------------------------------------------------------ DSB1: Z80
class dsb1 final : public model3_dsb, public IBus
{
public:
	dsb1(const uint8_t *prog, const uint8_t *mpeg) : progROM(prog), mpegROM(mpeg), ram(0x8000, 0)
	{
		Z80.Init(this, Z80IRQCallback);
		reset();
		// the delay loop (epr-19612 at 0x4CC: LD B,$10; EX (SP),HL; EX (SP),HL; DJNZ), recognised by its bytes
		for (UINT32 a = 0; a + 3 < 0x8000; a++)
			if (prog[a] == 0xE3 && prog[a + 1] == 0xE3 && prog[a + 2] == 0x10 && prog[a + 3] == 0xFC) { Z80.spinPC = UINT16(a); break; }
	}
	uint64_t spin_turns() const override { return Z80.spinTurns; }
	uint32_t state_hash(uint32_t h) const override
	{
		h = Z80.StateHash(h);
		for (UINT8 b : ram) h = (h ^ b) * 16777619u;
		const int v[] = {fifoIdxR, fifoIdxW, status, mpegState, int(volume), int(stereo), mpegStart, mpegEnd, loopStart, loopEnd};
		for (int x : v) for (int i = 0; i < 4; i++) h = (h ^ ((x >> (8 * i)) & 0xFF)) * 16777619u;
		return h;
	}
	int type() const override { return 1; }
	bool playing() const override { return dec.IsLoaded() && !dec.stopped; }

	UINT8 Read8(UINT32 addr) override
	{
		if (addr < 0x8000)
			return progROM[addr];
		return ram[addr&0x7FFF];
	}

	void Write8(UINT32 addr, UINT8 data) override
	{
		if (addr >= 0x8000)
			ram[addr&0x7FFF] = data;
	}

	void IOWrite8(UINT32 addr, UINT8 data) override
	{
		switch ((addr&0xFF))
		{
		case 0xE0:	// MPEG trigger
			mpegState = data;
			if (data == 0)	// stop
			{
				dec.Stop();
				return;
			}
			if (data == 1)	// play without loop
			{
				usingLoopStart	= 0;
				usingLoopEnd	= 0;
				usingMPEGStart	= mpegStart;
				usingMPEGEnd	= mpegEnd;
				dec.SetMemory(mpegROM, mpegStart, mpegEnd - mpegStart, false);
				return;
			}
			if (data == 2)	// play with loop
			{
				usingLoopStart	= 0;
				usingLoopEnd	= 0;
				usingMPEGStart	= mpegStart;
				usingMPEGEnd	= mpegEnd;
				dec.SetMemory(mpegROM, mpegStart, mpegEnd - mpegStart, false);
				return;
			}
			break;
		case 0xE2: startLatch &= 0x00FFFF; startLatch |= ((UINT32)data) << 16; break;
		case 0xE3: startLatch &= 0xFF00FF; startLatch |= ((UINT32)data) << 8; break;
		case 0xE4:	// MPEG start, low byte
			startLatch &= 0xFFFF00;
			startLatch |= data;
			if (mpegState == 0)
				mpegStart = startLatch;
			else
			{
				loopStart = startLatch;
				// SWA: if loop end is zero, it means "keep previous end marker"
				if (loopEnd == 0)
				{
					usingLoopStart	= loopStart;
					usingLoopEnd	= mpegEnd-loopStart;
					dec.UpdateMemory(mpegROM, usingLoopStart, usingLoopEnd, true);
				}
				else
				{
					usingLoopStart	= loopStart;
					usingLoopEnd	= loopEnd-loopStart;
					dec.UpdateMemory(mpegROM, usingLoopStart, usingLoopEnd, true);
				}
			}
			break;
		case 0xE5: endLatch &= 0x00FFFF; endLatch |= ((UINT32)data) << 16; break;
		case 0xE6: endLatch &= 0xFF00FF; endLatch |= ((UINT32)data) << 8; break;
		case 0xE7:	// MPEG end, low byte
			endLatch &= 0xFFFF00;
			endLatch |= data;
			if (mpegState == 0)
				mpegEnd = endLatch;
			else
			{
				loopEnd			= endLatch;
				usingLoopStart	= loopStart;
				usingLoopEnd	= loopEnd-loopStart;
				dec.UpdateMemory(mpegROM, usingLoopStart, usingLoopEnd, true);
			}
			break;
		case 0xE8: volume = 0x7F-data; break;	// MPEG volume
		case 0xE9: stereo = data; break;		// MPEG stereo
		case 0xF0: break;						// command echo back
		default: break;
		}
	}

	UINT8 IORead8(UINT32 addr) override
	{
		int	progress;
		switch ((addr&0xFF))
		{
		case 0xE2:	// MPEG position, high byte
			progress = dec.GetPosition();
			progress += mpegStart;
			return (progress>>16)&0xFF;
		case 0xE3:
			progress = dec.GetPosition();
			progress += mpegStart;
			return (progress>>8)&0xFF;
		case 0xE4:
			progress = dec.GetPosition();
			progress += mpegStart;
			return progress&0xFF;
		case 0xF0:	// Latch
		{
			UINT8 d = fifo[fifoIdxR];	// retrieve next command byte
			if (fifoIdxR != fifoIdxW)	// if these are equal, nothing has been written yet (don't advance)
			{
				fifoIdxR++;
				fifoIdxR &= FIFO_STACK_SIZE_MASK;
			}
			if (fifoIdxR == fifoIdxW)	// FIFO empty?
				status &= ~2;			// yes, indicate no commands left
			else
				status |= 2;
			Z80.SetINT(false);	// clear IRQ
			return d;
		}
		case 0xF1:	// Status: bit 0 must be 1 for most games; bit 1 command pending
			return status;
		}
		return 0;
	}

	void send(uint8_t data) override
	{
		fifo[fifoIdxW++] = data;
		fifoIdxW &= FIFO_STACK_SIZE_MASK;
	}

	void run(int numSamples, float *audioL, float *audioR) override
	{
		int		cycles;
		// While FIFO not empty, fire interrupts, run for up to one frame (Z80 at 4 MHz)
		for (cycles = (int)((4000000LL * numSamples) / 44100); (cycles > 0) && (fifoIdxR != fifoIdxW);  )
		{
			Z80.SetINT(true);	// fire an IRQ to indicate pending command
			cycles -= Z80.Run(500);
		}

		// Run remaining cycles
		Z80.Run(cycles);

		// Convert volume from 0x00-0x7F -> 0x00-0xFF
		UINT8 v = (UINT8) ((float) volume * (float)(255.0/127.0));

		// Decode MPEG for this frame
		dec.DecodeAudio(&mpegL[retainedSamples], &mpegR[retainedSamples], MPEG_SAMPLES_FOR(numSamples) - retainedSamples);
		retainedSamples = Resampler.UpSampleAndMix(audioL, audioR, mpegL, mpegR, v, v, numSamples, MPEG_SAMPLES_FOR(numSamples), 44100, 32000);
	}

	void reset() override
	{
		dec.Stop();
		Resampler.Reset();
		retainedSamples = 0;
		memset(fifo, 0, sizeof(fifo));
		fifoIdxW = fifoIdxR = 0;
		status = 1;
		mpegState = 0;	// why doesn't RB ever init this?
		volume = 0x7F;	// full volume
		usingLoopStart = 0;
		Z80.Reset();
	}

private:
	static int Z80IRQCallback(CZ80 *) { return 0x38; }

	resampler Resampler;
	int retainedSamples = 0;
	INT16 mpegL[MPEG_BUF] = {}, mpegR[MPEG_BUF] = {};
	const UINT8 *progROM, *mpegROM;
	std::vector<UINT8> ram;
	UINT8 fifo[FIFO_STACK_SIZE] = {};
	int fifoIdxR = 0, fifoIdxW = 0;
	// must init these otherwise we end up trying to read illegal addresses
	int mpegStart = 0, mpegEnd = 0, mpegState = 0, loopStart = 0, loopEnd = 0;
	UINT32 usingLoopStart = 0, usingLoopEnd = 0, usingMPEGStart = 0, usingMPEGEnd = 0;
	UINT32 startLatch = 0, endLatch = 0;
	UINT8 status = 1, cmdLatch = 0, volume = 0x7F, stereo = 0;
	mpeg_player dec;
	CZ80 Z80;
};

// ------------------------------------------------------------------ DSB2: 68000
enum
{
	ST_IDLE = 0, ST_GOT14, ST_14_0, ST_14_1, ST_GOT24, ST_24_0, ST_24_1, ST_GOT74,
	ST_GOTA0, ST_GOTA1, ST_GOTA3, ST_GOTA4, ST_GOTA5, ST_GOTA7,
	ST_GOTB0, ST_GOTB1, ST_GOTB2, ST_GOTB4, ST_GOTB5, ST_GOTB6
};

// the running Musashi context swapped for another and back (Supermodel's M68KSetContext / Get)
struct m68k_swap
{
	std::vector<uint8_t> saved;
	m68k_bus *bus;
	std::vector<uint8_t> &mine;
	m68k_swap(std::vector<uint8_t> &ctx, m68k_bus *b) : saved(m68k_context_size()), bus(g_m68k_bus), mine(ctx)
	{
		m68k_get_context(saved.data());
		if (!mine.empty()) m68k_set_context(mine.data());
		g_m68k_bus = b;
	}
	~m68k_swap()
	{
		mine.resize(m68k_context_size());
		m68k_get_context(mine.data());
		m68k_set_context(saved.data());
		g_m68k_bus = bus;
	}
};

class dsb2 final : public model3_dsb, public m68k_bus
{
public:
	dsb2(const uint8_t *prog, const uint8_t *mpeg) : progROM(prog), mpegROM(mpeg), ram(0x20000, 0)
	{
		ack_clears = true;
		spin_check();
		// Supermodel's M68KInit, in this board's own context: autovectored interrupts that clear
		// on acknowledge (its M68KIRQCallback without an IRQAck; arcsnd's Musashi does the same
		// with M68K_EMULATE_INT_ACK off)
		{
			m68k_swap s(ctx, this);
			m68k_init();
			m68k_set_cpu_type(M68K_CPU_TYPE_68000);
			// a fresh CPU: the context starts as a copy of the one running, whose registers a
			// reset does not touch
			for (int r = M68K_REG_D0; r <= M68K_REG_A7; r++) m68k_set_reg(m68k_register_t(r), 0);
		}
		reset();		// CDSB2::Reset
	}
	int type() const override { return 2; }
	uint64_t spin_turns() const override { return spin_turns_; }
	uint32_t state_hash(uint32_t h) const override
	{
		auto fold = [&](uint32_t v) { for (int k = 0; k < 4; k++) h = (h ^ uint8_t(v >> (8 * k))) * 16777619u; };
		m68k_swap s(const_cast<std::vector<uint8_t> &>(ctx), const_cast<dsb2 *>(this));
		for (int r = M68K_REG_D0; r <= M68K_REG_SR; r++) fold(m68k_get_reg(nullptr, m68k_register_t(r)));
		fold(uint32_t(m_cyclesElapsedThisFrame)); fold(uint32_t(m_nextTimerInterruptCycles));
		for (uint8_t b : ram) h = (h ^ b) * 16777619u;
		return h;
	}
	bool playing() const override { return dec.IsLoaded() && !dec.stopped; }

	// the 68000's map (CDSB2::Read8 .. Write32); a long access is two words, high first, as
	// Supermodel's Read32 / Write32 do it
	uint32_t read8(uint32_t addr) override
	{
		if (addr < (128*1024))
			return progROM[addr^1];
		if (addr == 0xc00001)
			return cmdLatch;
		if (addr == 0xc00003)	// bit 0 = command valid
			return 1;
		if (addr == 0xe80001)
			return 0x01;		// MPEG busy status: bit 1 = busy
		if ((addr >= 0xf00000) && (addr < 0xf10000))
		{
			addr &= 0x1ffff;
			return ram[addr^1];
		}
		return 0;
	}
	uint32_t read16(uint32_t addr) override
	{
		uint16_t v;
		if (addr < (128*1024))
		{
			memcpy(&v, &progROM[addr], 2);
			return v;
		}
		if ((addr >= 0xf00000) && (addr < 0xf20000))
		{
			addr &= 0x1ffff;
			memcpy(&v, &ram[addr], 2);
			return v;
		}
		return 0;
	}
	void write8(uint32_t addr, uint32_t data) override
	{
		if ((addr >= 0xf00000) && (addr < 0xf20000))
		{
			addr &= 0x1ffff;
			ram[addr^1] = uint8_t(data);
			return;
		}
		if (addr == 0xd00001) return;
		if (addr == 0xe00003)
		{
			WriteMPEGFIFO(uint8_t(data));
			return;
		}
	}
	void write16(uint32_t addr, uint32_t data) override
	{
		if ((addr >= 0xf00000) && (addr < 0xf20000))
		{
			addr &= 0x1ffff;
			const uint16_t v = uint16_t(data);
			memcpy(&ram[addr], &v, 2);
		}
	}

	void send(uint8_t data) override
	{
		fifo[fifoIdxW++] = data;
		fifoIdxW &= FIFO_STACK_SIZE_MASK;
	}

	void run(int numSamples, float *audioL, float *audioR) override
	{
		{
			m68k_swap s(ctx, this);

			// While FIFO not empty...
			while (fifoIdxR != fifoIdxW)
			{
				cmdLatch = fifo[fifoIdxR];	// retrieve next command byte
				fifoIdxR++;
				fifoIdxR &= FIFO_STACK_SIZE_MASK;
				m68k_set_irq(1);	// indicate pending command
				m_cyclesElapsedThisFrame += execute(500);
			}

			// IRQ2 a 1 kHz timer (Supermodel's note: Matthew Daniels' finding for Daytona 2 and Sega
			// Rally 2's fades)
			const int framePeriod = (int)((11000000LL * numSamples) / 44100);	// 68K at 11 MHz
			while (m_cyclesElapsedThisFrame < framePeriod)
			{
				if (m_cyclesElapsedThisFrame >= m_nextTimerInterruptCycles)
				{
					m68k_set_irq(2);
					m_nextTimerInterruptCycles += k_timerPeriod;
				}
				int cyclesToRun = (std::min)(m_nextTimerInterruptCycles, framePeriod) - m_cyclesElapsedThisFrame;
				m_cyclesElapsedThisFrame += execute(cyclesToRun);
			}
			m_cyclesElapsedThisFrame -= framePeriod;
			m_nextTimerInterruptCycles -= framePeriod;
		}

		// Decode MPEG for this frame
		dec.DecodeAudio(&mpegL[retainedSamples], &mpegR[retainedSamples], MPEG_SAMPLES_FOR(numSamples) - retainedSamples);

		INT16 *leftChannelSource = nullptr;
		INT16 *rightChannelSource = nullptr;
		UINT8 volL = 0, volR = 0;
		switch (stereo)
		{
		default:
		case StereoMode::Stereo:
			leftChannelSource = mpegL; rightChannelSource = mpegR; volL = volume[0]; volR = volume[1];
			break;
		case StereoMode::MonoLeft:
			leftChannelSource = mpegL; rightChannelSource = mpegL; volL = volume[0]; volR = volume[0];
			break;
		case StereoMode::MonoRight:
			leftChannelSource = mpegR; rightChannelSource = mpegR; volL = volume[1]; volR = volume[1];
			break;
		}
		retainedSamples = Resampler.UpSampleAndMix(audioL, audioR, leftChannelSource, rightChannelSource, volL, volR, numSamples, MPEG_SAMPLES_FOR(numSamples), 44100, 32000);
	}

	void reset() override
	{
		resetState();
		reset_cpu();
	}

private:
	enum class StereoMode : uint8_t { Stereo = 0, MonoLeft = 1, MonoRight = 2 };
	static constexpr int k_timerPeriod = 11000000/1000; // 1KHz timer

	// The spin skip. Daytona 2's DSB2 program (epr-20886) waits in two loops:
	//   0x73A, for work: a call to 0xE02, which compares the command ring's two pointers (0xF01000,
	//     0xF01004), and a compare of the timer count at 0xF0100E with 0x50 (eight instructions);
	//   0xD7E, a 40 ms pause: the timer count compared with 0x28 (two instructions).
	// Nothing in them writes but the call's own return address, the same each time, and only the
	// interrupt handlers change what they read. Musashi takes an interrupt only as m68k_execute starts
	// or when an instruction changes SR, and none in the loops does, so once the CPU is at a loop's
	// head with its condition holding and no interrupt to take, it goes round until the run's cycles
	// are spent. Whole turns can be counted instead of run: their cycles are added and the rest run
	// as before (at least one turn and the part-turn, so the registers, the flags and the stack slot
	// end as they would). The output and the CPU's state after every run are the same; a loop's
	// cycles a turn are measured on its first turn, and a loop whose turn does not follow its path is
	// dropped. The loops are found by their instructions, wherever they sit (spin_check): Sega Rally 2's
	// program (epr-20641) has the same two every address 0x28 lower (0x712, its call to 0xDDA and its compare
	// at 0xBC8; 0xD56), and a program without them runs with no skip.
	struct spin_loop
	{
		uint32_t head;
		std::vector<uint32_t> path;                       // the turn's instructions
		std::vector<std::pair<uint32_t, uint16_t>> words;  // the program's words there
		int cycles = 0;                                   // a turn's, once measured
		bool ok = true;
		bool pause = false;                               // the 40 ms pause (else the wait for work)
	};
	std::vector<spin_loop> spins;                     // found by spin_check
	bool spin_ok = false;
	uint64_t spin_turns_ = 0;

	uint32_t ram16(uint32_t a) const { uint16_t v; memcpy(&v, &ram[a & 0x1FFFF], 2); return v; }
	uint32_t ram32(uint32_t a) const { return ram16(a) << 16 | ram16(a + 2); }
	uint32_t rom16(uint32_t a) const { uint16_t v; memcpy(&v, &progROM[a], 2); return v; }

	// the program's words from a, against a pattern (-1 any word); the words matched are kept
	bool match(uint32_t a, std::initializer_list<int> pat, spin_loop &l) const
	{
		if (a + 2 * pat.size() > PROG_BYTES) return false;
		uint32_t x = a;
		for (int w : pat)
		{
			if (w >= 0 && rom16(x) != uint32_t(w)) return false;
			l.words.push_back({x, uint16_t(rom16(x))});
			x += 2;
		}
		return true;
	}
	// a branch's target: the opcode's address + 2 + the 16-bit displacement
	static uint32_t rel(uint32_t op, uint32_t disp) { return op + 2 + uint32_t(int32_t(int16_t(disp))); }

	// The two loops, found anywhere in the program by their instructions and their branches agreeing:
	//   the pause: cmpi.w #$28, $F0100E; bcs.s back to the cmpi;
	//   the wait for work, at H: bsr C; beq K, where C is movea.l $F01000, A4; cmpa.l $F01004, A4;
	//     bne +4; rts, and K is cmpi.w #$50, $F0100E; bcs back to H.
	// Only the ring and timer addresses and the constants are fixed; a branch must land where the loop's
	// next instruction is, so no other code matches.
	void spin_check()
	{
		spins.clear();
		for (uint32_t a = 0; a + 10 <= PROG_BYTES; a += 2)
		{
			spin_loop p{a, {a, a + 8}, {}};
			if (match(a, {0x0C79, 0x0028, 0x00F0, 0x100E, 0x65F6}, p)) { p.pause = true; spins.push_back(p); }
			spin_loop k{0, {}, {}};
			if (!match(a, {0x0C79, 0x0050, 0x00F0, 0x100E, 0x6500, -1}, k)) continue;
			const uint32_t h = rel(a + 8, rom16(a + 10));                 // the bcs's target, the loop's head
			spin_loop w{h, {}, {}};
			if (!match(h, {0x6100, -1, 0x6700, -1}, w)) continue;
			const uint32_t c = rel(h, rom16(h + 2)), kk = rel(h + 4, rom16(h + 6));
			if (kk != a || !match(c, {0x2879, 0x00F0, 0x1000, 0xB9F9, 0x00F0, 0x1004, 0x6600, 0x0004, 0x4E75}, w)) continue;
			w.words.insert(w.words.end(), k.words.begin(), k.words.end());
			w.path = {h, c, c + 6, c + 12, c + 16, h + 4, a, a + 8};
			spins.push_back(w);
		}
		spin_ok = !spins.empty();
	}
	const std::vector<spin_loop> &spin_loops() const { return spins; }
	// the loop the CPU is at the head of, with its condition holding
	spin_loop *spin_at(uint32_t pc)
	{
		for (spin_loop &l : spins)
		{
			if (pc != l.head || !l.ok) continue;
			if (l.pause ? ram16(0xF0100E) < 0x28 : (ram32(0xF01000) == ram32(0xF01004) && ram16(0xF0100E) < 0x50)) return &l;
		}
		return nullptr;
	}
	static bool in_spin(const spin_loop &l, uint32_t pc)
	{
		for (uint32_t x : l.path) if (x == pc) return true;
		return false;
	}

	// m68k_execute(cycles), with whole turns of a wait loop counted rather than run
	int execute(int cycles)
	{
#ifndef ARCSND_NO_DSB_SPIN
		if (!spin_ok || cycles < 1000) return m68k_execute(cycles);
		spin_loop *home = nullptr;
		{
			const uint32_t pc = m68k_get_reg(nullptr, M68K_REG_PC);
			for (spin_loop &l : spins) if (l.ok && in_spin(l, pc)) home = &l;
		}
		if (!home) return m68k_execute(cycles);
		// a run starts in a loop, often with the timer's interrupt due: on to the loop's head an
		// instruction at a time, the interrupt and its handler among them (m68k_execute(1) runs what
		// m68k_execute(cycles) would: an interrupt is taken where it would be, at the first one's start
		// or at the SR change that lets it in), then the whole turns
		int used = 0;
		for (int i = 0; i < 48 && used + 400 < cycles; i++)
		{
			const uint32_t pc = m68k_get_reg(nullptr, M68K_REG_PC);
			bool head = false;
			for (const spin_loop &l : spins) head = head || (l.ok && pc == l.head);
			if (head && dsb_m68k_runs_plain()) break;
			used += m68k_execute(1);
		}
		spin_loop *l = used + 400 < cycles && dsb_m68k_runs_plain() ? spin_at(m68k_get_reg(nullptr, M68K_REG_PC)) : nullptr;
		if (l)
		{
			if (!l->cycles)
			{
				// the first time: one turn, an instruction at a time, its path checked
				int turn = 0;
				for (uint32_t pc : l->path)
				{
					if (m68k_get_reg(nullptr, M68K_REG_PC) != pc) { l->ok = false; break; }
					turn += m68k_execute(1);
				}
				used += turn;
				if (l->ok && m68k_get_reg(nullptr, M68K_REG_PC) == l->head) l->cycles = turn;
				else l->ok = false;
			}
			else
			{
				const int turns = (cycles - used - 1) / l->cycles - 1;
				if (turns > 0)
				{
					used += turns * l->cycles;
					spin_turns_ += uint64_t(turns);
				}
			}
		}
		return used + (cycles > used ? m68k_execute(cycles - used) : 0);
#else
		return m68k_execute(cycles);
#endif
	}

	void reset_cpu()
	{
		m68k_swap s(ctx, this);
		m68k_pulse_reset();
	}

	void resetState()
	{
		dec.Stop();
		Resampler.Reset();
		retainedSamples = 0;
		memset(fifo, 0, sizeof(fifo));
		fifoIdxW = fifoIdxR = 0;
		mpegState = ST_IDLE;
		mpegStart = 0;
		mpegEnd = 0;
		playing_ = 0;
		volume[0] = 0xFF;	// set to max volume in case we miss the volume commands
		volume[1] = 0xFF;
		stereo = StereoMode::Stereo;
		m_cyclesElapsedThisFrame = 0;
		m_nextTimerInterruptCycles = k_timerPeriod;
	}

	void WriteMPEGFIFO(UINT8 byte)
	{
		switch (mpegState)
		{
		case ST_IDLE:
			if (byte == 0x14) mpegState = ST_GOT14;
			else if (byte == 0x15) mpegState = ST_GOT14;
			else if (byte == 0x24) mpegState = ST_GOT24;
			else if (byte == 0x25) mpegState = ST_GOT24;
			else if (byte == 0x74 || byte == 0x75)	// "start play"
			{
				usingLoopStart	= 0;
				usingLoopEnd	= 0;
				usingMPEGStart	= mpegStart;
				usingMPEGEnd	= mpegEnd;
				playing_		= 1;
				dec.SetMemory(mpegROM, mpegStart, mpegEnd - mpegStart, false);
				mpegState = ST_IDLE;
			}
			else if (byte == 0x84 || byte == 0x85)
			{
				dec.Stop();
				playing_ = 0;
			}
			else if (byte == 0xa0) mpegState = ST_GOTA0;
			else if (byte == 0xa1) mpegState = ST_GOTA1;
			else if (byte == 0xA3) mpegState = ST_GOTA3;
			else if (byte == 0xa4) mpegState = ST_GOTA4;
			else if (byte == 0xa5) mpegState = ST_GOTA5;
			else if (byte == 0xA7) mpegState = ST_GOTA7;
			else if (byte == 0xb0) mpegState = ST_GOTB0;
			else if (byte == 0xb1) mpegState = ST_GOTB1;
			else if (byte == 0xB2) mpegState = ST_GOTB2;
			else if (byte == 0xb4) mpegState = ST_GOTB4;
			else if (byte == 0xb5) mpegState = ST_GOTB5;
			else if (byte == 0xB6) mpegState = ST_GOTB6;
			break;
		case ST_GOT14: mpegStart &= 0x00FFFF; mpegStart |= (byte<<16); mpegState++; break;
		case ST_14_0:  mpegStart &= 0xFF00FF; mpegStart |= (byte<<8); mpegState++; break;
		case ST_14_1:
			mpegStart &= 0xFFFF00;
			mpegStart |= (byte);
			mpegState = ST_IDLE;
			if (playing_)
			{
				usingLoopStart	= mpegStart;
				usingLoopEnd	= mpegEnd - mpegStart;
				dec.UpdateMemory(mpegROM, usingLoopStart, usingLoopEnd, true);
			}
			break;
		case ST_GOT24: mpegEnd &= 0x00FFFF; mpegEnd |= (byte<<16); mpegState++; break;
		case ST_24_0:  mpegEnd &= 0xFF00FF; mpegEnd |= (byte<<8); mpegState++; break;
		case ST_24_1:
			mpegEnd &= 0xFFFF00;
			mpegEnd |= (byte);
			stereo = StereoMode::Stereo;
			mpegState = ST_IDLE;
			break;
		case ST_GOTA0:
			stereo = (byte != 0x00) ? StereoMode::MonoLeft : StereoMode::Stereo;
			mpegState = ST_IDLE;
			break;
		case ST_GOTA4:	// dayto2pe plays advertise tune from this state by writing 0x75
			mpegState = ST_IDLE;
			if (byte == 0x75)
			{
				usingLoopStart	= 0;
				usingLoopEnd	= 0;
				usingMPEGStart	= mpegStart;
				usingMPEGEnd	= mpegEnd;
				playing_		= 1;
				dec.SetMemory(mpegROM, mpegStart, mpegEnd - mpegStart, false);
			}
			break;
		case ST_GOTA5: mpegState = ST_IDLE; break;
		case ST_GOTB1:
			stereo = (byte != 0x00) ? StereoMode::MonoRight : StereoMode::Stereo;
			mpegState = ST_IDLE;
			break;
		case ST_GOTB4:
			mpegState = ST_IDLE;
			if (byte == 0x96)
			{
				dec.Stop();
				playing_ = 0;
			}
			break;
		case ST_GOTB5: mpegState = ST_IDLE; break;
		// Daytona 2: B6, A7 the rear speakers, B2, A3 the front (Supermodel's notes)
		case ST_GOTB6: volume[0] = byte; mpegState = ST_IDLE; break;
		case ST_GOTB0: volume[0] = byte; mpegState = ST_IDLE; break;
		case ST_GOTA7: volume[1] = byte; mpegState = ST_IDLE; break;
		case ST_GOTA1: volume[1] = byte; mpegState = ST_IDLE; break;
		case ST_GOTB2: mpegState = ST_IDLE; break;
		case ST_GOTA3: mpegState = ST_IDLE; break;
		default: break;
		}
	}

	resampler Resampler;
	int retainedSamples = 0;
	INT16 mpegL[MPEG_BUF] = {}, mpegR[MPEG_BUF] = {};
	const UINT8 *progROM, *mpegROM;
	std::vector<UINT8> ram;
	UINT8 fifo[FIFO_STACK_SIZE] = {};
	int fifoIdxR = 0, fifoIdxW = 0;
	int cmdLatch = 0, mpegState = ST_IDLE, mpegStart = 0, mpegEnd = 0, playing_ = 0;
	UINT8 volume[2] = {0xFF, 0xFF};
	StereoMode stereo = StereoMode::Stereo;
	UINT32 usingLoopStart = 0, usingLoopEnd = 0, usingMPEGStart = 0, usingMPEGEnd = 0;
	std::vector<uint8_t> ctx;
	int m_cyclesElapsedThisFrame = 0, m_nextTimerInterruptCycles = k_timerPeriod;
	mpeg_player dec;
};

}  // namespace

std::unique_ptr<model3_dsb> model3_dsb::make(int type, const uint8_t *prog, const uint8_t *mpeg)
{
	if (type == 1) return std::make_unique<dsb1>(prog, mpeg);
	if (type == 2) return std::make_unique<dsb2>(prog, mpeg);
	return nullptr;
}
