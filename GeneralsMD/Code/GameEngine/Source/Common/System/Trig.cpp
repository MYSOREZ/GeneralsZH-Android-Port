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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// Trig.cpp
// fast trig functions
// Author: Michael S. Booth, March 1994
// Converted to Generals by Matthew D. Campbell, February 2002

#include "PreRTS.h"

#include <math.h>
#include <limits.h>

#include "Lib/BaseType.h"
#include "Lib/trig.h"
#include "WWMath/wwmath.h"

#define TWOPI			6.28318530718f
#define DEG2RAD 	0.0174532925199f
#define TRIG_RES 4096

// the following are for fixed point ints with 12 fractional bits
#define INT_ONE								4096
#define INT_TWOPI							25736
#define INT_THREEPIOVERTWO 		19302
#define INT_PI								12868
#define INT_HALFPI 						6434

// These five feed object orientation and the transform matrix, which the lockstep
// CRC hashes raw, so they must be the exact same operations the client we play
// against performs.
//
// GeneralsX @bugfix Android port 20/09/2026 They used to call sinf/cosf/tanf/
// acosf/asinf, on the reasoning that the client calls those too. The source
// does -- but the source is not what decides the answer, the C runtime behind
// it is. The client is VC6 for 32-bit x86, whose CRT has no genuine
// single-precision transcendentals: sinf promotes to double and evaluates on
// the x87 unit. bionic does have real single-precision implementations, with
// their own error, so the same source line lands on a different float.
//
// Measured against the client's exact contract -- x87 with the control word
// setFPMode() installs (PC=24, round-to-nearest) -- over 62801 angles across
// [-pi, pi]:
//
//     x87 fsin  vs  sinf                      776 differ  (1.236%)
//     x87 fcos  vs  cosf                      768 differ  (1.223%)
//     x87 fsin  vs  (float)sin((double)x)       0 differ
//     x87 fcos  vs  (float)cos((double)x)       0 differ
//
// One angle in eighty, on a function that rebuilds the rotation matrix of every
// moving object every frame (Thing::setOrientation -> Cos/Sin -> m_transform,
// which Object::crc hashes). That is the observed signature of the cross-play
// desync: replaying a PC recording on Android diverges at the first checkpoint
// with only the objects that MOVE disagreeing, every static one byte-identical.
//
// So evaluate in double and narrow once. Off MSVC only: a VC6 build must keep
// calling the CRT entry points, because there they already are this.

Real Sin(Real x)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	return sinf(x);
#else
	return (Real)sin((double)x);
#endif
}

Real Cos(Real x)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	return cosf(x);
#else
	return (Real)cos((double)x);
#endif
}

Real Tan(Real x)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	return tanf(x);
#else
	return (Real)tan((double)x);
#endif
}

Real ACos(Real x)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	return acosf(x);
#else
	return (Real)acos((double)x);
#endif
}

Real ASin(Real x)
{
#if defined(_MSC_VER) && defined(_M_IX86)
	return asinf(x);
#else
	return (Real)asin((double)x);
#endif
}

double Sqrt(double x)
{
	return WWMath::SqrtOrigin(x);
}

#ifdef REGENERATE_TRIG_TABLES
void initTrig()
{
	static Byte inited = FALSE;
	Real angle, r;
	int i;

	if (inited)
		return;

	inited = TRUE;

	static int columns = 8;
	int column = 0;
	FILE *fp = fopen("trig.txt", "w");
	fprintf(fp, "static Int sinLookup[TRIG_RES] = {\n");
	for( i=0; i<TRIG_RES; i++ ) {
		angle = TWOPI * i / (Real)TRIG_RES;
		sinLookup[i] = (Int)(sin(angle) * INT_ONE);

		if (i == 0)
		{
			fprintf(fp, "\t0x%8.8X", sinLookup[i]);
		}
		else if (column == 0)
		{
		fprintf(fp, ",\n\t0x%8.8X", sinLookup[i]);
		}
		else
		{
		fprintf(fp, ", 0x%8.8X", sinLookup[i]);
		}
		column = (column + 1) % columns;
	}
	fprintf(fp, "\n};\n\n");

	column = 0;
	fprintf(fp, "static Int arcCosLookup[2 * INT_ONE] = {\n");
	for( i=0; i<2*INT_ONE; i++ ) {
		r = (Real)i / (Real)INT_ONE - 1.0f;

		arcCosLookup[i] = (Int)(acos( (double)r ) * INT_TWOPI / TWOPI );

		if (i == 0)
		{
			fprintf(fp, "\t0x%8.8X", arcCosLookup[i]);
		}
		else if (column == 0)
		{
		fprintf(fp, ",\n\t0x%8.8X", arcCosLookup[i]);
		}
		else
		{
		fprintf(fp, ", 0x%8.8X", arcCosLookup[i]);
		}
		column = (column + 1) % columns;
	}
	fprintf(fp, "\n};\n\n");

	fclose(fp);
}

class TrigInit
{
public:
	TrigInit() { initTrig(); }
};
TrigInit trigInitializer;

#endif // REGENERATE_TRIG_TABLES


