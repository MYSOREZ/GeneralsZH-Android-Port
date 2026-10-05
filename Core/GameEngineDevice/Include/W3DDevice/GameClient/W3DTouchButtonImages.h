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

// GeneralsX @feature Android port 05/10/2026 Pictures for the command-bar buttons the touch layer
// adds and the game has no art for (issue #25): the page arrows (GXBuilderPageMore,
// GXBuilderPageBack), scatter (GXScatter) and formation (GXFormation). Drawn once, at display
// init, into textures the display owns and registered in the mapped-image collection by name,
// where the control bar finds them like any other button picture. Shared by both games; it used
// to be a copy in each game's W3DDisplay.cpp.

#pragma once

void W3DRegisterTouchButtonImages();
