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

// GeneralsX @feature Android port 24/09/2026 Screen safe area for HUD text (issue #20).
//
// Phones with display cutouts and rounded or curved corners hide the parts of the frame
// the engine's corner HUD uses: the FPS counter and clock at the top left, the match
// timer at the top right, the GeneralsX credit at the bottom left. The launcher measures
// the safe insets (DisplayCutout, RoundedCorner) and passes them as fractions of the
// window with -gxSafeInsets left,top,right,bottom; the fractions keep them independent
// of the engine's internal resolution. Without the argument every inset is zero, which
// is the unchanged desktop behaviour. Client-side only: nothing here reaches the logic.
#pragma once

#include "Lib/BaseType.h"

namespace GXSafeArea
{
	void setFractions( Real left, Real top, Real right, Real bottom );

	// Insets in current display pixels.
	Int leftPx();
	Int topPx();
	Int rightPx();
	Int bottomPx();
}
