// SPDX-License-Identifier: GPL-3.0-or-later
// The SCSP DSP's program translated to ARM (A32, ARMv7-A: the MiSTer's Cortex-A9), one function a
// program, made again whenever the program changes. It computes exactly what SCSPDSP_Step
// (SCSPDSP.cpp) computes, step for step: the instruction fields are decoded once, the work a step's
// fields switch off is not emitted, and the sound memory pipeline (an MRD or MWT starts an access
// that the next step does, a read before a waiting write) is followed at translation time, since
// which access is pending at each step depends only on the program. The run-time inputs stay in
// the DSP's struct and are read where the interpreter reads them (COEF, MADRS, RBL, RBP, TEMP,
// MEMS, MIXS, EXTS), so the 68000 may change any of them between samples.
//
// The pending state at a sample's first step is what the previous sample left: the program alone
// decides it, except just after a change of program. A translation is made for each entry state
// met (there are six at most) and cached with the program's generation. A program that is being
// written (it changed less than g_scspdsp_jit_settle samples ago) runs in the interpreter.
//
// Registers through the program: r0 the struct, r1 ACC, r2 INPUTS, r3 FRC_REG, r4 Y_REG,
// r5 ADRS_REG, r6 MEMVAL, r7 DEC, r8 &TEMP[DEC & 7F] (TEMP is mirrored, SCSPDSP.h, so a read is one
// load), r9 SHIFTED, r10-r12 and lr scratch.
// RWADDR and WRITEVAL live in the struct (WRITEVAL written as a zero-extended word; the two bytes
// after it are padding).

#include "SCSPDSP.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

bool g_scspdsp_jit = true;
unsigned long long g_scspdsp_jit_samples = 0, g_scspdsp_jit_fallbacks = 0, g_scspdsp_jit_translations = 0;

#if defined(__arm__) && !defined(__thumb__)

#include <sys/mman.h>

namespace {

enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP = 13, LR = 14, PC = 15 };
enum { ST = R0, ACC = R1, INP = R2, FRC = R3, YR = R4, ADR = R5, MEMV = R6, DECR = R7, TB = R8, SH = R9 };
enum { T1 = R10, T2 = R11, T3 = R12, T4 = LR };
enum Cond { EQ = 0, NE = 1, MI = 4, PL = 5, GE = 10, LT = 11, GT = 12, LE = 13, AL = 14 };
enum Op { AND = 0, EOR = 1, SUB = 2, RSB = 3, ADD = 4, CMP = 10, ORR = 12, MOV = 13, BIC = 14, MVN = 15 };
enum Shift { LSL = 0, LSR = 1, ASR = 2 };

#define OFF(f) uint32_t(offsetof(_SCSPDSP, f))

struct emitter
{
	std::vector<uint32_t> code;
	void w(uint32_t x) { code.push_back(x); }

