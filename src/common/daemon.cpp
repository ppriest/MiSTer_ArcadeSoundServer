// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.h's pieces, from hng64snd's mister.cpp.

#include "daemon.h"

#include <execinfo.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if __has_include("arcsnd_build.h")
#include "arcsnd_build.h"
#else
#define ARCSND_BUILD "unknown"
#endif
#ifndef ARCSND_DATE
#define ARCSND_DATE "unknown"
#endif

extern "C" char __executable_start, etext;     // the linker's: the static binary's code

namespace arcsnd {

volatile sig_atomic_t g_stop = 0;
const char *const BUILD = ARCSND_BUILD;

namespace {

// A crash's report on stderr (/tmp/arcsnd.log from the start script): the signal, the faulting
// address, pc, lr and sp, the unwinder's return addresses, and the words on the stack that point
// into the code, for `addr2line -f -e arcsnd` on this build's unstripped binary. Only write() and
// formatting here: the handler may run with the heap broken.
void put(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	if (write(2, s, n) < 0) {}
}

void put_hex(uint32_t v)
{
	char b[12] = " 0x";
	for (int i = 0; i < 8; i++) b[3 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
	put(b);
}

void on_crash(int sig, siginfo_t *si, void *ctx)
{
	const mcontext_t &m = static_cast<ucontext_t *>(ctx)->uc_mcontext;
	put("arcsnd "); put(BUILD); put(": signal"); put_hex(sig); put(" code"); put_hex(si->si_code);
	put(" addr"); put_hex(uint32_t(uintptr_t(si->si_addr)));
	put(" pc"); put_hex(m.arm_pc); put(" lr"); put_hex(m.arm_lr); put(" sp"); put_hex(m.arm_sp); put("\n");
	void *bt[24];
	const int n = backtrace(bt, 24);
	put("arcsnd: backtrace");
	for (int i = 0; i < n; i++) put_hex(uint32_t(uintptr_t(bt[i])));
	put("\narcsnd: stack");
	const uint32_t lo = uint32_t(uintptr_t(&__executable_start)), hi = uint32_t(uintptr_t(&etext));
	const uint32_t *sp = reinterpret_cast<const uint32_t *>(m.arm_sp);
	for (int i = 0, k = 0; i < 256 && k < 16; i++)
		if (sp[i] >= lo && sp[i] < hi) { put_hex(sp[i]); k++; }
	put("\n");
	signal(sig, SIG_DFL);
	raise(sig);
}

// before main and every other static constructor, so that a crash in either is reported; on a
// stack of its own, so that an overflow is too
__attribute__((constructor(101))) void install_crash_report()
{
	static char alt[32768];
	stack_t ss{};
	ss.ss_sp = alt;
	ss.ss_size = sizeof alt;
	sigaltstack(&ss, nullptr);
	struct sigaction sa{};
	sa.sa_sigaction = on_crash;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);
	void *bt[1];
	backtrace(bt, 1);                    // the unwinder's first use outside the handler
	put("Arcade Sound Server - Paul Priest\nBuild: " ARCSND_DATE " ("); put(BUILD); put(")\n");
}

}  // namespace

int64_t now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

int64_t cpu_ns()
{
	timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

void sleep_ns(int64_t ns)
{
	timespec ts{time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)};
	nanosleep(&ts, nullptr);
}

core_name read_core_name()
{
	core_name c;
	struct stat st;
	if (stat("/tmp/CORENAME", &st) != 0) return c;
	c.written = int64_t(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
	char name[64] = {};
	FILE *f = fopen("/tmp/CORENAME", "r");
	if (!f) return c;
	const size_t n = fread(name, 1, sizeof name - 1, f);
	fclose(f);
	name[n] = 0;
	name[strcspn(name, "\r\n")] = 0;
	c.name = name;
	return c;
}

void *map(int mem, uint32_t addr, uint32_t size)
{
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, addr);
	return p == MAP_FAILED ? nullptr : p;
}

void unmap(void *p, uint32_t size)
{
	if (p) munmap(p, size);
}

void copy_in(void *dst, const volatile uint8_t *src, size_t n)
{
	auto *d = static_cast<uint32_t *>(dst);
	auto *s = reinterpret_cast<const volatile uint32_t *>(src);
	for (size_t i = 0; i < n / 4; i++) d[i] = s[i];
}

void resampler::run(const std::vector<int32_t> &in, size_t frames, int stride, int li, int ri, std::vector<int16_t> &out)
{
	for (size_t i = 0; i < frames; i++)
	{
		const int32_t l = in[i * stride + li];
		const int32_t r = in[i * stride + ri];
		while (phase < 1.0)
		{
			const double a = prev_l + (l - prev_l) * phase;
			const double b = prev_r + (r - prev_r) * phase;
			out.push_back(int16_t(std::max(-32768.0, std::min(32767.0, a))));
			out.push_back(int16_t(std::max(-32768.0, std::min(32767.0, b))));
			phase += step;
		}
		phase -= 1.0;
		prev_l = l;
		prev_r = r;
	}
}

void audio_out::open_dev()
{
	if (fd >= 0) return;
	fd = open("/dev/MrAudio", O_WRONLY | O_NONBLOCK);
	if (fd < 0) perror("/dev/MrAudio");
	level_t = now_ns();
}

void audio_out::close_dev()
{
	if (fd >= 0) close(fd);
	fd = -1;
}

void audio_out::drain(bool playing)
{
	const int64_t t = now_ns();
	const double played = (t - level_t) * PLAY_PER_NS;
	if (playing && played > level)
	{
		if (level > 0) gaps++;
		gap_ns += int64_t((played - level) / PLAY_PER_NS);
	}
	level = std::max(0.0, level - played);
	level_t = t;
}

void audio_out::put(const std::vector<int16_t> &pcm, bool now)
{
	if (pcm.empty()) return;
	outq.insert(outq.end(), pcm.begin(), pcm.end());
	last_l = pcm[pcm.size() - 2];
	last_r = pcm[pcm.size() - 1];
	if (!now && outq.size() < 2 * 240) return;
	const ssize_t n = fd < 0 ? 0 : write(fd, outq.data(), outq.size() * sizeof(int16_t));
	if (n < 0) perror("MrAudio");
	else level += double(n / 4);
	outq.clear();
}

void audio_out::fade()
{
	std::vector<int16_t> f;
	for (int i = 1; i <= 240; i++)
	{
		f.push_back(int16_t(last_l * (240 - i) / 240));
		f.push_back(int16_t(last_r * (240 - i) / 240));
	}
	put(f, true);
}

}  // namespace arcsnd
