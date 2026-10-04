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

// GeneralsX @feature Android port 27/09/2026 Values that change without a new APK.
//
// The launcher (UpdateManager.java) fetches a signed manifest from the repository's `updates`
// branch and writes its `config` section to files/update/remote_config.ini as key=value lines.
// Only a verified manifest is ever written there, and the file is in the app's private storage,
// so the engine reads it as trusted. A missing file or key means "use the value compiled in".
// Parsed once per process. Off Android there is no such file and every lookup misses.
//
// It also owns the boot marker of an updated engine: the launcher writes files/update/
// boot_pending before loading a downloaded engine, and markEngineBootComplete() removes it once
// the main menu is up, so an engine that never gets that far can be dropped automatically.
#pragma once

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#if defined(__ANDROID__)
#include <unistd.h>
#endif

namespace GXRemoteConfig
{
	inline std::string updateDirPath()
	{
#if defined(__ANDROID__)
		char path[256];
		const int userId = (int)(getuid() / 100000);
		snprintf(path, sizeof(path), "/data/user/%d/com.generalsx.zerohour/files/update", userId);
		return path;
#else
		return std::string();
#endif
	}

	inline const std::map<std::string, std::string> &values()
	{
		static std::map<std::string, std::string> s_values;
		static bool s_loaded = false;
		if (!s_loaded)
		{
			s_loaded = true;
			const std::string dir = updateDirPath();
			if (!dir.empty())
			{
				FILE *f = fopen((dir + "/remote_config.ini").c_str(), "r");
				if (f != nullptr)
				{
					char line[2048];
					while (fgets(line, sizeof(line), f) != nullptr)
					{
						if (line[0] == '#')
							continue;
						char *eq = strchr(line, '=');
						if (eq == nullptr)
							continue;
						*eq = '\0';
						std::string value = eq + 1;
						while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
							value.pop_back();
						s_values[line] = value;
					}
					fclose(f);
				}
			}
		}
		return s_values;
	}

	// The value for key, or fallback when the manifest did not set it.
	inline std::string get(const char *key, const char *fallback)
	{
		const std::map<std::string, std::string> &v = values();
		std::map<std::string, std::string>::const_iterator it = v.find(key);
		return (it != v.end() && !it->second.empty()) ? it->second : std::string(fallback);
	}

	inline void markEngineBootComplete()
	{
		const std::string dir = updateDirPath();
		if (!dir.empty())
			remove((dir + "/boot_pending").c_str());
	}
}
