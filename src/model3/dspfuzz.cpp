// SPDX-License-Identifier: GPL-3.0-or-later
// The translated DSP program (scspdsp_jit.cpp) against the interpreter (SCSPDSP_Step), on random
// programs, coefficients, addresses, ring sizes, registers, pending accesses and sound memory: each
// case runs both from the same state for a few samples (MIXS and EXTS new each sample, as the
// slots give them) and compares every register, EFREG, TEMP, MEMS and the whole sound memory.
// RWADDR and WRITEVAL are compared when an access is left pending (otherwise nothing reads them).
// TEMP is compared with its mirror (SCSPDSP.h), which both must keep.
//   dspfuzz [cases] [seed]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "SCSPDSP.h"

namespace {
uint64_t rng = 0x9E3779B97F4A7C15ull;
uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return uint32_t(rng >> 16); }

int compare(const _SCSPDSP &a, const _SCSPDSP &b, const uint16_t *ra, const uint16_t *rb, char *why)
{
#define C(f) if (a.f != b.f) { sprintf(why, #f " %d / %d", int(a.f), int(b.f)); return 1; }
	C(ACC) C(INPUTS) C(FRC_REG) C(Y_REG) C(ADRS_REG) C(MEMVAL) C(DEC) C(READPENDING) C(WRITEPENDING)
	if (a.READPENDING || a.WRITEPENDING) { C(RWADDR) }
	if (a.WRITEPENDING && (a.WRITEVAL & 0xffff) != (b.WRITEVAL & 0xffff)) { sprintf(why, "WRITEVAL"); return 1; }
	for (int i = 0; i < 16; i++) if (a.EFREG[i] != b.EFREG[i]) { sprintf(why, "EFREG[%d] %d / %d", i, a.EFREG[i], b.EFREG[i]); return 1; }
	for (int i = 0; i < 256; i++) if (a.TEMP[i] != b.TEMP[i]) { sprintf(why, "TEMP[%d] %x / %x (other copy %x / %x)", i, a.TEMP[i], b.TEMP[i], a.TEMP[i ^ 128], b.TEMP[i ^ 128]); return 1; }
	for (int i = 0; i < 32; i++) if (a.MEMS[i] != b.MEMS[i]) { sprintf(why, "MEMS[%d] %x / %x", i, a.MEMS[i], b.MEMS[i]); return 1; }
	for (int i = 0; i < 16; i++) if (a.MIXS[i] != b.MIXS[i]) { sprintf(why, "MIXS[%d]", i); return 1; }
	for (int i = 0; i < 0x80000; i++) if (ra[i] != rb[i]) { sprintf(why, "RAM[%05x] %04x / %04x", i, ra[i], rb[i]); return 1; }
	return 0;
}
}  // namespace

int main(int argc, char **argv)
{
	extern unsigned g_scspdsp_jit_settle;
	g_scspdsp_jit_settle = 0;           // translate at once: every case is a new program
	const int cases = argc > 1 ? atoi(argv[1]) : 2000;
	if (argc > 2) rng ^= strtoull(argv[2], nullptr, 0) * 0x2545F4914F6CDD1Dull;
	std::unique_ptr<uint16_t[]> ram0(new uint16_t[0x80000]), ra(new uint16_t[0x80000]), rb(new uint16_t[0x80000]);
	int bad = 0;
	for (int c = 0; c < cases; c++)
	{
		_SCSPDSP d0;
		SCSPDSP_Init(&d0);
		for (int i = 0; i < 0x80000; i++) ram0[i] = uint16_t(rnd());
		// the program: every field at random, with the memory flags and the length weighted to reach
		// the pipeline's corners (consecutive reads and writes, programs of 126-128 steps)
		const int len = (rnd() % 4 == 0) ? 126 + int(rnd() % 3) : int(rnd() % 129);
		for (int s = 0; s < 128; s++)
			for (int k = 0; k < 4; k++) d0.MPRO[s * 4 + k] = s < len ? uint16_t(rnd()) : 0;
		if (len && (rnd() & 1)) for (int s = 0; s < len; s++) d0.MPRO[s * 4 + 2] |= uint16_t((rnd() & 3) << 13);
		for (int i = 0; i < 64; i++) d0.COEF[i] = int16_t(rnd());
		for (int i = 0; i < 32; i++) d0.MADRS[i] = uint16_t(rnd());
		for (int i = 0; i < 128; i++) d0.TEMP[i] = d0.TEMP[i + 128] = int32_t(rnd() << 8) >> 8;   // with its mirror
		for (int i = 0; i < 32; i++) d0.MEMS[i] = int32_t(rnd() << 8) >> 8;
		d0.RBL = (8 * 1024) << (rnd() & 3);
		d0.RBP = rnd() & 0x7F;
		d0.RBLMASK = d0.RBL - 1;
		d0.RBPOFF = d0.RBP << 12;
		d0.DEC = rnd();
		d0.ACC = int32_t(rnd() << 6) >> 6;
		d0.INPUTS = int32_t(rnd() << 8) >> 8;
		d0.FRC_REG = rnd() & 0x1FFF;
		d0.Y_REG = rnd() & 0xFFFFFF;
		d0.ADRS_REG = rnd() & 0xFFF;
		d0.MEMVAL = int32_t(rnd() << 8) >> 8;
		d0.RWADDR = rnd() & 0x7FFFF;
		d0.WRITEVAL = uint16_t(rnd());
		d0.READPENDING = int(rnd() % 3);
		d0.WRITEPENDING = rnd() & 1;
		SCSPDSP_Start(&d0);
		if (rnd() % 8 == 0) { d0.LastStep = 0; }
		_SCSPDSP a = d0, b = d0;
		a.SCSPRAM = ra.get(); b.SCSPRAM = rb.get();
		memcpy(ra.get(), ram0.get(), 0x100000);
		memcpy(rb.get(), ram0.get(), 0x100000);
		const int samples = 1 + int(rnd() % 6);
		char why[96] = "";
		for (int n = 0; n < samples; n++)
		{
			int32_t mixs[16];
			int16_t exts[2];
			for (auto &m : mixs) m = int32_t(rnd() << 12) >> 12;
			for (auto &x : exts) x = int16_t(rnd());
			memcpy(a.MIXS, mixs, sizeof mixs); memcpy(b.MIXS, mixs, sizeof mixs);
			memcpy(a.EXTS, exts, sizeof exts); memcpy(b.EXTS, exts, sizeof exts);
			if (rnd() % 4 == 0) { const int i = int(rnd() % 64); a.COEF[i] = b.COEF[i] = int16_t(rnd()); }
			g_scspdsp_jit = false; SCSPDSP_Step(&a);
			g_scspdsp_jit = true; SCSPDSP_Step(&b);
			if (compare(a, b, ra.get(), rb.get(), why))
			{
				if (bad++ < 10) printf("case %d (length %d), sample %d: %s\n", c, d0.LastStep, n, why);
				break;
			}
		}
	}
	extern unsigned long long g_scspdsp_jit_samples, g_scspdsp_jit_fallbacks, g_scspdsp_jit_translations;
	printf("%d cases, %d differ; translated %llu samples (%llu translations, %llu fallbacks)\n", cases, bad,
	       g_scspdsp_jit_samples, g_scspdsp_jit_translations, g_scspdsp_jit_fallbacks);
	return bad != 0;
}
