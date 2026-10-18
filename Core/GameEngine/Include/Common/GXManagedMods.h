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
// A first line "#profile <name>" names the set's profile: its saves and replays go to
// <user data>/Profiles/<name>/ (profileDir), so a modded save never meets the vanilla game.
// Read once per process. Always empty off Android.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if defined(__ANDROID__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace GXManagedMods
{
	inline std::string &profileNameStorage()
	{
		static std::string s_profile;
		return s_profile;
	}

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
					if (folder.compare(0, 9, "#profile ") == 0)
					{
						// Only characters safe in a folder name, whatever the launcher wrote.
						std::string name;
						for (size_t i = 9; i < folder.size() && name.size() < 80; ++i)
						{
							const char c = folder[i];
							const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
								|| (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '+' || c == '.';
							name += safe ? c : '_';
						}
						if (!name.empty() && name[0] != '.')
							profileNameStorage() = name;
						continue;
					}
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

	// The set's own folder for saves and replays, "<userData>Profiles/<name>/" (created), or empty
	// when no mods are on: then the game's usual folders are used.
	inline std::string profileDir(const char *userData)
	{
		folders();
		const std::string &name = profileNameStorage();
		if (name.empty() || userData == nullptr || userData[0] == '\0')
			return std::string();
		std::string dir = userData;
		if (dir.back() != '/' && dir.back() != '\\')
			dir += '/';
		dir += "Profiles/";
#if defined(__ANDROID__)
		mkdir(dir.c_str(), 0775);
#endif
		dir += name;
		dir += '/';
#if defined(__ANDROID__)
		mkdir(dir.c_str(), 0775);
#endif
		return dir;
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
