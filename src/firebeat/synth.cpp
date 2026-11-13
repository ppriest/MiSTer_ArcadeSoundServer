// SPDX-License-Identifier: GPL-3.0-or-later
// synth.h has what is followed and what is not.

#define TSF_IMPLEMENTATION
#include "tsf.h"

#include "synth.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr int VOICES = 48;                       // the XT446's SWP30 has 64; fewer for the A9
constexpr float GAIN_DB = -6.0f;
}

namespace {

// the file read-only into memory, every page read in; nullptr when it cannot be
void *map_file(const char *path, size_t &size)
{
#ifdef _WIN32
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return nullptr;
	LARGE_INTEGER n;
	HANDLE m = GetFileSizeEx(h, &n) ? CreateFileMappingA(h, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
	CloseHandle(h);
	if (!m) return nullptr;
	void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
	CloseHandle(m);
	size = size_t(n.QuadPart);
	return p;
#else
	const int fd = open(path, O_RDONLY);
	if (fd < 0) return nullptr;
	struct stat st;
	void *p = fstat(fd, &st) == 0 && st.st_size > 0
	        ? mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0) : MAP_FAILED;
	close(fd);
	if (p == MAP_FAILED) return nullptr;
	size = size_t(st.st_size);
	// a page dropped under memory pressure would be read back from the SD card mid-render
	if (mlock(p, size) != 0) perror("arcsnd: mlock SoundFont");
	return p;
#endif
}

void unmap_file(void *p, size_t size)
{
#ifdef _WIN32
	(void)size;
	UnmapViewOfFile(p);
#else
	munmap(p, size);
#endif
}

}  // namespace

bool fb_synth::load(const char *sf2_path)
{
	base = map_file(sf2_path, size);
	if (!base) return false;
	f = size <= 0x7FFFFFFF ? tsf_load_memory(base, int(size)) : nullptr;
	if (!f) { unmap_file(base, size); base = nullptr; return false; }
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, RATE, GAIN_DB);
	tsf_set_max_voices(f, VOICES);
	reset();
	return true;
}

fb_synth::~fb_synth()
{
	if (f) tsf_close(f);
	if (base) unmap_file(base, size);
}

void fb_synth::part_reset(int ch)
{
	// XG's part 10 is a drum kit at power-on
	bank_msb[ch] = ch == 9 ? 127 : 0;
	prog[ch] = 0;
	tsf_channel_midi_control(f, ch, 121, 0);
	tsf_channel_midi_control(f, ch, 7, 100);
	tsf_channel_midi_control(f, ch, 10, 64);
	tsf_channel_set_pitchwheel(f, ch, 8192);
	program(ch);
}

void fb_synth::reset()
{
	if (!f) return;
	tsf_reset(f);
	for (int ch = 0; ch < 16; ch++) part_reset(ch);
	status = 0; have = 0; in_sysex = false; sx.clear();
}

void fb_synth::release_all()
{
	if (!f) return;
	for (int ch = 0; ch < 16; ch++) tsf_channel_note_off_all(f, ch);
}

void fb_synth::program(int ch)
{
	int i = -1;
	if (bank_msb[ch] == 127)
	{
		i = tsf_get_presetindex(f, 128, prog[ch]);
		if (i < 0) i = tsf_get_presetindex(f, 128, 0);
	}
	if (i < 0) i = tsf_get_presetindex(f, 0, prog[ch]);
	if (i < 0) i = 0;
	tsf_channel_set_presetindex(f, ch, i);
}

void fb_synth::message()
{
	const int ch = status & 15, d0 = data[0], d1 = data[1];
	switch (status >> 4)
	{
	case 0x8: tsf_channel_note_off(f, ch, d0); break;
	case 0x9:
		if (d1 == 0) tsf_channel_note_off(f, ch, d0);
		else { notes++; if (!tsf_channel_note_on(f, ch, d0, d1 / 127.0f)) dropped++; }
		break;
	case 0xB:
		if (d0 == 0) bank_msb[ch] = d1;                // applied at the next program change, as XG
		else if (d0 == 32) {}                          // XG variations: the GM voice stands in
		else
		{
			tsf_channel_midi_control(f, ch, d0, d1);
			if (d0 == 121) program(ch);
		}
		break;
	case 0xC: prog[ch] = d0; program(ch); break;
	case 0xE: tsf_channel_set_pitchwheel(f, ch, d0 | d1 << 7); break;
	default: break;                                    // aftertouch
	}
}

void fb_synth::byte(uint8_t b)
{
	if (!f) return;
	if (b >= 0xF8) return;                             // real time, active sensing included
	if (in_sysex)
	{
		if (b < 0x80) { if (sx.size() < 64) sx.push_back(b); return; }
		in_sysex = false;
		sysex++;
		// GM System On: 7E 7F 09 01; XG System On: 43 1n 4C 00 00 7E 00
		const bool gm = sx.size() >= 4 && sx[0] == 0x7E && sx[2] == 0x09 && sx[3] == 0x01;
		const bool xg = sx.size() >= 7 && sx[0] == 0x43 && (sx[1] & 0xF0) == 0x10 && sx[2] == 0x4C &&
		                sx[3] == 0 && sx[4] == 0 && sx[5] == 0x7E && sx[6] == 0;
		if (gm || xg) for (int ch = 0; ch < 16; ch++) { tsf_channel_sounds_off_all(f, ch); part_reset(ch); }
		if (b == 0xF7) return;                         // else a status byte ends it: fall through
	}
	if (b == 0xF0) { in_sysex = true; sx.clear(); status = 0; return; }
	if (b >= 0xF0) { status = 0; return; }            // system common: none used
	if (b >= 0x80) { status = b; have = 0; return; }
	if (!status) return;
	data[have++] = b;
	const int need = (status >> 4) == 0xC || (status >> 4) == 0xD ? 1 : 2;
	if (have == need) { message(); have = 0; }
}

void fb_synth::render(int frames, std::vector<int16_t> &out)
{
	const size_t at = out.size();
	out.resize(at + 2 * size_t(frames));
	if (f) tsf_render_short(f, out.data() + at, frames, 0);
}

int fb_synth::voices() const
{
	return f ? tsf_active_voice_count(f) : 0;
}
