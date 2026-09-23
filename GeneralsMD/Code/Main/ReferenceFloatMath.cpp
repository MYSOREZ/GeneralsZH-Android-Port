/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// ReferenceFloatMath.cpp /////////////////////////////////////////////////////
//
// GeneralsX @bugfix Android port 22/09/2026 The float transcendentals, as the reference
// client evaluates them.
//
// Cross-play has to reproduce the GeneralsOnline PC client, which is 32-bit MSVC. Its
// CRT has no single-precision transcendentals on x86: sinf, atan2f and the rest are
// inline wrappers that call the double function and round the result to float once.
// Our CRT (bionic, glibc, Apple's libm) has real single-precision implementations, and
// they disagree with "double, then round" in the last bit on a meaningful share of
// inputs -- measured at about 1.2% for sinf and far more for atan2f.
//
// The calls are everywhere, and most of them do not say "sinf" in the source. In C++ a
// plain atan2(dy, dx) with Real arguments resolves to the float overload, which the
// compiler emits as a call to atan2f -- checked on the NDK's clang: atan2(float, float)
// becomes "b atan2f", sin(float) becomes "b sinf". Locomotor alone steers every moving
// unit with eight of those per frame. A replay recorded on the PC stayed in lockstep
// with us exactly until its dozer started to drive, and then never again.
//
// Rather than rewrite every call site and hope none is missed, this file gives the
// module its own definitions of those functions with the reference's semantics. They
// have hidden visibility, so every call inside this binary binds to them at link time
// instead of going through the PLT to the platform libm; nothing outside the binary is
// affected. sqrtf and fmodf are deliberately absent: both are correctly rounded on
// every platform, so there is nothing to reproduce.
//
// GeneralsX @bugfix Android port 23/09/2026 sincosf, which no source file calls. When
// clang sees sin(a) and cos(a) of the same float argument it merges them into ONE call
// to sincosf -- and bionic's sincosf is its own single-precision routine. So every such
// pair went straight past the definitions below, into the platform libm. libmain.so
// imported sincosf@LIBC, and the objects that asked for it were Geometry.cpp
// (GeometryInfo::get2DBounds, i.e. which partition cells a box-shaped object touches),
// BuildAssistant.cpp (whether a building may be placed and where its exit is),
// AISkirmishPlayer.cpp (where the AI puts its base defences) and AIGroup.cpp. Defining
// it here, with the same "double, then round once" as sinf and cosf, catches every merge
// the compiler makes now or later. The double pair is merged the same way, into sincos;
// that one is defined here too, as plain sin() and cos(), so a merged pair can never
// return anything the two separate calls would not.
//
// This file is compiled with -fno-builtin, so the compiler cannot recognise the pattern
// "(float)sin((double)x)" inside sinf and turn it back into a call to sinf.

#if !(defined(_MSC_VER) && defined(_M_IX86))

#include <dlfcn.h>
#include <fenv.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>

// GeneralsX @feature Android port 23/09/2026 Who calls the maths, and where it could have
// rounded the other way. The double libm functions below are defined here too, as thin
// forwarders to the platform's own, so every call from inside this binary -- a plain
// sin(), the double inside sinf(), a merged sincos() -- passes through one place. For a
// window of logic frames (gxMathTraceFrame(), driven by GameLogic::update) each call is
// counted against its three innermost callers, and flagged "fragile" when the exact double
// result sits within a few double ULPs of a float rounding midpoint: that is the only way a
// different libm (the PC client's MSVC CRT) could give the simulation a different float.
// At the end of the window the sites are printed with their addresses in this binary;
// a copy of libmain with its symbol table turns them into function names.

namespace
{
	typedef double (*Fn1)(double);
	typedef double (*Fn2)(double, double);

	void *realSym(const char *name)
	{
		void *p = dlsym(RTLD_NEXT, name);
		if (p == nullptr)
		{
			void *libm = dlopen("libm.so", RTLD_NOW);
			if (libm != nullptr)
				p = dlsym(libm, name);
		}
		if (p == nullptr)
		{
			fprintf(stderr, "[GX-NET] math trace: cannot resolve libm %s, aborting\n", name);
			abort();
		}
		return p;
	}

	enum { MF_SIN, MF_COS, MF_TAN, MF_ASIN, MF_ACOS, MF_ATAN, MF_ATAN2, MF_SINH, MF_COSH, MF_TANH,
		MF_EXP, MF_LOG, MF_LOG10, MF_POW, MF_COUNT };
	const char *const kFnName[MF_COUNT] = { "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
		"sinh", "cosh", "tanh", "exp", "log", "log10", "pow" };

	const int kDepth = 3;
	const int kSlots = 4096;
	struct Site
	{
		int fn;
		unsigned long pc[kDepth];
		unsigned calls;
		unsigned fragile;
		double lastFragileArg;
	};
	Site g_sites[kSlots];
	int g_siteCount = 0;
	unsigned g_overflow = 0;

