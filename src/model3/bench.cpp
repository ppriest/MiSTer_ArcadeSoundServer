// SPDX-License-Identifier: GPL-3.0-or-later
// The Model 3 sound board against a Supermodel capture (supermodel-snd's M3SND_DIR: midi.txt, and
// sm_pcm.bin, the sound board's four channels as 32-bit integers a sample). Supermodel runs a
// frame of the PowerPC and then that frame's samples, so frame f's MIDI bytes ("D f cycle byte")
// are given to the SCSP before frame f's samples ("S f count"). With Supermodel's timers (-t 0)
// every sample must equal Supermodel's; with the arcade's (-t 1, the default) the timers' period is
// what moves. The stereo mix (Supermodel's: L = (front L + rear L) / 2, R likewise) goes to a
// 44.1 kHz wav. Reports timer A's re-arm period, from the 68000's writes to TIMA, and the time the
// board takes per second of sound.
//
//   model3_bench <prog.bin> <samples.bin> <midi.txt> <out.wav> [-t mode] [-ref sm_pcm.bin] [-frames n] [-dump out.bin]
//                [-dsb type dsbprog.bin mpeg.bin] [-mixref sm_pcm.bin sm_mix.bin] [-dsbonly] [-split]
// (-ref and -dump: the four channels as 32-bit integers a sample, sm_pcm.bin's form; out.wav may be /dev/null)
// -dsb: the set's Digital Sound Board (dsb.h; type 1 or 2) beside the SCSPs, a frame at a time after
// the frame's MIDI bytes, as Supermodel's CSoundBoard::RunFrame runs it. -mixref: its music against
// Supermodel's, isolated: Supermodel's own SCSP channels (sm_pcm.bin) plus this DSB, converted as
// Supermodel converts its mix, against Supermodel's mix after the DSB (sm_mix.bin), every channel.
// -dsbonly: the DSB alone (the SCSPs not run), for its own cost.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <time.h>
#include "board.h"
#include "SCSP.h"
#include "SCSPDSP.h"
#include "dsplink_shm.h"
#include "dsb.h"

namespace {

std::vector<uint8_t> load(const char *path)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END);
	b.resize(size_t(ftell(f)));
	fseek(f, 0, SEEK_SET);
	if (fread(b.data(), 1, b.size(), f) != b.size()) { perror(path); exit(2); }
	fclose(f);
	return b;
}

void write_wav(const char *path, const std::vector<int16_t> &pcm, int rate)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	const uint32_t data = uint32_t(pcm.size() * 2);
	auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + data); fwrite("WAVEfmt ", 1, 8, f);
	w32(16); w16(1); w16(2); w32(uint32_t(rate)); w32(uint32_t(rate * 4)); w16(4); w16(16);
	fwrite("data", 1, 4, f); w32(data);
	fwrite(pcm.data(), 2, pcm.size(), f);
	fclose(f);
}

int16_t mix(int32_t a, int32_t b)
{
	const int32_t s = int32_t((float(a) + float(b)) * 0.5f);
	return int16_t(std::max(-32768, std::min(32767, s)));
}

double thread_s()
{
	timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

}  // namespace

// called after each frame's samples; does nothing (qemu/markcount.so counts instructions up to each call)
extern "C" __attribute__((noinline)) void m3_frame_mark(unsigned frame) { __asm__ volatile("" : : "r"(frame) : "memory"); }

// the DSB is dsb.cpp's; a bench linked without it (sound/fpga_dsp's, which list their sources) has none
__attribute__((weak)) std::unique_ptr<model3_dsb> model3_dsb::make(int, const uint8_t *, const uint8_t *) { return nullptr; }

static uint64_t m3_frame_base = 0;     // the first sample of the frame being run

