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


// GeneralsX @feature Android port 01/10/2026 Interface scale (the launcher's GXUiScale).
//
// The game stretches every .wnd layout over the whole screen: a layout made for 800x600 is scaled
// by the screen's width / 800 and height / 600 (parseScreenRect). On a 2340x1080 phone that is 2.9
// across and only 1.8 down, so buttons come out wide and low, and nothing ever gets physically
// bigger -- not even at a lower game resolution. This scales chosen layouts once more, around an
// anchor at the edge they sit on (the control bar grows up from the bottom, the generals' power
// bar out from the right edge, dialogs from their centre), and never past the screen: the factor is
// clamped per axis so the layout's content still fits. Full-screen container windows keep their
// size, so only what is drawn on them moves. Code that places windows of a layout by hand (the
// control bar's scheme) maps its coordinates through the same transform. Client-side only.
#pragma once

#include "Lib/BaseType.h"

namespace GXUiScale
{
	// X' = X * kx + addX * screenWidth (and the same for Y), in screen pixels.
	struct Transform
	{
		Bool active;
		Real kx, ky;
		Real addX, addY;  // fractions of the screen
		Real fontK;       // for the layout's fonts
		Real mapX( Real x ) const;
		Real mapY( Real y ) const;
	};

	// The launcher's interface scale (GX_UI_SCALE, percent), 1 when unset.
	Real userScale();

	// The transform for a layout file ("Menus/QuitMenu.wnd", "ControlBar.wnd"), computed when the
	// layout is loaded (beginLayout); identity for layouts that are not scaled or not loaded yet.
	const Transform &forLayout( const char *layoutFile );

	// winCreateFromScript: the layout about to be parsed, and its text, to find what to scale.
	void beginLayout( const char *layoutFile, const char *text, Int length );
	void endLayout();
	// parseScreenRect / parseFont: the next window of the layout being parsed.
	void mapNextWindowRect( Int *loX, Int *loY, Int *hiX, Int *hiY );
	Int scaleFontSize( Int size );
}