	volatile int g_active = 0;
	pthread_t g_thread;
	unsigned g_from = 1900, g_to = 2000;
	int g_configured = 0;
	int g_dumped = 0;
	unsigned long g_base = 0;

	struct Walk { unsigned long pc[kDepth + 2]; int n; };
	_Unwind_Reason_Code walkCb(struct _Unwind_Context *ctx, void *arg)
	{
		Walk *w = static_cast<Walk *>(arg);
		if (w->n >= kDepth + 2)
			return _URC_END_OF_STACK;
		w->pc[w->n++] = (unsigned long)_Unwind_GetIP(ctx);
		return _URC_NO_REASON;
	}

	// Within a few double ULPs of the midpoint between the two floats around d.
	bool fragile(double d)
	{
		if (!(d == d) || d == 0.0)
			return false;
		const float f = (float)d;
		double lo, hi;
		if ((double)f <= d) { lo = f; hi = (double)__builtin_nextafterf(f, 3.4e38f); }
		else                { hi = f; lo = (double)__builtin_nextafterf(f, -3.4e38f); }
		const double mid = lo + (hi - lo) * 0.5;
		double ulp = __builtin_nextafter(d, 1e308) - d;
		if (ulp < 0) ulp = -ulp;
		double dist = d - mid;
		if (dist < 0) dist = -dist;
		return dist <= 4.0 * ulp;
	}

	void recordInner(int fn, double arg, double result);
	void record(int fn, double arg, double result)
	{
		if (!g_active || !pthread_equal(pthread_self(), g_thread))
			return;
		// the bookkeeping below does float arithmetic of its own; keep it out of the
		// exception flags the simulation's own fp-flags trace reads
		fexcept_t saved;
		fegetexceptflag(&saved, FE_ALL_EXCEPT);
		recordInner(fn, arg, result);
		fesetexceptflag(&saved, FE_ALL_EXCEPT);
	}
	void recordInner(int fn, double arg, double result)
	{
		Walk w; w.n = 0;
		_Unwind_Backtrace(walkCb, &w);
		// frame 0 is record(), frame 1 the forwarder; the callers start at 2
		unsigned long pc[kDepth] = { 0, 0, 0 };
		for (int i = 0; i < kDepth; ++i)
			if (i + 2 < w.n) pc[i] = w.pc[i + 2];
		unsigned h = (unsigned)fn * 2654435761u;
		for (int i = 0; i < kDepth; ++i) h ^= (unsigned)(pc[i] * 0x9E3779B1u) + (h << 6) + (h >> 2);
		const bool frag = fragile(result);
		for (int probe = 0; probe < kSlots; ++probe)
		{
			Site &s = g_sites[(h + probe) % kSlots];
			if (s.calls == 0)
			{
				s.fn = fn;
				memcpy(s.pc, pc, sizeof(pc));
				s.calls = 1;
				s.fragile = frag ? 1 : 0;
				if (frag) s.lastFragileArg = arg;
				++g_siteCount;
				return;
			}
			if (s.fn == fn && memcmp(s.pc, pc, sizeof(pc)) == 0)
			{
				++s.calls;
				if (frag) { ++s.fragile; s.lastFragileArg = arg; }
				return;
			}
		}
		++g_overflow;
	}

	void dump()
	{
		Site *order[kSlots];
		int n = 0;
		unsigned total = 0, totalFragile = 0;
		for (int i = 0; i < kSlots; ++i)
			if (g_sites[i].calls) { order[n++] = &g_sites[i]; total += g_sites[i].calls; totalFragile += g_sites[i].fragile; }
		for (int i = 1; i < n; ++i)
			for (int j = i; j > 0 && (order[j]->fragile > order[j-1]->fragile ||
				(order[j]->fragile == order[j-1]->fragile && order[j]->calls > order[j-1]->calls)); --j)
			{ Site *t = order[j]; order[j] = order[j-1]; order[j-1] = t; }
		fprintf(stderr, "[GX-NET] math trace frames %u..%u: %u libm calls on the logic thread, %d call sites, "
			"%u fragile (a different libm could round the float the other way), %u unrecorded\n",
			g_from, g_to - 1, total, n, totalFragile, g_overflow);
		for (int i = 0; i < n && i < 120; ++i)
		{
			const Site &s = *order[i];
			fprintf(stderr, "[GX-NET] math site %-5s calls=%u fragile=%u callers=libmain+0x%lx < 0x%lx < 0x%lx",
				kFnName[s.fn], s.calls, s.fragile,
				s.pc[0] ? s.pc[0] - g_base : 0, s.pc[1] ? s.pc[1] - g_base : 0, s.pc[2] ? s.pc[2] - g_base : 0);
			if (s.fragile)
				fprintf(stderr, " lastFragileArg=%.17g", s.lastFragileArg);
			fprintf(stderr, "\n");
		}
		fflush(stderr);
	}