	static bool rot_imm(uint32_t v, uint32_t &enc)
	{
		for (uint32_t r = 0; r < 16; r++)
		{
			const uint32_t x = r ? (v << (2 * r)) | (v >> (32 - 2 * r)) : v;
			if (x < 256) { enc = r << 8 | x; return true; }
		}
		return false;
	}
	// data processing: register operand with an immediate shift
	void dp(Op op, int rd, int rn, int rm, Shift sh = LSL, int amt = 0, Cond c = AL, bool s = false)
	{
		const bool test = op == CMP;
		w(uint32_t(c) << 28 | uint32_t(op) << 21 | uint32_t(s || test) << 20 | uint32_t(rn) << 16 |
		  uint32_t(test ? 0 : rd) << 12 | uint32_t(amt & 31) << 7 | uint32_t(sh) << 5 | uint32_t(rm));
	}
	// data processing: register operand shifted by a register
	void dpr(Op op, int rd, int rn, int rm, Shift sh, int rs, Cond c = AL)
	{
		w(uint32_t(c) << 28 | uint32_t(op) << 21 | uint32_t(rn) << 16 | uint32_t(rd) << 12 | uint32_t(rs) << 8 |
		  uint32_t(sh) << 5 | 1u << 4 | uint32_t(rm));
	}
	// data processing with an immediate; false when it does not encode
	bool dpi(Op op, int rd, int rn, uint32_t v, Cond c = AL, bool s = false)
	{
		uint32_t enc;
		if (!rot_imm(v, enc)) return false;
		const bool test = op == CMP;
		w(uint32_t(c) << 28 | 1u << 25 | uint32_t(op) << 21 | uint32_t(s || test) << 20 | uint32_t(rn) << 16 |
		  uint32_t(test ? 0 : rd) << 12 | enc);
		return true;
	}
	void ldr(int rt, int rn, uint32_t off) { w(0xE5900000u | uint32_t(rn) << 16 | uint32_t(rt) << 12 | off); }
	void str(int rt, int rn, uint32_t off) { w(0xE5800000u | uint32_t(rn) << 16 | uint32_t(rt) << 12 | off); }
	// word store, offset -4095..4095, conditional
	void str_s(int rt, int rn, int off, Cond c) { w(uint32_t(c) << 28 | 0x05000000u | (off >= 0 ? 1u << 23 : 0) | uint32_t(rn) << 16 | uint32_t(rt) << 12 | uint32_t(off >= 0 ? off : -off)); }
	void strb(int rt, int rn, uint32_t off) { w(0xE5C00000u | uint32_t(rn) << 16 | uint32_t(rt) << 12 | off); }
	// halfword forms (op4 0xB: LDRH / STRH, 0xF: LDRSH); an offset past 255 goes through lr
	void half(bool load, int op4, int rt, int rn, uint32_t off)
	{
		if (off > 255)
		{
			dpi(ADD, T4, rn, off & ~0xFFu);
			rn = T4;
			off &= 0xFF;
		}
		w(0xE1C00000u | uint32_t(load) << 20 | uint32_t(rn) << 16 | uint32_t(rt) << 12 | (off >> 4) << 8 | uint32_t(op4) << 4 | (off & 15));
	}
	void ldrh(int rt, int rn, uint32_t off) { half(true, 0xB, rt, rn, off); }
	void ldrsh(int rt, int rn, uint32_t off) { half(true, 0xF, rt, rn, off); }
	void strh(int rt, int rn, uint32_t off) { half(false, 0xB, rt, rn, off); }
	void sbfx(int rd, int rn, int lsb, int width) { w(0xE7A00050u | uint32_t(width - 1) << 16 | uint32_t(rd) << 12 | uint32_t(lsb) << 7 | uint32_t(rn)); }
	void ubfx(int rd, int rn, int lsb, int width) { w(0xE7E00050u | uint32_t(width - 1) << 16 | uint32_t(rd) << 12 | uint32_t(lsb) << 7 | uint32_t(rn)); }
	void ssat(int rd, int sat, int rn, int lsl) { w(0xE6A00010u | uint32_t(sat - 1) << 16 | uint32_t(rd) << 12 | uint32_t(lsl) << 7 | uint32_t(rn)); }
	void smull(int lo, int hi, int rn, int rm) { w(0xE0C00090u | uint32_t(hi) << 16 | uint32_t(lo) << 12 | uint32_t(rm) << 8 | uint32_t(rn)); }
	void clz(int rd, int rm) { w(0xE16F0F10u | uint32_t(rd) << 12 | uint32_t(rm)); }
	void sxth(int rd, int rm) { w(0xE6BF0070u | uint32_t(rd) << 12 | uint32_t(rm)); }
	void push(uint32_t list) { w(0xE92D0000u | list); }
	void pop(uint32_t list) { w(0xE8BD0000u | list); }
};

