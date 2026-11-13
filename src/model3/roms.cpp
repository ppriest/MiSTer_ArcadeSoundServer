// SPDX-License-Identifier: GPL-3.0-or-later
// The Model 3 sound board's ROMs from the set's zip (and its parent's), as MiSTer's MRA loader finds
// them: the 68000 program (512 KB) and the SCSP samples (16 MB; an 8 MB set mirrored into the top
// half, as Supermodel's CModel3::LoadROMSet), each file at its offset from Supermodel's Games.xml
// (roms_table.h), in the zip's byte order. A file is found by its CRC-32 first, then by its name.
// The server reads them while Main loads the set into the core, so the core loads nothing for sound.
#include "roms.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

#include "board.h"
#include "dsb.h"
#include "miniz.h"
#include "roms_table.h"

namespace {

const char *const DIRS[] = {"/media/fat/games/mame/", "/media/fat/_Arcade/mame/", "/media/usb0/games/mame/",
                            "/media/fat/games/MAME/", "/media/fat/_Arcade/MAME/"};

struct zip
{
	mz_zip_archive z{};
	bool open = false;
	explicit zip(const std::string &set, const char *dir_override)
	{
		if (set.empty()) return;
		if (dir_override)
		{
			open = mz_zip_reader_init_file(&z, (std::string(dir_override) + "/" + set + ".zip").c_str(), 0);
			return;
		}
		for (const char *d : DIRS)
			if ((open = mz_zip_reader_init_file(&z, (std::string(d) + set + ".zip").c_str(), 0))) return;
	}
	~zip() { if (open) mz_zip_reader_end(&z); }
	// the entry with this CRC-32, else this name (any folder, any case); -1 none
	int find(const m3snd_file &f)
	{
		if (!open) return -1;
		const int n = int(mz_zip_reader_get_num_files(&z));
		for (int i = 0; i < n; i++)
		{
			mz_zip_archive_file_stat st;
			if (mz_zip_reader_file_stat(&z, mz_uint(i), &st) && !st.m_is_directory && st.m_crc32 == f.crc) return i;
		}
		for (int i = 0; i < n; i++)
		{
			mz_zip_archive_file_stat st;
			if (!mz_zip_reader_file_stat(&z, mz_uint(i), &st) || st.m_is_directory) continue;
			const char *b = strrchr(st.m_filename, '/');
			if (!strcasecmp(b ? b + 1 : st.m_filename, f.name)) return i;
		}
		return -1;
	}
};

// one region: every file from the set's zip or its parent's, at its offset; the bytes used
bool region(zip &a, zip &b, const m3snd_file *files, int n, std::vector<uint8_t> &out, uint32_t &used, std::string &why)
{
	used = 0;
	for (int k = 0; k < n && files[k].name; k++)
	{
		zip *z = &a;
		int i = a.find(files[k]);
		if (i < 0) { z = &b; i = b.find(files[k]); }
		if (i < 0) { why = std::string("no ") + files[k].name + " in the set's zip or its parent's"; return false; }
		mz_zip_archive_file_stat st;
		mz_zip_reader_file_stat(&z->z, mz_uint(i), &st);
		if (st.m_crc32 != files[k].crc) fprintf(stderr, "arcsnd model3: %s's CRC is %08x, not %08x (a different dump)\n", files[k].name, unsigned(st.m_crc32), files[k].crc);
		if (files[k].offset + st.m_uncomp_size > out.size()) { why = std::string(files[k].name) + " does not fit its region"; return false; }
		if (!mz_zip_reader_extract_to_mem(&z->z, mz_uint(i), out.data() + files[k].offset, size_t(st.m_uncomp_size), 0))
		{
			why = std::string("cannot inflate ") + files[k].name;
			return false;
		}
		used = std::max<uint32_t>(used, files[k].offset + uint32_t(st.m_uncomp_size));
	}
	return true;
}

}  // namespace

void model3_rom_sets(std::vector<const char *> &out)
{
	for (const m3snd_set &x : M3SND_SETS) out.push_back(x.name);
}

bool model3_sound_roms(const char *set, std::vector<uint8_t> &prog, std::vector<uint8_t> &samples, std::string &why, const char *dir)
{
	const m3snd_set *s = nullptr;
	for (const m3snd_set &x : M3SND_SETS)
		if (!strcmp(x.name, set)) s = &x;
	if (!s) { why = std::string("no sound ROM layout for ") + set; return false; }
	zip a(s->name, dir), b(s->parent, dir);
	if (!a.open && !b.open) { why = std::string("no ") + set + ".zip"; return false; }
	prog.assign(model3_board::PROG_BYTES, 0);
	samples.assign(model3_board::SAMPLE_BYTES, 0);
	uint32_t used = 0;
	if (!region(a, b, s->prog, 2, prog, used, why)) return false;
	if (!region(a, b, s->smp, 5, samples, used, why)) return false;
	if (used <= model3_board::SAMPLE_BYTES / 2) memcpy(samples.data() + model3_board::SAMPLE_BYTES / 2, samples.data(), model3_board::SAMPLE_BYTES / 2);
	return true;
}

bool model3_dsb_roms(const char *set, int &type, std::vector<uint8_t> &prog, std::vector<uint8_t> &mpeg, std::string &why, const char *dir)
{
	type = 0;
	const m3dsb_set *s = nullptr;
	for (const m3dsb_set &x : M3DSB_SETS)
		if (!strcmp(x.name, set)) s = &x;
	if (!s) return true;                                   // no Digital Sound Board on this set
	zip a(s->name, dir), b(s->parent, dir);
	if (!a.open && !b.open) { why = std::string("no ") + set + ".zip"; return false; }
	prog.assign(model3_dsb::PROG_BYTES, 0);
	mpeg.assign(model3_dsb::MPEG_BYTES, 0);
	uint32_t used = 0;
	if (!region(a, b, s->prog, 2, prog, used, why)) return false;
	if (!region(a, b, s->mpeg, 5, mpeg, used, why)) return false;
	type = s->type;
	return true;
}
