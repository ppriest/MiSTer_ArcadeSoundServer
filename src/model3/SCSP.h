/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2011 Bart Trzynadlowski, Nik Henson
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free 
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/

// Changed for arcsnd (PROVENANCE.md): the declarations of the functions SCSP.cpp adds (the timer mode, the
// bench's counters, SCSP_SetDSPLink and the SCSP_DSPLink interface the FPGA's DSP link implements).

 
/*
 * SCSP.h
 * 
 * Header file defining for SCSP emulation.
 */
#define MAX_SCSP	2
#ifndef INCLUDED_SCSP_H
#define INCLUDED_SCSP_H

#include "sm_shim.h"

void SCSP_w8(UINT32 addr,UINT8 val);
void SCSP_w16(UINT32 addr,UINT16 val);
void SCSP_w32(UINT32 addr,UINT32 val);
UINT8 SCSP_r8(UINT32 addr);
UINT16 SCSP_r16(UINT32 addr);
UINT32 SCSP_r32(UINT32 addr);

// arcsnd: 1 the arcade's timer period (the default, my Model 2 core's R976), 0 Supermodel's
void SCSP_SetTimerMode(int mode);
// arcsnd: the sample the SCSPs are on (the 68K runs inside it)
UINT32 SCSP_SampleCount(void);
// arcsnd: the steps chip n's DSP program runs a sample (0: stopped)
int SCSP_DSPSteps(int n);
int SCSP_DAC18B(int n);
// arcsnd: the DSP moved out of this process (study D-53: the FPGA's m3_scsp_dsp). With a link set,
// SCSP_DoMasterSamples runs no DSP: each sample it hands the link the slots' dry sums (before the
// effect return and the clip) and both chips' MIXS, and takes the four channels back; every write
// that reaches the DSP or the final mix goes to the link as it happens, between samples.
struct SCSP_DSPLink
{
	// kind 0 a DSP register word (addr = byte address >> 1), 1 register 0x402's RBL / RBP (data), 2
	// Start (data = LastStep), 3 a sound RAM word in the ring (addr = word address), 5 DAC18B (data)
	virtual void command(int chip, int kind, UINT32 addr, UINT16 data) = 0;
	// the effect return's gains of slot n (0-15): Supermodel's EFSDL ? pan : 0, left and right
	virtual void gain(int chip, int slot, INT32 left, INT32 right) = 0;
	virtual void sample(const INT32 dry[4], const INT32 mixs[2][16], float out[4]) = 0;
	// true once the link has given up on its DSP (see unsupported): this process then runs the DSP
	// and its effect return itself and hands the link finished sums as the dry ones, MIXS zero
	virtual bool fallback() const { return false; }
	// a write the link cannot follow (a byte write into TEMP or MEMS, whose words it holds)
	virtual void unsupported(const char *why) = 0;
	virtual ~SCSP_DSPLink() = default;
};
void SCSP_SetDSPLink(SCSP_DSPLink *link);	// sends the state the link needs (gains, DAC18B, RBL / RBP)
SCSP_DSPLink *SCSP_GetDSPLink(void);

void SCSP_SetCB(int (*Run68k)(int cycles),void (*Int68k)(int irq));
void SCSP_Update(int numSamples);	// generates numSamples samples (at most the buffer length) and runs the 68K alongside
void SCSP_MidiIn(UINT8);
void SCSP_MidiOutW(UINT8);
UINT8 SCSP_MidiOutR();

/*
 * SCSP_Init(n):
 *
 * Initializes the SCSPs, allocates internal memory, and creates a mutex for 
 * MIDI FIFO access. Call SCSP_SetCB() and SCSP_SetBuffers() before calling
 * this.
 *
 * Parameters:
 *		n	Number of SCSPs to create. Always use 2! 
 *
 * Returns:
 *		FAIL if an error occured (prints own error messages), OKAY otherwise.
 */
Result SCSP_Init(const Util::Config::Node &config, int n);

void SCSP_SetRAM(int n,UINT8 *r);

void SCSP_Master_w8(UINT32 addr,UINT8 val);
void SCSP_Master_w16(UINT32 addr,UINT16 val);
void SCSP_Master_w32(UINT32 addr,UINT32 val);
void SCSP_Slave_w8(UINT32 addr,UINT8 val);
void SCSP_Slave_w16(UINT32 addr,UINT16 val);
void SCSP_Slave_w32(UINT32 addr,UINT32 val);
UINT8 SCSP_Master_r8(UINT32 addr);
UINT16 SCSP_Master_r16(UINT32 addr);
UINT32 SCSP_Master_r32(UINT32 addr);
UINT8 SCSP_Slave_r8(UINT32 addr);
UINT16 SCSP_Slave_r16(UINT32 addr);
UINT32 SCSP_Slave_r32(UINT32 addr);

// Supermodel interface functions
void SCSP_SaveState(CBlockFile *StateFile);
void SCSP_LoadState(CBlockFile *StateFile);
void SCSP_SetBuffers(float *leftBufferPtr, float *rightBufferPtr, float* leftRearBufferPtr, float* rightRearBufferPtr, int bufferLength);
void SCSP_Deinit(void);


#endif	// INCLUDED_SCSP_H