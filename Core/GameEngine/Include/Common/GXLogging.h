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

// GeneralsX @feature Android port 27/09/2026 The launcher's "Collect logs" switch.
//
// Turning it off drops an empty file named logging_off into the app's own files directory
// (SetupActivity.LOGGING_OFF_MARKER). With it present nothing is logged in the background:
// the stderr mirror (SDL3Main.cpp), crash.log (AndroidCrashHandler.cpp) and GeneralsOnline.log
// (NGMP_Helpers.cpp) all ask here first. The path is derived from the uid the same way
// AndroidCrashHandler.cpp derives crash.log's, because the crash handler asks before SDL_main
// has run and before any Android storage API is reachable. Answered once per process: the
// switch is flipped in the launcher, never while the game runs. Always FALSE off Android.
#pragma once

#if defined(__ANDROID__)
#include <cstdio>
#include <unistd.h>
#endif

inline bool GXLoggingDisabled()
{
#if defined(__ANDROID__)
	static int state = -1;
	if (state < 0)
	{
		char path[256];
		const int userId = (int)(getuid() / 100000);
		snprintf(path, sizeof(path), "/data/user/%d/com.generalsx.zerohour/files/logging_off", userId);
		state = (access(path, F_OK) == 0) ? 1 : 0;
	}
	return state == 1;
#else
	return false;
#endif
}
