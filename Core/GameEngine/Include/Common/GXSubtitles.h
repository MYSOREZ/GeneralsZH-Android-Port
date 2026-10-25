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

// GeneralsX @feature Android port 25/10/2026 The launcher's "Show subtitles" switch.
//
// Every voice line a mission script plays (campaign radio chatter, the Generals Challenge taunts)
// has its text in the string table as DIALOGEVENT:<speech>Subtitle. The game shows one only when
// the text does not start with '*', and the retail data marks every one of them with '*': the
// texts were a script for translators, never shown. With the switch on, they are shown, without
// the mark, in the game's own military caption. A display setting only: nothing in the
// simulation reads it. The launcher creates a file named "subtitles" in the app's own files
// directory when the switch is on and deletes it when off. Read once per process; always off
// off Android.
#pragma once

#if defined(__ANDROID__)
#include <cstdio>
#include <unistd.h>
#endif

namespace GXSubtitles
{
	inline bool enabled()
	{
		static int s_state = -1;
		if (s_state < 0)
		{
			s_state = 0;
#if defined(__ANDROID__)
			char path[256];
			const int userId = (int)(getuid() / 100000);
			snprintf(path, sizeof(path), "/data/user/%d/com.generalsx.zerohour/files/subtitles", userId);
			s_state = access(path, F_OK) == 0 ? 1 : 0;
#endif
		}
		return s_state == 1;
	}
}
