// SPDX-License-Identifier: GPL-3.0-or-later
// arcsnd: arcade sound boards on the MiSTer's ARM, for cores that leave their sound board to the
// HPS (PROVENANCE.md). One copy runs, in the background, from the Scripts menu or at boot
// (Scripts/arcade_hybridsoundserver.sh); it watches /tmp/CORENAME and runs the session of the board the loaded set has,
// each against its core's shared block in DDR3, until the core changes:
//
//   hornet  Konami Hornet (Gradius IV): hornet/session.cpp, Arcade-KonamiHornet_MiSTer's
//           rtl/mister/hornet_sndbridge.sv
//   hng64   SNK Hyper NeoGeo 64: hng64/session.cpp, Arcade-HyperNG64_MiSTer's rtl/hng64_sndbridge.sv
//   model3  Sega Model 3: model3/session.cpp, SegaModel3_MiSTer's rtl/sound/m3_sndbridge.sv
//           (added for my Model 3 core: its 68000 and SCSP slots run here, the SCSP DSPs in the FPGA)
// (docs/PROTOCOL.md)
//
//   arcsnd [-v] [-crash]   on stderr always: the build and its date, the sets it plays, each
//                          startup step, each core name, each board's start and stop
//                          -v: a status line every 5 s as well (the scripts leave it out: it grows
//                          /tmp/arcsnd.log for as long as a board plays)
//                          -crash: crash after starting up, to check the crash report

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "daemon.h"

void hornet_sets(std::vector<const char *> &out);
void hng64_sets(std::vector<const char *> &out);
bool hornet_claims(const char *name);
void hornet_run(int mem, bool verbose);
bool hng64_claims(const char *name);
void hng64_run(int mem, bool verbose);
// the Model 3 board, dispatched like the other two; its sets go into the banner's list
void model3_sets(std::vector<const char *> &out);
bool model3_claims(const char *name);
void model3_run(int mem, bool verbose);

namespace {
void on_signal(int) { arcsnd::g_stop = 1; }

// Core 0 kept for a board while it plays: every device interrupt goes to core 1, and back as it was when
// the board stops. The USB controller's all land on core 0 by default, and a Sinden gun's camera (about
// 12,000 a second) starved the Model 3 board into its core's 93 ms watchdog, 18 resets in 45 s; on core
// 1, none. The per-CPU interrupts refuse the write and stay where they are.
struct irq_move { std::string path, mask; };
std::vector<irq_move> irqs_to_core1()
{
	std::vector<irq_move> moved;
	if (DIR *d = opendir("/proc/irq"))
	{
		while (const dirent *e = readdir(d))
		{
			if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
			const std::string path = std::string("/proc/irq/") + e->d_name + "/smp_affinity";
			char was[32] = {};
			const int r = open(path.c_str(), O_RDONLY);
			if (r < 0) continue;
			const ssize_t n = read(r, was, sizeof was - 1);
			close(r);
			if (n <= 0) continue;
			const int w = open(path.c_str(), O_WRONLY);
			if (w < 0) continue;
			if (write(w, "2\n", 2) == 2) moved.push_back({path, std::string(was, n)});
			close(w);
		}
		closedir(d);
	}
	fprintf(stderr, "arcsnd: %zu interrupts on core 1 while the board plays\n", moved.size());
	return moved;
}
void irqs_back(const std::vector<irq_move> &moved)
{
	for (const irq_move &m : moved)
	{
		const int w = open(m.path.c_str(), O_WRONLY);
		if (w < 0) continue;
		if (write(w, m.mask.data(), m.mask.size()) < 0) {}
		close(w);
	}
	fprintf(stderr, "arcsnd: %zu interrupts back where they were\n", moved.size());
}
}

int main(int argc, char **argv)
{
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	bool verbose = false, crash = false;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-v")) verbose = true;
		else if (!strcmp(argv[i], "-crash")) crash = true;
	}
	// each set with the core (the .mra's <rbf>) that runs it
	fprintf(stderr, "Game titles supported:\n");
	for (const auto &[core, list] : {std::pair{"KonamiHornet", hornet_sets}, std::pair{"HyperNG64", hng64_sets}, std::pair{"Model3", model3_sets}})
	{
		std::vector<const char *> sets;
		list(sets);
		for (const char *s : sets) fprintf(stderr, "%-10s  %s\n", s, core);
	}

	const int mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0) { perror("/dev/mem"); return 1; }
	fprintf(stderr, "arcsnd: /dev/mem open\n");

	// one copy: two writing /dev/MrAudio interleave
	const int lock = open("/tmp/arcsnd.lock", O_RDWR | O_CREAT, 0644);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0)
	{
		fprintf(stderr, "arcsnd: another copy is running\n");
		return 1;
	}
	fprintf(stderr, "arcsnd: the only copy\n");

	// ahead of the daemons, not of the system: a real-time priority, once the process fell behind,
	// starved Main_MiSTer and sshd (HyperNG64, LESSONS_LEARNED). Core 0: Main_MiSTer's main thread
	// is pinned to core 1 and takes most of it.
	if (setpriority(PRIO_PROCESS, 0, -10) != 0) perror("setpriority");
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	if (sched_setaffinity(0, sizeof cpus, &cpus) != 0) perror("sched_setaffinity");
	fprintf(stderr, "arcsnd: priority and CPU set\n");
	if (crash) *static_cast<volatile int *>(nullptr) = 0;

	std::string last = "(none)";
	while (!arcsnd::g_stop)
	{
		const arcsnd::core_name c = arcsnd::read_core_name();
		if (c.name != last) fprintf(stderr, "arcsnd: core \"%s\"\n", c.name.c_str());
		last = c.name;
		if (hornet_claims(c.name.c_str()))
		{
			fprintf(stderr, "arcsnd: %s, the Hornet board\n", c.name.c_str());
			const auto moved = irqs_to_core1();
			hornet_run(mem, verbose);
			irqs_back(moved);
		}
		else if (hng64_claims(c.name.c_str()))
		{
			fprintf(stderr, "arcsnd: %s, the hng64 board\n", c.name.c_str());
			const auto moved = irqs_to_core1();
			hng64_run(mem, verbose);
			irqs_back(moved);
		}
		else if (model3_claims(c.name.c_str()))
		{
			fprintf(stderr, "arcsnd: %s, the Model 3 sound board\n", c.name.c_str());
			const auto moved = irqs_to_core1();
			model3_run(mem, verbose);
			irqs_back(moved);
		}
		else arcsnd::sleep_ns(250'000'000);
	}
	return 0;
}
