// SPDX-License-Identifier: GPL-3.0-or-later
// Musashi has one 68000, in globals, and calls m68k_read_memory_8() and the rest by name. The
// boards that use it (hornet, model3) each point g_m68k_bus at their own memory map before they
// run it; only one board runs at a time. I took the callbacks out of hornet/board.cpp into
// m68k_bus.cpp when the Model 3 board came in, since both boards link into one binary; the pages
// below let the Model 3 board give Musashi its RAM and ROM directly, which it needs for speed.
#pragma once

#include <cstdint>

struct m68k_bus
{
	// Plain memory a board can hand Musashi directly, by 1 MB page of the 24-bit space: bytes at
	// p + (address & mask), each 68000 word stored little-endian (a byte at address ^ 1, Supermodel's
	// order). A null p sends the page's accesses through the functions below.
	struct page { uint8_t *p = nullptr; uint32_t mask = 0; };
	page rd[16], wr[16];
	// the interrupt line clears when the CPU takes the interrupt (Supermodel's default acknowledge,
	// which its DSB2 uses). arcsnd's Musashi (M68K_EMULATE_INT_ACK off) always clears it; the flag is
	// for a build on a Musashi with an acknowledge callback (the bench's Supermodel Musashi)
	bool ack_clears = false;

	virtual uint32_t read8(uint32_t a) = 0;
	virtual uint32_t read16(uint32_t a) = 0;
	virtual void write8(uint32_t a, uint32_t d) = 0;
	virtual void write16(uint32_t a, uint32_t d) = 0;
	virtual ~m68k_bus() = default;
};

extern m68k_bus *g_m68k_bus;
