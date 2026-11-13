// SPDX-License-Identifier: GPL-3.0-or-later
// The few Supermodel definitions its SCSP uses (scsp.cpp, scspdsp.cpp; PROVENANCE.md): its integer
// types, Result, ErrorLog, a configuration node that answers the two settings the SCSP reads, a
// mutex that does nothing (one thread runs the board), and a state file that saves nothing.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int8_t   INT8;
typedef int16_t  INT16;
typedef int32_t  INT32;
typedef int64_t  INT64;

enum class Result { OKAY, FAIL };

inline Result ErrorLog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("arcsnd model3: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	return Result::FAIL;
}

namespace Util { namespace Config {
// "Balance" (master against slave SCSP, -100..100) and "MultiThreaded" are all the SCSP asks for
struct Value
{
	float v;
	template <typename T> T ValueAs() const { return T(v); }
};
struct Node
{
	float balance = 0.0f;
	Value Get(const char *name) const { return Value{!strcmp(name, "Balance") ? balance : 0.0f}; }
	Value operator[](const char *name) const { return Get(name); }
};
} }

class CMutex
{
public:
	bool Lock() { return true; }
	bool Unlock() { return true; }
};
struct CThread
{
	static CMutex *CreateMutex() { return new CMutex; }
};

class CBlockFile
{
public:
	void NewBlock(const char *, const char *) {}
	void Write(const void *, size_t) {}
	void Read(void *, size_t) {}
	Result FindBlock(const char *) { return Result::FAIL; }
};