// one SCSPDSP_Step: entered with the pending accesses (rp, wp), leaves them in (rp, wp)
void translate(const _SCSPDSP *DSP, int &rp, bool &wp, emitter &e)
{
	e.push(0x0FF0u | 1u << LR);                      // r4-r11, lr
	e.ldr(ACC, ST, OFF(ACC));
	e.ldr(INP, ST, OFF(INPUTS));
	e.ldr(FRC, ST, OFF(FRC_REG));
	e.ldr(YR, ST, OFF(Y_REG));
	e.ldr(ADR, ST, OFF(ADRS_REG));
	e.ldr(MEMV, ST, OFF(MEMVAL));
	e.ldr(DECR, ST, OFF(DEC));
	e.dpi(AND, T1, DECR, 0x7F);
	e.dp(ADD, TB, ST, T1, LSL, 2);
	if (!e.dpi(ADD, TB, TB, OFF(TEMP)))
	{
		e.dpi(ADD, TB, TB, OFF(TEMP) & 0xF00);
		e.dpi(ADD, TB, TB, OFF(TEMP) & 0xFF);
	}

	// DoMemoryAccess, the pending state known here; uses T1, T2, T4
	auto memory_access = [&] {
		if (rp)
		{
			e.ldr(T1, ST, OFF(RWADDR));
			e.ldr(T2, ST, OFF(SCSPRAM));
			e.dp(ADD, T1, T2, T1, LSL, 1);
			e.w(0xE1D000B0u | uint32_t(T1) << 16 | uint32_t(T2) << 12);       // ldrh T2, [T1]
			if (rp == 2)
			{
				e.sxth(T2, T2);
				e.dp(MOV, MEMV, 0, T2, LSL, 8);
			}
			else
			{
				// UNPACK: sign T1, exponent T4, mantissa T2
				e.dp(MOV, T1, 0, T2, LSR, 15);
				e.ubfx(T4, T2, 11, 4);
				e.ubfx(T2, T2, 0, 11);
				e.dp(MOV, T2, 0, T2, LSL, 11);
				e.dp(ORR, T2, T2, T1, LSL, 23);
				e.dpi(CMP, 0, T4, 11);
				e.dpi(MOV, T4, 0, 11, GT);
				e.dp(ORR, T2, T2, T1, LSL, 22, GT);
				e.dpi(EOR, T1, T1, 1, LE);
				e.dp(ORR, T2, T2, T1, LSL, 22, LE);
				e.sbfx(T2, T2, 0, 24);
				e.dpr(MOV, MEMV, 0, T2, ASR, T4);
			}
			rp = 0;
		}
		else if (wp)
		{
			e.ldr(T1, ST, OFF(RWADDR));
			e.ldr(T2, ST, OFF(SCSPRAM));
			e.dp(ADD, T1, T2, T1, LSL, 1);
			e.ldr(T2, ST, OFF(WRITEVAL));
			e.w(0xE1C000B0u | uint32_t(T1) << 16 | uint32_t(T2) << 12);       // strh T2, [T1]
			wp = false;
		}
	};
	// RWADDR = ((T1 & RBLMASK or already masked) + RBPOFF) & 0x7FFFF; uses T2
	auto store_rwaddr = [&](bool masked) {
		if (!masked)
		{
			e.ldr(T2, ST, OFF(RBLMASK));
			e.dp(AND, T1, T1, T2);
		}
		e.ldr(T2, ST, OFF(RBPOFF));
		e.dp(ADD, T1, T1, T2);
		e.ubfx(T1, T1, 0, 19);
		e.str(T1, ST, OFF(RWADDR));
	};

	const int last = DSP->LastStep;
	// a step's multiply-accumulate is dead when the next step neither takes SHIFTED (made from ACC)
	// nor adds ACC, since every step writes ACC; after the last step the empty steps overwrite ACC
	// (when there are any: a program of 128 steps hands it to the next sample)
	auto takes_acc = [&](int k) {
		const UINT16 *p = DSP->MPRO + k * 4;
		const bool sh = ((p[2] >> 6) & 1) || ((p[2] >> 12) & 1) || ((p[0] >> 7) & 1) || ((p[2] >> 14) & 1) ||
		                (((p[2] >> 7) & 1) && ((p[2] >> 4) & 3) == 3);
		return sh || ((p[2] & 1) && !((p[2] >> 1) & 1));
	};
	for (int step = 0; step < last; ++step)
	{
		const bool mac_live = step + 1 < last ? takes_acc(step + 1) : last == 128;
		const UINT16 *IPtr = DSP->MPRO + step * 4;
		const UINT32 TRA = (IPtr[0] >> 8) & 0x7F, TWT = (IPtr[0] >> 7) & 1, TWA = IPtr[0] & 0x7F;
		const UINT32 XSEL = (IPtr[1] >> 15) & 1, YSEL = (IPtr[1] >> 13) & 3, IRA = (IPtr[1] >> 6) & 0x3F;
		const UINT32 IWT = (IPtr[1] >> 5) & 1, IWA = IPtr[1] & 0x1F;
		const UINT32 TABLE = (IPtr[2] >> 15) & 1, MWT = (IPtr[2] >> 14) & 1, MRD = (IPtr[2] >> 13) & 1;
		const UINT32 EWT = (IPtr[2] >> 12) & 1, EWA = (IPtr[2] >> 8) & 0xF, ADRL = (IPtr[2] >> 7) & 1;
		const UINT32 FRCL = (IPtr[2] >> 6) & 1, SHIFT = (IPtr[2] >> 4) & 3, YRL = (IPtr[2] >> 3) & 1;
		const UINT32 NEGB = (IPtr[2] >> 2) & 1, ZERO = (IPtr[2] >> 1) & 1, BSEL = IPtr[2] & 1;
		const UINT32 COEF = (IPtr[3] >> 9) & 0x3f, NOFL = (IPtr[3] >> 8) & 1, MASA = (IPtr[3] >> 2) & 0x1f;
		const UINT32 ADREB = (IPtr[3] >> 1) & 1, NXADR = IPtr[3] & 1;

		// input
		if (IRA <= 0x1f) { e.ldr(INP, ST, OFF(MEMS) + 4 * IRA); e.sbfx(INP, INP, 0, 24); }
		else if (IRA <= 0x2f) { e.ldr(INP, ST, OFF(MIXS) + 4 * (IRA - 0x20)); e.sbfx(INP, INP, 0, 20); e.dp(MOV, INP, 0, INP, LSL, 4); }
		else if (IRA <= 0x31) { e.ldrsh(INP, ST, OFF(EXTS) + 2 * (IRA - 0x30)); e.dp(MOV, INP, 0, INP, LSL, 8); }

		// Y in T1, from the registers before this step (only the multiply takes it)
		if (!mac_live) {}
		else if (YSEL == 0) e.sbfx(T1, FRC, 0, 13);
		else if (YSEL == 1) { e.ldrsh(T1, ST, OFF(COEF) + 2 * COEF); e.dp(MOV, T1, 0, T1, ASR, 3); }
		else if (YSEL == 2) e.sbfx(T1, YR, 11, 13);
		else e.ubfx(T1, YR, 4, 12);

		if (YRL) e.dpi(BIC, YR, INP, 0xFF000000u);

		// SHIFTED, if anything takes it
		if (FRCL || EWT || TWT || MWT || (ADRL && SHIFT == 3))
		{
			if (SHIFT == 0) e.ssat(SH, 24, ACC, 0);
			else if (SHIFT == 1) e.ssat(SH, 24, ACC, 1);
			else if (SHIFT == 2) { e.dp(MOV, SH, 0, ACC, LSL, 1); e.sbfx(SH, SH, 0, 24); }
			else e.sbfx(SH, ACC, 0, 24);
		}
		if (FRCL)
		{
			if (SHIFT == 3) e.ubfx(FRC, SH, 0, 12);
			else e.ubfx(FRC, SH, 11, 13);
		}

		// multiply and accumulate (unless dead); TEMP in T2 if taken
		if (mac_live)
		{
			if (!XSEL || (!BSEL && !ZERO))
			{
				e.ldr(T2, TB, 4 * TRA);                 // TEMP[(TRA + DEC) & 7F], from the mirror
				e.sbfx(T2, T2, 0, 24);
			}
			e.smull(T3, T4, XSEL ? int(INP) : int(T2), T1);
			e.dp(MOV, T3, 0, T3, LSR, 12);
			e.dp(ORR, T3, T3, T4, LSL, 20);
			if (!ZERO)
			{
				e.dp(NEGB ? SUB : ADD, ACC, T3, BSEL ? int(ACC) : int(T2));
				e.sbfx(ACC, ACC, 0, 26);
			}
			else e.sbfx(ACC, T3, 0, 26);
		}

		if (EWT) { e.dp(MOV, T1, 0, SH, ASR, 8); e.strh(T1, ST, OFF(EFREG) + 2 * EWA); }
		if (TWT)
		{
			// both copies: TEMP + (DEC & 7F) + TWA is one, the other 128 entries above or below it
			e.str(SH, TB, 4 * TWA);
			e.dpi(AND, T1, DECR, 0x7F);
			e.dpi(ADD, T1, T1, TWA);
			e.w(0xE3100080u | uint32_t(T1) << 16);  // tst T1, #128
			e.str_s(SH, TB, int(4 * TWA) - 512, NE);
			e.str_s(SH, TB, int(4 * TWA) + 512, EQ);
		}
		if (IWT) e.str(MEMV, ST, OFF(MEMS) + 4 * IWA);

		memory_access();

		// the address, wanted only when an access is pending at the next step
		if (MRD || MWT || wp)
		{
			e.ldrh(T1, ST, OFF(MADRS) + 2 * MASA);
			if (NXADR) e.dpi(ADD, T1, T1, 1);
			if (ADREB) { e.sbfx(T2, ADR, 0, 12); e.dp(ADD, T1, T1, T2); }
			if (!TABLE) e.dp(ADD, T1, T1, DECR);
			else e.ubfx(T1, T1, 0, 16);
			store_rwaddr(TABLE != 0);
		}
		if (MRD) rp = NOFL ? 2 : 1;
		if (MWT)
		{
			wp = true;
			if (NOFL) e.ubfx(T1, SH, 8, 16);
			else
			{
				// PACK: exponent T1, shift T2, result T4
				e.dp(EOR, T1, SH, SH, LSL, 1);
				e.dpi(BIC, T1, T1, 0xFF000000u);
				e.clz(T1, T1);
				e.dpi(SUB, T1, T1, 8);
				e.dpi(CMP, 0, T1, 12);
				e.dpi(MOV, T1, 0, 12, GT);
				e.dpi(RSB, T2, T1, 11, AL, true);
				e.dpi(MOV, T2, 0, 0, MI);
				e.dpr(MOV, T4, 0, SH, ASR, T2);
				e.ubfx(T4, T4, 0, 11);
				e.dp(ORR, T4, T4, T1, LSL, 11);
				e.dpi(AND, T2, SH, 0x800000u);
				e.dp(ORR, T1, T4, T2, LSR, 8);
			}
			e.str(T1, ST, OFF(WRITEVAL));
		}
		if (ADRL)
		{
			if (SHIFT == 3) e.ubfx(ADR, SH, 12, 12);
			else e.ubfx(ADR, INP, 16, 12);
		}
	}

	// the empty steps after the program, as the interpreter runs them (two at most)
	for (int step = last; step < 128 && step < last + 2; ++step)
	{
		memory_access();
		if (rp || wp)
		{
			e.ldrh(T1, ST, OFF(MADRS));
			e.dp(ADD, T1, T1, DECR);
			store_rwaddr(false);
		}
		e.ldr(INP, ST, OFF(MEMS));
		e.sbfx(INP, INP, 0, 24);
		e.ldr(T2, TB, 0);
		e.sbfx(T2, T2, 0, 24);
		e.sbfx(T1, FRC, 0, 13);
		e.smull(T3, T4, T2, T1);
		e.dp(MOV, T3, 0, T3, LSR, 12);
		e.dp(ORR, T3, T3, T4, LSL, 20);
		e.dp(ADD, ACC, T3, T2);
		e.sbfx(ACC, ACC, 0, 26);
	}

	e.str(ACC, ST, OFF(ACC));
	e.str(INP, ST, OFF(INPUTS));
	e.str(FRC, ST, OFF(FRC_REG));
	e.str(YR, ST, OFF(Y_REG));
	e.str(ADR, ST, OFF(ADRS_REG));
	e.str(MEMV, ST, OFF(MEMVAL));
	e.dpi(SUB, DECR, DECR, 1);
	e.str(DECR, ST, OFF(DEC));
	e.dpi(MOV, T1, 0, uint32_t(rp));
	e.str(T1, ST, OFF(READPENDING));
	e.dpi(MOV, T1, 0, wp ? 1u : 0u);
	e.strb(T1, ST, OFF(WRITEPENDING));
	e.dpi(MOV, T1, 0, 0);
	for (int i = 0; i < 16; i++) e.str(T1, ST, OFF(MIXS) + 4 * i);
	e.pop(0x0FF0u | 1u << PC);
}

