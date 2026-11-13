// SPDX-License-Identifier: GPL-3.0-or-later
// The Model 3 sound board's ROMs from the set's zip (roms.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// the set's 68000 program (512 KB) and samples (16 MB) as model3_board takes them; false and why if
// the zip or a file is missing. dir: a folder to look in instead of MiSTer's mame folders.
bool model3_sound_roms(const char *set, std::vector<uint8_t> &prog, std::vector<uint8_t> &samples, std::string &why, const char *dir = nullptr);
// the set's Digital Sound Board: type 1 (DSB1, Z80) or 2 (DSB2, 68000) with its program (128 KB, as
// model3_dsb takes it) and MPEG music (16 MB), or type 0 for a set without one; false and why if
// the zip or a file is missing
// every set the ROM table knows (roms_table.h), in its order: the sets the session claims
void model3_rom_sets(std::vector<const char *> &out);
bool model3_dsb_roms(const char *set, int &type, std::vector<uint8_t> &prog, std::vector<uint8_t> &mpeg, std::string &why, const char *dir = nullptr);
