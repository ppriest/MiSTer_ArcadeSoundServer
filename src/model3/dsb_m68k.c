// SPDX-License-Identifier: GPL-3.0-or-later
// For dsb.cpp's spin skip: whether Musashi's next m68k_execute would start by taking an interrupt
// (or eat reset cycles, or is stopped), read from its own state (m68kcpu.h's macros, arcsnd's Musashi).
#include "../hornet/musashi/m68kcpu.h"

int dsb_m68k_runs_plain(void)
{
	return !(CPU_INT_LEVEL > FLAG_INT_MASK) && !m68ki_cpu.nmi_pending && !RESET_CYCLES && !CPU_STOPPED;
}
