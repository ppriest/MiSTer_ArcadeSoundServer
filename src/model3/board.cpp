// SPDX-License-Identifier: GPL-3.0-or-later
// The Model 3 sound board (board.h, PROVENANCE.md), from Supermodel's Src/Model3/SoundBoard.cpp.

#include "board.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "SCSP.h"

extern "C" {
#include "../hornet/musashi/m68k.h"
}

namespace {

constexpr int CHUNK = 1024;                 // samples a call to SCSP_Update

// the SCSP runs the 68000 and sets its interrupt level (SoundBoard.cpp's callbacks)
int run_68k(int cycles) { return m68k_execute(cycles) - cycles; }
void irq_68k(int level) { m68k_set_irq(level); }

Util::Config::Node g_config;

inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

}  // namespace

model3_board::model3_board(const uint8_t *prog, const uint8_t *samples, int timer_mode)
	: m_ram1(new uint8_t[0x100000]()), m_ram2(new uint8_t[0x100000]()), m_prog(prog), m_smp(samples)
{
	for (auto &b : m_buf) b.assign(CHUNK, 0.0f);
	SCSP_SetBuffers(m_buf[0].data(), m_buf[1].data(), m_buf[2].data(), m_buf[3].data(), CHUNK);
	SCSP_SetCB(run_68k, irq_68k);
	SCSP_Init(g_config, 2);
	SCSP_SetTimerMode(timer_mode);
	SCSP_SetRAM(0, m_ram1.get());
	SCSP_SetRAM(1, m_ram2.get());
	// CSoundBoard::Reset: the vector table into RAM, the default banks, the 68000's reset
	memcpy(m_ram1.get(), m_prog, 16);
	// the plain memory, straight to Musashi (read8 .. write16 below do the same for these pages)
	rd[0] = wr[0] = {m_ram1.get(), 0xFFFFF};
	rd[2] = wr[2] = {m_ram2.get(), 0xFFFFF};
	rd[6] = {const_cast<uint8_t *>(m_prog), 0x7FFFF};
	m_ctrl = 0;
	update_banks();
	if (getenv("M3_PTRS"))           // where the big blocks are, for the cache simulation (sound/sim)
		fprintf(stderr, "region prog %p 80000\nregion samples %p 1000000\nregion ram1 %p 100000\nregion ram2 %p 100000\n",
		        (const void *)m_prog, (const void *)m_smp, (void *)m_ram1.get(), (void *)m_ram2.get());
	g_m68k_bus = this;
	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_pulse_reset();
}

model3_board::~model3_board()
{
	SCSP_Deinit();
	if (g_m68k_bus == this) g_m68k_bus = nullptr;
}

// Supermodel: with bit 4 of 0x400001 set, 0x800000-0xFFFFFF reads the top 8 MB of the 16
void model3_board::update_banks()
{
	m_bank = (m_ctrl & 0x10) ? m_smp + 0x800000 : m_smp;
	for (int pg = 8; pg < 16; pg++) rd[pg] = {const_cast<uint8_t *>(m_bank), 0x7FFFFF};
}

void model3_board::link_ram(bool on)
{
	wr[0] = on ? page{} : page{m_ram1.get(), 0xFFFFF};
	wr[2] = on ? page{} : page{m_ram2.get(), 0xFFFFF};
}

void model3_board::midi_in(uint8_t b)
{
	SCSP_MidiIn(b);
}

void model3_board::run(int n, std::vector<int32_t> &out)
{
	while (n > 0)
	{
		const int k = n < CHUNK ? n : CHUNK;
		SCSP_Update(k);
		for (int i = 0; i < k; i++)
			for (int c = 0; c < 4; c++) out.push_back(int32_t(m_buf[c][i]));
		m_count += k;
		n -= k;
	}
}

// ------------------------------------------------------------------ the 68000's memory map
// (CSoundBoard::Read8 .. Write32: RAM and ROM hold each 68000 word little-endian, so a byte is at
// its address ^ 1)

uint32_t model3_board::read8(uint32_t a)
{
	switch ((a >> 20) & 0xF)
	{
		case 0x0: return m_ram1[a ^ 1];
		case 0x1: return SCSP_Master_r8(a);
		case 0x2: return m_ram2[(a & 0x0FFFFF) ^ 1];
		case 0x3: return SCSP_Slave_r8(a);
		case 0x6: return m_prog[(a & 0x07FFFF) ^ 1];
		case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF:
			return m_bank[(a & 0x7FFFFF) ^ 1];
		default: return 0;
	}
}

uint32_t model3_board::read16(uint32_t a)
{
	switch ((a >> 20) & 0xF)
	{
		case 0x0: return rd16(&m_ram1[a]);
		case 0x1:
		{
			const uint32_t d = SCSP_Master_r16(a);
			if (m_log && (a & 0xFFF) >= 0x400 && (a & 0xFFF) < 0x430) m_log(SCSP_SampleCount(), 'R', a & 0xFFF, d);
			return d;
		}
		case 0x2: return rd16(&m_ram2[a & 0x0FFFFF]);
		case 0x3: return SCSP_Slave_r16(a);
		case 0x6: return rd16(&m_prog[a & 0x07FFFF]);
		case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF:
			return rd16(&m_bank[a & 0x7FFFFF]);
		default: return 0;
	}
}

void model3_board::write8(uint32_t a, uint32_t d)
{
	switch ((a >> 20) & 0xF)
	{
		case 0x0:
			m_ram1[a ^ 1] = uint8_t(d);
			if (SCSP_DSPLink *l = SCSP_GetDSPLink()) l->command(0, 3, (a & 0xFFFFF) >> 1, rd16(&m_ram1[a & 0xFFFFE]));
			break;
		case 0x1:
			if (m_log && (a & 0xFFF) >= 0x400 && (a & 0xFFF) < 0x430) m_log(SCSP_SampleCount(), 'w', a & 0xFFF, d);
			SCSP_Master_w8(a, uint8_t(d));
			break;
		case 0x2:
			m_ram2[(a & 0x0FFFFF) ^ 1] = uint8_t(d);
			if (SCSP_DSPLink *l = SCSP_GetDSPLink()) l->command(1, 3, (a & 0xFFFFF) >> 1, rd16(&m_ram2[a & 0xFFFFE]));
			break;
		case 0x3: SCSP_Slave_w8(a, uint8_t(d)); break;
		default:
			if (a == 0x400001)
			{
				m_ctrl = uint8_t(d);
				update_banks();
			}
			break;
	}
}

void model3_board::write16(uint32_t a, uint32_t d)
{
	switch ((a >> 20) & 0xF)
	{
		case 0x0:
			wr16(&m_ram1[a], uint16_t(d));
			if (SCSP_DSPLink *l = SCSP_GetDSPLink()) l->command(0, 3, (a & 0xFFFFF) >> 1, uint16_t(d));
			break;
		case 0x1:
			if (m_log && (a & 0xFFF) >= 0x400 && (a & 0xFFF) < 0x430) m_log(SCSP_SampleCount(), 'W', a & 0xFFF, d);
			SCSP_Master_w16(a, uint16_t(d));
			break;
		case 0x2:
			wr16(&m_ram2[a & 0x0FFFFF], uint16_t(d));
			if (SCSP_DSPLink *l = SCSP_GetDSPLink()) l->command(1, 3, (a & 0xFFFFF) >> 1, uint16_t(d));
			break;
		case 0x3: SCSP_Slave_w16(a, uint16_t(d)); break;
		default: break;
	}
}