struct variant
{
	const _SCSPDSP *dsp = nullptr;
	UINT32 gen = 0;
	int entry_rp = 0;
	bool entry_wp = false;
	void (*fn)(_SCSPDSP *) = nullptr;
};

constexpr int VARIANTS = 16;                // two chips, a few entry states each
constexpr size_t CODE_BYTES = 64 * 1024;    // 128 steps of at most about 80 instructions
variant g_var[VARIANTS];
uint8_t *g_code = nullptr;
int g_next = 0;

void (*make(const _SCSPDSP *DSP, int rp, bool wp))(_SCSPDSP *)
{
	if (!g_code)
	{
		void *p = mmap(nullptr, CODE_BYTES * VARIANTS, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) return nullptr;
		g_code = static_cast<uint8_t *>(p);
	}
	// the slot: one of this program's own, else the oldest
	const int k = g_next;
	g_next = (g_next + 1) % VARIANTS;
	if (const char *pd = getenv("M3DSP_PROGDUMP"))      // measurement: each translated program, appended
		if (FILE *f = fopen(pd, "ab")) { const int32_t n = DSP->LastStep; fwrite(&n, 4, 1, f); fwrite(DSP->MPRO, 2, 512, f); fclose(f); }
	emitter e;
	int erp = rp;
	bool ewp = wp;
	translate(DSP, erp, ewp, e);
	if (e.code.size() * 4 > CODE_BYTES) return nullptr;
	uint8_t *dst = g_code + size_t(k) * CODE_BYTES;
	memcpy(dst, e.code.data(), e.code.size() * 4);
	__builtin___clear_cache(reinterpret_cast<char *>(dst), reinterpret_cast<char *>(dst + e.code.size() * 4));
	g_var[k] = variant{DSP, DSP->Gen, rp, wp, reinterpret_cast<void (*)(_SCSPDSP *)>(dst)};
	g_scspdsp_jit_translations++;
	return g_var[k].fn;
}

}  // namespace

