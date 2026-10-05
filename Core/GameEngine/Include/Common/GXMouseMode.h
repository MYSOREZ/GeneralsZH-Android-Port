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


// GeneralsX @feature Android port 05/10/2026 The launcher's "Mouse and touchpad" switch (issue #39).
//
// Off by default: the game is played by touch, read natively by the touch layer
// (SDL3GameEngine.cpp, TouchInput.cpp), and there is no pointer. Turned on, it drops an empty file
// named mouse_mode into the app's own files directory (SetupActivity.MOUSE_MODE_MARKER), and the
// game has a pointer instead: a connected mouse drives it exactly as on a PC (SDL3Mouse feeds the
// message stream), and a finger moves it like a laptop touchpad (SDL3GameEngine.cpp,
// handleTouchpadEvent), with the cursor drawn by the game. Read the same way as GXLogging.h, once
// per process. Always FALSE off Android.
#pragma once

#if defined(__ANDROID__)
#include <cstdio>
#include <unistd.h>
#endif

inline bool GXMouseModeEnabled()
{
#if defined(__ANDROID__)
	static int state = -1;
	if (state < 0)
	{
		char path[256];
		const int userId = (int)(getuid() / 100000);
		snprintf(path, sizeof(path), "/data/user/%d/com.generalsx.zerohour/files/mouse_mode", userId);
		state = (access(path, F_OK) == 0) ? 1 : 0;
	}
	return state == 1;
#else
	return false;
#endif
}
