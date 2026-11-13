// SPDX-License-Identifier: GPL-3.0-or-later
// Supermodel's IBus (Src/CPU/Bus.h, GPL-3.0-or-later, the Supermodel Team), the bus its Z80 calls:
// for the Digital Sound Board's DSB1 (dsb.cpp, PROVENANCE.md).
#pragma once

#include "sm_shim.h"

class IBus
{
public:
	virtual UINT8	Read8(UINT32 addr)	{ return 0xFF; }
	virtual UINT16	Read16(UINT32 addr)	{ return 0xFFFF; }
	virtual UINT32	Read32(UINT32 addr)	{ return 0xFFFFFFFF; }
	virtual void	Write8(UINT32 addr, UINT8 data)		{}
	virtual void	Write16(UINT32 addr, UINT16 data)	{}
	virtual void	Write32(UINT32 addr, UINT32 data)	{}
	virtual UINT8	IORead8(UINT32 addr)	{ return 0xFF; }
	virtual void	IOWrite8(UINT32 addr, UINT8 data)	{}
	virtual ~IBus() = default;
};
