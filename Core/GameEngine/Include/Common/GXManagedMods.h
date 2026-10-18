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

// GeneralsX @feature Android port 18/10/2026 The mods the launcher's Mods screen turned on
// (PLAN-027 step 1): each a folder under <game folder>/mods/, holding .big files. Right before
// starting a game the launcher writes their absolute paths, one per line, highest priority first,
// to mods_active.txt in the app's own files directory (no file: no managed mods), and
// ArchiveFileSystem::loadMods() mounts them above the game's archives and above legacy '!' mods.
// Read once per process. Always empty off Android.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if defined(__ANDROID__)
#include <unistd.h>
#endif

namespace GXManagedMods
{
	// The enabled mod folders, highest priority first, each ending with '/'.
	inline const std::vector<std::string> &folders()
	{
		static std::vector<std::string> s_folders;
		static bool s_loaded = false;
		if (!s_loaded)
		{
			s_loaded = true;
#if defined(__ANDROID__)
			char path[256];
			const int userId = (int)(getuid() / 100000);
			snprintf(path, sizeof(path), "/data/user/%d/com.generalsx.zerohour/files/mods_active.txt", userId);
			FILE *fp = fopen(path, "r");
			if (fp != nullptr)
			{
				char line[1024];
				while (fgets(line, sizeof(line), fp) != nullptr)
				{
					std::string folder = line;
					while (!folder.empty() && (folder.back() == '\n' || folder.back() == '\r'))
						folder.pop_back();
					if (folder.empty() || folder[0] != '/')
						continue;
					if (folder.back() != '/')
						folder += '/';
					s_folders.push_back(folder);
				}
				fclose(fp);
			}
#endif
		}
		return s_folders;
	}

	// True when path lies inside one of the enabled mod folders.
	inline bool contains(const char *path)
	{
		const std::vector<std::string> &list = folders();
		for (size_t i = 0; i < list.size(); ++i)
		{
			if (strncmp(path, list[i].c_str(), list[i].size()) == 0)
				return true;
		}
		return false;
	}
}