	void configure()
	{
		g_configured = 1;
		FILE *f = fopen("gx_math_trace.txt", "r");
		if (f != nullptr)
		{
			unsigned a = 0, b = 0;
			if (fscanf(f, "%u %u", &a, &b) == 2 && b > a) { g_from = a; g_to = b; }
			fclose(f);
		}
		Dl_info info;
		if (dladdr((const void *)&configure, &info) != 0)
			g_base = (unsigned long)info.dli_fbase;
		fprintf(stderr, "[GX-NET] math trace armed for logic frames %u..%u (gx_math_trace.txt \"FROM TO\" moves it), "
			"image base %p\n", g_from, g_to - 1, (void *)g_base);
		fflush(stderr);
	}
}

// Called by GameLogic::update once per logic frame, on the logic thread, when the network
// trace is on.
extern "C" __attribute__((visibility("hidden"))) void gxMathTraceFrame(unsigned frame)
{
	if (!g_configured)
		configure();
	if (frame == 0)
	{
		// a new game: forget the previous one
		memset(g_sites, 0, sizeof(g_sites));
		g_siteCount = 0; g_overflow = 0; g_dumped = 0; g_active = 0;
		return;
	}
	if (frame >= g_from && frame < g_to)
	{
		g_thread = pthread_self();
		g_active = 1;
	}
	else
	{
		g_active = 0;
		if (frame >= g_to && !g_dumped && g_siteCount > 0)
		{
			g_dumped = 1;
			dump();
		}
	}
}

#define GX_REFERENCE_FLOAT_MATH extern "C" __attribute__((visibility("hidden")))

#define GX_FORWARD1(NAME, ID) \
	GX_REFERENCE_FLOAT_MATH double NAME(double x) \
	{ \
		static Fn1 real = (Fn1)realSym(#NAME); \
		const double r = real(x); \
		if (g_active) record(ID, x, r); \
		return r; \
	}

GX_FORWARD1(sin, MF_SIN)
GX_FORWARD1(cos, MF_COS)
GX_FORWARD1(tan, MF_TAN)
GX_FORWARD1(asin, MF_ASIN)
GX_FORWARD1(acos, MF_ACOS)
GX_FORWARD1(atan, MF_ATAN)
GX_FORWARD1(sinh, MF_SINH)
GX_FORWARD1(cosh, MF_COSH)
GX_FORWARD1(tanh, MF_TANH)
GX_FORWARD1(exp, MF_EXP)
GX_FORWARD1(log, MF_LOG)
GX_FORWARD1(log10, MF_LOG10)

GX_REFERENCE_FLOAT_MATH double atan2(double y, double x)
{
	static Fn2 real = (Fn2)realSym("atan2");
	const double r = real(y, x);
	if (g_active) record(MF_ATAN2, y, r);
	return r;
}

GX_REFERENCE_FLOAT_MATH double pow(double x, double y)
{
	static Fn2 real = (Fn2)realSym("pow");
	const double r = real(x, y);
	if (g_active) record(MF_POW, x, r);
	return r;
}

GX_REFERENCE_FLOAT_MATH float sinf(float x)            { return (float)sin((double)x); }
GX_REFERENCE_FLOAT_MATH float cosf(float x)            { return (float)cos((double)x); }
GX_REFERENCE_FLOAT_MATH float tanf(float x)            { return (float)tan((double)x); }
GX_REFERENCE_FLOAT_MATH float asinf(float x)           { return (float)asin((double)x); }
GX_REFERENCE_FLOAT_MATH float acosf(float x)           { return (float)acos((double)x); }
GX_REFERENCE_FLOAT_MATH float atanf(float x)           { return (float)atan((double)x); }
GX_REFERENCE_FLOAT_MATH float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
GX_REFERENCE_FLOAT_MATH float sinhf(float x)           { return (float)sinh((double)x); }
GX_REFERENCE_FLOAT_MATH float coshf(float x)           { return (float)cosh((double)x); }
GX_REFERENCE_FLOAT_MATH float tanhf(float x)           { return (float)tanh((double)x); }
GX_REFERENCE_FLOAT_MATH float expf(float x)            { return (float)exp((double)x); }
GX_REFERENCE_FLOAT_MATH float logf(float x)            { return (float)log((double)x); }
GX_REFERENCE_FLOAT_MATH float log10f(float x)          { return (float)log10((double)x); }
GX_REFERENCE_FLOAT_MATH float powf(float x, float y)   { return (float)pow((double)x, (double)y); }
GX_REFERENCE_FLOAT_MATH void sincos(double x, double* s, double* c)
{
	*s = sin(x);
	*c = cos(x);
}
GX_REFERENCE_FLOAT_MATH void sincosf(float x, float* s, float* c)
{
	*s = (float)sin((double)x);
	*c = (float)cos((double)x);
}

#endif
