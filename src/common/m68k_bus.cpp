// SPDX-License-Identifier: GPL-3.0-or-later
// Musashi's memory callbacks, through the running board's map (m68k_bus.h): its plain-memory pages
// directly, the rest through its functions. 24-bit addresses; a long access is two words, high first.

#include "m68k_bus.h"

#include <cstring>

m68k_bus *g_m68k_bus = nullptr;

namespace {

inline unsigned r16(unsigned a)
{
	a &= 0xffffff;
	const m68k_bus::page &g = g_m68k_bus->rd[a >> 20];
	if (g.p)
	{
		uint16_t v;
		memcpy(&v, g.p + (a & g.mask), 2);
		return v;
	}
	return g_m68k_bus->read16(a);
}

inline void w16(unsigned a, unsigned d)
{
	a &= 0xffffff;
	const m68k_bus::page &g = g_m68k_bus->wr[a >> 20];
	if (g.p)
	{
		const uint16_t v = uint16_t(d);
		memcpy(g.p + (a & g.mask), &v, 2);
	}
	else g_m68k_bus->write16(a, d & 0xffff);
}

}  // namespace

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a)
{
	a &= 0xffffff;
	const m68k_bus::page &g = g_m68k_bus->rd[a >> 20];
	return g.p ? g.p[(a & g.mask) ^ 1] : g_m68k_bus->read8(a);
}
unsigned int m68k_read_memory_16(unsigned int a) { return r16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return r16(a) << 16 | r16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int d)
{
	a &= 0xffffff;
	const m68k_bus::page &g = g_m68k_bus->wr[a >> 20];
	if (g.p) g.p[(a & g.mask) ^ 1] = uint8_t(d);
	else g_m68k_bus->write8(a, d & 0xff);
}
void m68k_write_memory_16(unsigned int a, unsigned int d) { w16(a, d); }
void m68k_write_memory_32(unsigned int a, unsigned int d)
{
	w16(a, d >> 16);
	w16(a + 2, d & 0xffff);
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return r16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
}
