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
// This file is compiled with -fno-builtin, so the compiler cannot recognise the pattern
// "(float)sin((double)x)" inside sinf and turn it back into a call to sinf.

#if !(defined(_MSC_VER) && defined(_M_IX86))

// Deliberately no <math.h>: the definitions below must not inherit the default
// visibility of the platform's declarations.
extern "C"
{
	double sin(double);
	double cos(double);
	double tan(double);
	double asin(double);
	double acos(double);
	double atan(double);
	double atan2(double, double);
	double sinh(double);
	double cosh(double);
	double tanh(double);
	double exp(double);
	double log(double);
	double log10(double);
	double pow(double, double);
}

#define GX_REFERENCE_FLOAT_MATH extern "C" __attribute__((visibility("hidden")))

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

#endif