int main(int argc, char **argv)
{
	if (argc < 5)
	{
		fprintf(stderr, "usage: model3_bench <prog.bin> <samples.bin> <midi.txt> <out.wav> [-t mode] [-ref sm_pcm.bin] [-frames n]\n");
		return 2;
	}
	int mode = 1;
	long max_frames = -1;
	const char *ref_path = nullptr, *dump_path = nullptr;
	int dsb_type = 0;
	const char *dsb_prog_path = nullptr, *dsb_mpeg_path = nullptr, *mix_pcm_path = nullptr, *mix_ref_path = nullptr;
	// -recref sm_mix.bin (with -split): the DSP run here (the link's fallback, forced), so each record's dry
	// sums are the finished channels with the music in the front pair; clipped as the FPGA clips them, they
	// are compared with Supermodel's mix after the DSB
	const char *rec_ref_path = nullptr;
	bool dsb_only = false, dsb_timed = false;
	for (int i = 5; i < argc; i++)
	{
		if (!strcmp(argv[i], "-split") || !strcmp(argv[i], "-fpga")) continue;
		if (!strcmp(argv[i], "-dsbonly")) { dsb_only = true; continue; }
		if (!strcmp(argv[i], "-dsbtimed")) { dsb_timed = true; continue; }
		if (!strcmp(argv[i], "-dsb") && i + 3 < argc) { dsb_type = atoi(argv[i + 1]); dsb_prog_path = argv[i + 2]; dsb_mpeg_path = argv[i + 3]; i += 3; continue; }
		if (!strcmp(argv[i], "-mixref") && i + 2 < argc) { mix_pcm_path = argv[i + 1]; mix_ref_path = argv[i + 2]; i += 2; continue; }
		if (!strcmp(argv[i], "-recref") && i + 1 < argc) { rec_ref_path = argv[i + 1]; i += 1; continue; }
		if (i + 1 >= argc) break;
		if (!strcmp(argv[i], "-t")) mode = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-ref")) ref_path = argv[i + 1];
		else if (!strcmp(argv[i], "-frames")) max_frames = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-dump")) dump_path = argv[i + 1];
		i++;
	}
	const std::vector<uint8_t> prog = load(argv[1]), smp = load(argv[2]);
	if (prog.size() != model3_board::PROG_BYTES || smp.size() != model3_board::SAMPLE_BYTES)
	{
		fprintf(stderr, "program must be 512 KB and samples 16 MB\n");
		return 2;
	}
	std::vector<uint8_t> ref;
	if (ref_path) ref = load(ref_path);

	// the capture, frame by frame: its MIDI bytes and its sample count
	struct frame { std::vector<uint8_t> midi; std::vector<unsigned long long> cyc; unsigned long long start = 0; int samples = 0; };
	std::vector<frame> fr;
	{
		FILE *f = fopen(argv[3], "r");
		if (!f) { perror(argv[3]); return 2; }
		char line[128];
		while (fgets(line, sizeof line, f))
		{
			unsigned fn, v;
			unsigned long long cyc;
			if (line[0] == 'F' && sscanf(line + 1, "%u %llu", &fn, &cyc) >= 1) { if (fn >= fr.size()) fr.resize(fn + 1); fr[fn].start = cyc; }
			else if (line[0] == 'D' && sscanf(line + 1, "%u %llu %x", &fn, &cyc, &v) == 3) { if (fn >= fr.size()) fr.resize(fn + 1); fr[fn].midi.push_back(uint8_t(v)); fr[fn].cyc.push_back(cyc); }
			else if (line[0] == 'S' && sscanf(line + 1, "%u %u", &fn, &v) == 2) { if (fn >= fr.size()) fr.resize(fn + 1); fr[fn].samples = int(v); }
		}
		fclose(f);
	}
	while (!fr.empty() && fr.back().samples == 0) fr.pop_back();
	if (max_frames >= 0 && size_t(max_frames) < fr.size()) fr.resize(size_t(max_frames));

	model3_board board(prog.data(), smp.data(), mode);
	// the Digital Sound Board, its ROMs, and Supermodel's channels before and after it
	std::vector<uint8_t> dsb_prog, dsb_mpeg, mix_pcm, mix_ref;
	std::unique_ptr<model3_dsb> dsb;
	if (dsb_type)
	{
		dsb_prog = load(dsb_prog_path);
		dsb_mpeg = load(dsb_mpeg_path);
		if (dsb_prog.size() != model3_dsb::PROG_BYTES || dsb_mpeg.size() != model3_dsb::MPEG_BYTES)
		{
			fprintf(stderr, "the DSB's program must be 128 KB and its MPEG 16 MB\n");
			return 2;
		}
		dsb = model3_dsb::make(dsb_type, dsb_prog.data(), dsb_mpeg.data());
		if (!dsb) { fprintf(stderr, "no DSB type %d\n", dsb_type); return 2; }
	}
	if (mix_pcm_path) { mix_pcm = load(mix_pcm_path); mix_ref = load(mix_ref_path); }
	std::vector<uint8_t> rec_ref;
	if (rec_ref_path) rec_ref = load(rec_ref_path);
	size_t rec_compared = 0, rec_differ = 0, rec_rear_differ = 0;
	int32_t rec_maxdiff = 0;
	std::vector<float> dsb_l(model3_dsb::MAX_RUN), dsb_r(model3_dsb::MAX_RUN);
	std::vector<int32_t> dsb_pcm(2 * model3_dsb::MAX_RUN);     // the music as the FPGA takes it (model3_dsb::q8)
	size_t mix_compared = 0, mix_differ = 0, mix_first = SIZE_MAX, dsb_playing = 0, dsb_nonzero = 0;
	uint32_t dsb_hash = 2166136261u;        // FNV-1a over the DSB's float output, bit for bit
	int32_t dsb_peak = 0;
	std::vector<std::pair<uint64_t, uint8_t>> dsb_q;   // -dsbtimed: bytes on their way, with the sample they arrive at
	uint64_t dsb_uart = 0;                             // the sample the uPD71051's last byte is out
	const bool dsb_state = getenv("M3DSB_STATE") != nullptr;   // the DSB's CPU state after every frame, hashed
	uint32_t dsb_state_hash = 2166136261u;
	FILE *dsb_dump = getenv("M3DSB_DUMP") ? fopen(getenv("M3DSB_DUMP"), "wb") : nullptr;   // the DSB's float L, R a sample
	// -split: the HPS's half of the DSP split alone (dsplink_shm into memory rings), for the
	// instruction counts; the output is then the FPGA's and this run's is zero
	dsplink_shm split;
	std::vector<uint32_t> split_rec, split_cmd;
	for (int i = 5; i < argc; i++)
		if (!strcmp(argv[i], "-split"))
		{
			split_rec.assign(size_t(dsplink_shm::REC_STRIDE) * dsplink_shm::REC_RING, 0);
			split_cmd.assign(2 * dsplink_shm::CMD_RING, 0);
			split.rec = split_rec.data();
			split.cmd = split_cmd.data();
			split.start(0, 0, 256);
			split.word_io = getenv("ARCSND_M3_WORDIO") != nullptr;
			split.full_io = getenv("ARCSND_M3_FULL") != nullptr;
			SCSP_SetDSPLink(&split);
			board.link_ram(true);
			if (rec_ref_path) split.unsupported("-recref (forced)");
		}
#ifdef FPGA_DSP
	// the DSP in the FPGA's RTL (sound/fpga_dsp/fpga_link.cpp), the rest here: -fpga
	extern SCSP_DSPLink *make_fpga_link();
	SCSP_DSPLink *fpga = nullptr;
	for (int i = 5; i < argc; i++)
		if (!strcmp(argv[i], "-fpga")) { fpga = make_fpga_link(); SCSP_SetDSPLink(fpga); board.link_ram(true); }
#endif
	// timer A: each 68000 write to TIMA (0x418, the word or its low byte) and the period between them
	std::vector<std::pair<uint64_t, uint32_t>> tima;
	std::map<uint32_t, int> tima_values;
	board.m_log = [&](uint64_t s, char rw, uint32_t reg, uint32_t d) {
		if ((rw == 'W' && reg == 0x418) || (rw == 'w' && reg == 0x419))
		{
			tima.push_back({s, d & 0xff});
			tima_values[d & 0xff]++;
		}
	};

	std::vector<int32_t> out;
	std::vector<int16_t> wav;
	FILE *dump = dump_path ? fopen(dump_path, "wb") : nullptr;
	{
		size_t total = 0;
		for (const frame &x : fr) total += size_t(x.samples);
		wav.reserve(2 * total);         // no reallocation inside the run: the per-frame counts stay the board's
		out.reserve(4 * 2048);
	}
	size_t compared = 0, differ = 0, first_diff = SIZE_MAX, midi_bytes = 0;
	const double t0 = thread_s();
	double run_s = 0, worst = 0;            // worst: a frame's board time over its length in sound
	size_t worst_f = 0, over = 0;
	std::vector<std::pair<double, int>> win;  // the last 58 frames (about a second): board time, samples
	double win_t = 0, worst_win = 0;
	int win_s = 0;
	size_t worst_win_f = 0;
	uint32_t hash = 2166136261u;            // FNV-1a over the four channels, to compare a run without its reference
	for (size_t f = 0; f < fr.size(); f++)
	{
		const double a = thread_s();
		for (size_t k = 0; k < fr[f].midi.size(); k++)
		{
			const uint8_t b = fr[f].midi[k];
			if (!dsb_only) board.midi_in(b);
			if (!dsb) continue;
			if (!dsb_timed) { dsb->send(b); continue; }
			// -dsbtimed: the byte reaches the DSB as the board's sound port delivers it, at its time
			// in the frame (its PowerPC cycle) and then the uPD71051's 10 bits at 31.25 kbaud
			// (MIDI_LAT, 14 samples) after the byte before it has gone
			const unsigned long long c0 = fr[f].start, c1 = f + 1 < fr.size() && fr[f + 1].start > c0 ? fr[f + 1].start : 0;
			const uint64_t at = m3_frame_base + (c1 ? uint64_t(double(fr[f].cyc[k] - c0) / double(c1 - c0) * fr[f].samples) : 0);
			const uint64_t due = std::max(at, dsb_uart) + 14;
			dsb_uart = due;
			dsb_q.push_back({due, b});
		}
		midi_bytes += fr[f].midi.size();
		out.clear();
		const int ns = fr[f].samples;
		if (dsb && ns)
		{
			// the DSB's frame first: its PCM is in each record the SCSP's samples write
			std::fill(dsb_l.begin(), dsb_l.begin() + ns, 0.0f);
			std::fill(dsb_r.begin(), dsb_r.begin() + ns, 0.0f);
			if (!dsb_timed) dsb->run(ns, dsb_l.data(), dsb_r.data());
			else
			{
				// in pieces, each byte given the DSB at the sample it arrives (as the session does)
				int done = 0;
				while (done < ns)
				{
					while (!dsb_q.empty() && dsb_q.front().first <= m3_frame_base + uint64_t(done)) { dsb->send(dsb_q.front().second); dsb_q.erase(dsb_q.begin()); }
					int upto = ns;
					if (!dsb_q.empty() && dsb_q.front().first < m3_frame_base + uint64_t(ns)) upto = int(dsb_q.front().first - m3_frame_base);
					dsb->run(upto - done, dsb_l.data() + done, dsb_r.data() + done);
					done = upto;
				}
			}
			for (int i = 0; i < ns; i++)
			{
				dsb_pcm[2 * i] = model3_dsb::q8(dsb_l[i]);
				dsb_pcm[2 * i + 1] = model3_dsb::q8(dsb_r[i]);
			}
			split.dsb_q8 = dsb_pcm.data();
		}
		if (dsb_only) { for (int i = 0; i < ns; i++) for (int c = 0; c < 4; c++) out.push_back(0); }
		else if (!rec_ref.empty())
		{
			// in pieces the record ring (512) holds, each piece's records read back: the engine's clip of the
			// dry sums (the fallback's pans are zero), 18 bits with DAC18B, else 16 after >> 2, in
			// Supermodel's units (its buffer holds the 18-bit level with DAC18B, the 16-bit otherwise)
			for (int k = 0; k < ns; k += 256)
			{
				const int n = std::min(256, ns - k);
				const uint64_t s0 = board.samples();
				board.run(n, out);
				for (int i = 0; i < n; i++)
				{
					const uint64_t sn = s0 + uint64_t(i);
					const uint32_t *r = split_rec.data() + size_t(dsplink_shm::REC_STRIDE) * ((sn + 255) % dsplink_shm::REC_RING);
					for (int c = 0; c < 4; c++)
					{
						int32_t v = int32_t(r[2 * (32 + c)] << 8) >> 8;
						const int d18 = SCSP_DAC18B(c >> 1);
						v = d18 ? std::max(-131072, std::min(131071, v)) : std::max(-32768, std::min(32767, v >> 2));
						int32_t m;
						if ((sn + 1) * 16 > rec_ref.size()) continue;
						memcpy(&m, &rec_ref[sn * 16 + 4 * c], 4);
						rec_compared++;
						if (v != m) { rec_differ++; rec_maxdiff = std::max(rec_maxdiff, std::abs(v - m)); if (c >= 2) rec_rear_differ++; }
					}
				}
			}
		}
		else board.run(ns, out);
		if (dsb && ns)
		{
			const bool pl = dsb->playing();
			if (dsb_state) dsb_state_hash = dsb->state_hash(dsb_state_hash);
			for (int i = 0; i < ns; i++)
			{
				uint32_t bl, br;
				memcpy(&bl, &dsb_l[i], 4); memcpy(&br, &dsb_r[i], 4);
				for (int k = 0; k < 4; k++) dsb_hash = (dsb_hash ^ uint8_t(bl >> (8 * k))) * 16777619u;
				for (int k = 0; k < 4; k++) dsb_hash = (dsb_hash ^ uint8_t(br >> (8 * k))) * 16777619u;
				if (dsb_pcm[2 * i] || dsb_pcm[2 * i + 1]) dsb_nonzero++;
				dsb_peak = std::max<int32_t>(dsb_peak, std::max(std::abs(dsb_pcm[2 * i]), std::abs(dsb_pcm[2 * i + 1])) >> 8);
				if (pl) dsb_playing++;
			}
			if (dsb_dump)
				for (int i = 0; i < ns; i++) { fwrite(&dsb_l[i], 4, 1, dsb_dump); fwrite(&dsb_r[i], 4, 1, dsb_dump); }
			if (!mix_ref.empty())
			{
				// Supermodel's front channels (its SCSP's, at SoundVolume 100) plus this DSB, as int32, and
				// its rear channels, against its mix after the DSB
				const size_t base = size_t(m3_frame_base) * 16;
				for (int i = 0; i < ns && base + size_t(i) * 16 + 16 <= mix_ref.size() && base + size_t(i) * 16 + 16 <= mix_pcm.size(); i++)
					for (int c = 0; c < 4; c++)
					{
						int32_t a, m;
						memcpy(&a, &mix_pcm[base + size_t(i) * 16 + 4 * c], 4);
						memcpy(&m, &mix_ref[base + size_t(i) * 16 + 4 * c], 4);
						float x = float(a) * 1.0f;
						if (c == 0) x += dsb_l[i];
						if (c == 1) x += dsb_r[i];
						mix_compared++;
						if (int32_t(x) != m) { if (mix_first == SIZE_MAX) mix_first = base / 16 + size_t(i); mix_differ++; }
					}
			}
		}
		m3_frame_base += uint64_t(ns);
		m3_frame_mark(unsigned(f));
		const double took = thread_s() - a;
		run_s += took;
		if (fr[f].samples)
		{
			const double r = took * model3_board::SAMPLE_RATE / fr[f].samples;
			if (r > worst) { worst = r; worst_f = f; }
			if (r > 1.0) over++;
			win.push_back({took, fr[f].samples});
			win_t += took; win_s += fr[f].samples;
			if (win.size() > 58) { win_t -= win.front().first; win_s -= win.front().second; win.erase(win.begin()); }
			if (win.size() == 58 && win_t * model3_board::SAMPLE_RATE / win_s > worst_win)
			{
				worst_win = win_t * model3_board::SAMPLE_RATE / win_s;
				worst_win_f = f + 1 - 58;
			}
		}
		for (int32_t v : out)
			for (int k = 0; k < 4; k++) hash = (hash ^ uint8_t(uint32_t(v) >> (8 * k))) * 16777619u;
		if (dsb)
			for (int i = 0; i < 2 * ns; i++)
				for (int k = 0; k < 3; k++) hash = (hash ^ uint8_t(uint32_t(dsb_pcm[i]) >> (8 * k))) * 16777619u;
		if (dump) fwrite(out.data(), 4, out.size(), dump);
		for (size_t i = 0; i < out.size(); i += 4)
		{
			const int32_t dl = dsb ? dsb_pcm[i / 2] / 256 : 0, dr = dsb ? dsb_pcm[i / 2 + 1] / 256 : 0;
			wav.push_back(mix(out[i] + dl, out[i + 2]));
			wav.push_back(mix(out[i + 1] + dr, out[i + 3]));
		}
		if (!ref.empty())
		{
			const size_t base = size_t(board.samples() - uint64_t(fr[f].samples)) * 16;
			for (size_t i = 0; i < out.size() && base + i * 4 + 4 <= ref.size(); i++)
			{
				int32_t r;
				memcpy(&r, &ref[base + i * 4], 4);
				compared++;
				if (r != out[i])
				{
					if (first_diff == SIZE_MAX) first_diff = base / 16 + i / 4;
					differ++;
				}
			}
		}
	}
	const double total_s = thread_s() - t0;
	if (dump) fclose(dump);
	SCSP_SetDSPLink(nullptr);
#ifdef FPGA_DSP
	SCSP_SetDSPLink(nullptr);
	delete fpga;
#endif
	const double audio_s = double(m3_frame_base) / model3_board::SAMPLE_RATE;   // (board.samples() but for -dsbonly)

	printf("timer mode %d: %zu frames, %llu samples (%.2f s of sound), %zu MIDI bytes\n", mode, fr.size(),
	       (unsigned long long)m3_frame_base, audio_s, midi_bytes);
	if (!ref.empty())
	{
		printf("against the reference: %zu channel samples compared, %zu differ", compared, differ);
		if (differ) printf(" (first at sample %zu, %.3f s)", first_diff, first_diff / 44100.0);
		printf("\n");
	}
	// the re-arm period: successive TIMA writes, in samples; a gap of more than 4 periods is a
	// pause in the sequencer (a stop, a new piece) and is left out
	if (tima.size() > 2)
	{
		std::vector<double> d;
		for (size_t i = 1; i < tima.size(); i++) d.push_back(double(tima[i].first - tima[i - 1].first));
		std::vector<double> s = d;
		std::sort(s.begin(), s.end());
		const double med = s[s.size() / 2];
		double sum = 0;
		size_t n = 0;
		std::map<int, int> hist;
		for (double x : d)
			if (x <= 4 * med) { sum += x; n++; hist[int(x)]++; }
		printf("timer A: %zu TIMA writes (values:", tima.size());
		for (auto &kv : tima_values) printf(" %02X x%d", kv.first, kv.second);
		printf("), re-arm period mean %.3f samples over %zu (median %.0f; histogram", n ? sum / n : 0.0, n, med);
		for (auto &kv : hist) if (kv.second * 1000 >= int(n)) printf(" %d:%d", kv.first, kv.second);
		int other = 0;
		for (auto &kv : hist) if (kv.second * 1000 < int(n)) other += kv.second;
		printf(" other:%d", other);
		printf(")\n");
	}
	printf("CPU: %.3f s for %.2f s of sound (board alone %.3f s): %.2f%% of real time, %.1f ms a second\n",
	       total_s, audio_s, run_s, 100.0 * run_s / audio_s, 1000.0 * run_s / audio_s);
	printf("worst frame: %.1f%% of its time in sound (frame %zu); %zu frames over 100%%\n", 100.0 * worst, worst_f, over);
	printf("worst second: %.1f%% (58 frames from frame %zu)\n", 100.0 * worst_win, worst_win_f);
	printf("output hash %08x\n", hash);
	printf("DAC18B at the end: master %d, slave %d\n", SCSP_DAC18B(0), SCSP_DAC18B(1));
	if (!rec_ref.empty())
		printf("records (the fallback, clipped as the FPGA does) against Supermodel's mix: %zu channel samples, %zu differ (%zu of them rear), by at most %d\n",
		       rec_compared, rec_differ, rec_rear_differ, rec_maxdiff);
	extern UINT64 g_scsp_quiet_samples;
	if (split.rec) printf("link: %.2f writes to the block a sample (%s; records and commands)\n",
	                      double(split.io_tx) / double(board.samples() ? board.samples() : 1),
	                      getenv("ARCSND_M3_WORDIO") ? "words" : getenv("ARCSND_M3_FULL") ? "whole records" : "changes only");
	printf("silence skip: %llu chip-samples of %llu (%.1f%%)\n", (unsigned long long)g_scsp_quiet_samples,
	       (unsigned long long)(2 * m3_frame_base), m3_frame_base ? 50.0 * double(g_scsp_quiet_samples) / double(m3_frame_base) : 0.0);
	if (dsb)
	{
		printf("DSB%d: %zu samples playing, %zu non-zero, peak %d; float output hash %08x; %llu wait-loop turns skipped\n", dsb->type(), dsb_playing,
		       dsb_nonzero, dsb_peak, dsb_hash, (unsigned long long)dsb->spin_turns());
		if (dsb_state) printf("DSB CPU state hash %08x\n", dsb_state_hash);
		if (!mix_ref.empty())
		{
			printf("DSB against Supermodel's mix: %zu channel samples compared, %zu differ", mix_compared, mix_differ);
			if (mix_differ) printf(" (first at sample %zu, %.3f s)", mix_first, mix_first / 44100.0);
			printf("\n");
		}
	}
	extern unsigned long long g_scspdsp_jit_samples, g_scspdsp_jit_fallbacks, g_scspdsp_jit_translations;
	printf("DSP program steps at the end: master %d, slave %d; translated: %llu samples, %llu translations, %llu in the interpreter\n",
	       SCSP_DSPSteps(0), SCSP_DSPSteps(1), g_scspdsp_jit_samples, g_scspdsp_jit_translations, g_scspdsp_jit_fallbacks);
	if (const char *jd = getenv("M3DSP_JITDUMP"))
	{
		// the translated DSP programs, for the cycle model: the area's address, then its bytes
		size_t n;
		const void *code = SCSPDSP_JitCode(&n);
		if (FILE *f = n ? fopen(jd, "wb") : nullptr)
		{
			const uint32_t base = uint32_t(uintptr_t(code));
			fwrite(&base, 4, 1, f);
			fwrite(code, 1, n, f);
			fclose(f);
		}
	}
	write_wav(argv[4], wav, model3_board::SAMPLE_RATE);
	printf("-> %s, %zu frames\n", argv[4], wav.size() / 2);
	return ((!ref.empty() && differ) || mix_differ) ? 1 : 0;
}
