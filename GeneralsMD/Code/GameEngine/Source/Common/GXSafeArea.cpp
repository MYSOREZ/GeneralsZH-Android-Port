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

// GeneralsX @feature Android port 24/09/2026 See Common/GXSafeArea.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GXSafeArea.h"
#include "GameClient/Display.h"

namespace
{
	Real s_left = 0.0f;
	Real s_top = 0.0f;
	Real s_right = 0.0f;
	Real s_bottom = 0.0f;

	Real clampFraction( Real f )
	{
		// A quarter of the screen is already far more than any cutout or corner needs;
		// anything beyond that is a bad argument, not a real device.
		if (!(f > 0.0f))
			return 0.0f;
		return f > 0.25f ? 0.25f : f;
	}

	Int toPixels( Real fraction, Int extent )
	{
		return (Int)(fraction * (Real)extent + 0.5f);
	}
}

namespace GXSafeArea
{

void setFractions( Real left, Real top, Real right, Real bottom )
{
	s_left = clampFraction(left);
	s_top = clampFraction(top);
	s_right = clampFraction(right);
	s_bottom = clampFraction(bottom);
	fprintf(stderr, "INFO: HUD safe insets (fraction of the window) left %.4f top %.4f right %.4f bottom %.4f\n",
		(double)s_left, (double)s_top, (double)s_right, (double)s_bottom);
	fflush(stderr);
}

Int leftPx() { return TheDisplay ? toPixels(s_left, TheDisplay->getWidth()) : 0; }
Int topPx() { return TheDisplay ? toPixels(s_top, TheDisplay->getHeight()) : 0; }
Int rightPx() { return TheDisplay ? toPixels(s_right, TheDisplay->getWidth()) : 0; }
Int bottomPx() { return TheDisplay ? toPixels(s_bottom, TheDisplay->getHeight()) : 0; }

}
