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

// GeneralsX @feature Android port 18/10/2026 Feature switches the engine trusts only when it has
// verified their signature itself.
//
// GXRemoteConfig reads remote_config.ini, which the launcher writes after checking the manifest's
// signature -- so a launcher patched to write its own values, or a file edited by hand, is
// believed. That is fine for server addresses, not for a switch that decides whether this client
// may join PC games (pc_crossplay): there the engine reads the signed manifest the launcher saved
// (files/update/manifest.json + manifest.json.sig) and checks the signature against the update
// key's public half, compiled in. No signature, a bad one, or a manifest without the key: off.
#pragma once

namespace GXSignedSettings
{
	// True only if the signed manifest's config sets feature to "1".
	bool enabled(const char *feature);
}
