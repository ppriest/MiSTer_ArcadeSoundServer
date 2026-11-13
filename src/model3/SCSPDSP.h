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

// Changed for arcsnd (PROVENANCE.md): the struct gains the fields the ARM translation needs (TEMP's mirror,
// RBL - 1 and RBP << 12 kept, the program generation); the interpreter's behaviour is unchanged.


/*
 * SCSPDSP.h
 * 
 * Header file for SCSP DSP emulation. Used only by SCSP.cpp; do not use
 * externally.
 */

#ifndef INCLUDED_SCSPDSP_H
#define INCLUDED_SCSPDSP_H

#include "sm_shim.h"


//the DSP Context
struct _SCSPDSP
{
//Config
	UINT16 *SCSPRAM;
	unsigned int RBP;	//Ring buf pointer
	unsigned int RBL;	//Delay ram (Ring buffer) size in words

//context	
	
	INT16 COEF[64];		//16 bit signed
	UINT16 MADRS[32];	//offsets (in words), 16 bit
	UINT16 MPRO[128*4];	//128 steps 64 bit
	INT32 TEMP[256];	//TEMP regs,24 bit signed; arcsnd: 128-255 mirror 0-127 (every writer keeps them equal), so
						// the translated program reads TEMP[(n + DEC) & 7F] as one load from TEMP + (DEC & 7F)
	INT32 MEMS[32];	//MEMS regs,24 bit signed
	unsigned int DEC;

//input
	INT32 MIXS[16];	//MIXS, 24 bit signed
	INT16 EXTS[2];	//External inputs (CDDA)	16 bit signed

//output
	INT16 EFREG[16];	//EFREG, 16 bit signed
	
	bool Stopped;
	int LastStep;

//registers that keep their value from one step (and sample) to the next
	INT32 ACC;			//accumulator (SFT_REG), 26 bit signed
	INT32 INPUTS;		//selected input, 24 bit signed
	INT32 FRC_REG;		//13 bit
	INT32 Y_REG;		//24 bit
	UINT32 ADRS_REG;	//12 bit
	INT32 MEMVAL;		//last word read from sound memory, waiting for an IWT
	UINT32 RWADDR;		//sound memory word address of the access started by the previous step
	UINT16 WRITEVAL;	//word to write there
	int READPENDING;	//0: none, 1: read (float), 2: read (NOFL)
	bool WRITEPENDING;
// arcsnd: the program's generation, a new one (g_scspdsp_gen, never reused) at every MPRO write and
// SCSPDSP_Start: the translation of scspdsp_jit.cpp is made again
	UINT32 Gen;
// arcsnd: RBL - 1 and RBP << 12, kept by SCSP_UpdateReg and SCSPDSP_Init for the translated program
	UINT32 RBLMASK, RBPOFF;
};

void SCSPDSP_Init(_SCSPDSP *DSP);
void SCSPDSP_SetSample(_SCSPDSP *DSP,INT32 sample,int SEL);	// IMXL is applied by the caller
void SCSPDSP_Step(_SCSPDSP *DSP);
void SCSPDSP_Start(_SCSPDSP *DSP);
// arcsnd: the translated program (scspdsp_jit.cpp): runs one sample and returns true, or false where
// there is no translation (not ARM); g_scspdsp_jit false keeps every sample in the interpreter
bool SCSPDSP_JitStep(_SCSPDSP *DSP);
extern bool g_scspdsp_jit;
// arcsnd: the translations' code area and its size (bench and measurement)
const void *SCSPDSP_JitCode(size_t *bytes);
extern UINT32 g_scspdsp_gen;

#endif	// INCLUDED_SCSPDSP_H