const void *SCSPDSP_JitCode(size_t *bytes)
{
	*bytes = g_code ? CODE_BYTES * VARIANTS : 0;
	return g_code;
}

// A program is written a word at a time while the DSP runs; it is translated once it has stood
// unchanged for g_scspdsp_jit_settle samples, and the interpreter runs it until then.
unsigned g_scspdsp_jit_settle = 64;          // SETTLE; the fuzz bench sets 0
struct settle_state { const _SCSPDSP *dsp; UINT32 gen; unsigned samples; };
static settle_state g_settle[2] = {};
static variant *g_last[2] = {};     // each chip's translation used last

bool SCSPDSP_JitStep(_SCSPDSP *DSP)
{
	settle_state *st = g_settle[0].dsp == DSP ? &g_settle[0] : g_settle[1].dsp == DSP ? &g_settle[1] : nullptr;
	if (!st)
	{
		st = g_settle[0].dsp ? &g_settle[1] : &g_settle[0];
		if (st->dsp) st = &g_settle[0];
		*st = settle_state{DSP, DSP->Gen, 0};
	}
	if (st->gen != DSP->Gen) { st->gen = DSP->Gen; st->samples = 0; }
	if (st->samples < g_scspdsp_jit_settle) { st->samples++; g_scspdsp_jit_fallbacks++; return false; }
	const int rp = DSP->READPENDING;
	const bool wp = DSP->WRITEPENDING;
	variant *&last = st == &g_settle[0] ? g_last[0] : g_last[1];
	if (last && last->dsp == DSP && last->gen == DSP->Gen && last->entry_rp == rp && last->entry_wp == wp)
	{
		last->fn(DSP);
		g_scspdsp_jit_samples++;
		return true;
	}
	for (variant &v : g_var)
		if (v.dsp == DSP && v.gen == DSP->Gen && v.entry_rp == rp && v.entry_wp == wp)
		{
			last = &v;
			v.fn(DSP);
			g_scspdsp_jit_samples++;
			return true;
		}
	void (*fn)(_SCSPDSP *) = make(DSP, rp, wp);
	if (!fn) { g_scspdsp_jit_fallbacks++; return false; }
	fn(DSP);
	g_scspdsp_jit_samples++;
	return true;
}

#else

bool SCSPDSP_JitStep(_SCSPDSP *) { return false; }
const void *SCSPDSP_JitCode(size_t *bytes) { *bytes = 0; return nullptr; }

#endif
