// SPDX-License-Identifier: GPL-3.0-or-later
// arcsnd: arcade sound boards on the MiSTer's ARM, for cores that leave their sound board to the
// HPS (PROVENANCE.md). One copy runs, in the background, from the Scripts menu or at boot
// (Scripts/arcade_hybridsoundserver.sh); it watches /tmp/CORENAME and runs the session of the board the loaded set has,
// each against its core's shared block in DDR3, until the core changes:
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
#include <vector>

#include "daemon.h"

// core registration
#define ARCSND_BOARD(b) \
	void b##_sets(std::vector<const char *> &out); \
	bool b##_claims(const char *name); \
	void b##_run(int mem, bool verbose);
ARCSND_BOARD(hornet)
ARCSND_BOARD(hng64)
ARCSND_BOARD(model3)
ARCSND_BOARD(firebeat)
#undef ARCSND_BOARD

namespace {

const char *prog = "arcsnd";
constexpr int64_t IDLE_POLL_NS = 250'000'000;          // core name poll period

struct board
{
	const char *core;                                  // the .mra's <rbf>
	const char *what;                                  // for the log
	void (*sets)(std::vector<const char *> &out);
	bool (*claims)(const char *name);
	void (*run)(int mem, bool verbose);
	bool irqs_off_core0;                               // irqs_to_core1 while it plays
};

const board BOARDS[] = {
	{"KonamiHornet", "Konami Hornet", hornet_sets, hornet_claims, hornet_run, true},
	{"HyperNG64", "SNK Hyper NeoGeo 64", hng64_sets, hng64_claims, hng64_run, true},
	{"Model3", "Sega Model 3", model3_sets, model3_claims, model3_run, true},
	{"Firebeat", "Konami Keyboardmania's (Firebeat) MIDI", firebeat_sets, firebeat_claims, firebeat_run, false},
};

void on_signal(int) { arcsnd::g_stop = 1; }

// Core 0 kept for a board while it plays: every device interrupt goes to core 1, and back as it was when
// the board stops. The USB controller's all land on core 0 by default.
struct irq_move { std::string path, mask; };

std::vector<irq_move> irqs_to_core1()
{
	std::vector<irq_move> moved;
	if (DIR *d = opendir("/proc/irq"))
	{
		while (const dirent *e = readdir(d))
		{
			if (e->d_name[0] < '0' || e->d_name[0] > '9') {
				continue;
			}

			const std::string path = std::string("/proc/irq/") + e->d_name + "/smp_affinity";
			char was[32] = {};
			const int r = open(path.c_str(), O_RDONLY);
			if (r < 0) {
				continue;
			}

			const ssize_t n = read(r, was, sizeof was - 1);
			close(r);
			if (n <= 0) {
				continue;
			}

			const int w = open(path.c_str(), O_WRONLY);
			if (w < 0) {
				continue;
			}
			if (write(w, "2\n", 2) == 2) {
				moved.push_back({path, std::string(was, n)});
			}
			close(w);
		}
		closedir(d);
	}
	fprintf(stderr, "%s: %zu interrupts on core 1 while the board plays\n", prog, moved.size());
	return moved;
}

void irqs_back(const std::vector<irq_move> &moved)
{
	for (const irq_move &m : moved)
	{
		const int w = open(m.path.c_str(), O_WRONLY);
		if (w < 0) {
			continue;
		}
		if (write(w, m.mask.data(), m.mask.size()) < 0) {}
		close(w);
	}
	fprintf(stderr, "%s: %zu interrupts back where they were\n", prog, moved.size());
}

// ensure only one copy running
bool only_copy()
{
	const int lock = open("/tmp/arcsnd.lock", O_RDWR | O_CREAT, 0644);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "%s: another copy is running\n", prog);
		return false;
	}
	fprintf(stderr, "%s: the only copy\n", prog);
	return true;
}

// nice and affinity on core 0
void set_priority_and_cpu()
{
	if (setpriority(PRIO_PROCESS, 0, -10) != 0) {
		perror("setpriority");
	}
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	if (sched_setaffinity(0, sizeof cpus, &cpus) != 0) {
		perror("sched_setaffinity");
	}
	fprintf(stderr, "%s: priority and CPU set\n", prog);
}

// each core (the .mra's <rbf>) with the sets it plays
void print_game_titles()
{
	fprintf(stderr, "\nGame titles supported:\n");
	for (const board &b : BOARDS)
	{
		std::vector<const char *> sets;
		b.sets(sets);
		fprintf(stderr, "%s: ", b.core);
		for (size_t i = 0; i < sets.size(); i++) {
			fprintf(stderr, i ? ", %s" : "%s", sets[i]);
		}
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "\n");
}

}

int main(int argc, char **argv)
{
	if (argc > 0 && argv[0] && argv[0][0]) {
		const char *slash = strrchr(argv[0], '/');
		prog = slash ? slash + 1 : argv[0];
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	bool verbose = false, crash = false;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-v")) {
			verbose = true;
		} else if (!strcmp(argv[i], "-crash")) {
			crash = true;
		}
	}

	print_game_titles();

	const int mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0) { 
		perror("/dev/mem"); return 1; 
	}
	fprintf(stderr, "%s: /dev/mem open\n", prog);

	if (!only_copy()) {
		return 1;
	}
	set_priority_and_cpu();

	if (crash) {
		*static_cast<volatile int *>(nullptr) = 0;
	}

	// main loop
	std::string last = "(none)";
	while (!arcsnd::g_stop)
	{
		const arcsnd::core_name c = arcsnd::read_core_name();
		if (c.name != last) {
			fprintf(stderr, "%s: core \"%s\"\n", prog, c.name.c_str());
		}
		last = c.name;

		const board *b = nullptr;
		for (const board &k : BOARDS) {
			if (k.claims(c.name.c_str())) {
				b = &k; break; 
			}
		}
		if (!b) { 
			arcsnd::sleep_ns(IDLE_POLL_NS); continue;
		}

		fprintf(stderr, "%s: %s, %s\n", prog, c.name.c_str(), b->what);

		std::vector<irq_move> moved;
		if (b->irqs_off_core0) {
			moved = irqs_to_core1();
		}
		b->run(mem, verbose);
		if (b->irqs_off_core0) {
			irqs_back(moved);
		}
	}
	return 0;
}
