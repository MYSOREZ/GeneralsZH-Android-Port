#pragma once

#include <string>

// GeneralsX @feature Android port 10/07/2026 entry point for MainMenuUtils.cpp's
// Online-button handler to hand off to GeneralsOnline instead of the dead
// GameSpy patch-check/DNS path. Not ported from upstream -- ours.

// Returns true if a GeneralsOnline session was found (written by the Android
// launcher's GeneralsOnlineActivity, generalsonline_session.txt) and the
// connect flow was started -- caller should skip the legacy GameSpy path
// entirely in that case. Returns false (does nothing) if there's no session
// yet, or on any non-Android build.
bool TryStartGeneralsOnline();

// GeneralsX @bugfix Android port 13/09/2026 The GeneralsOnline auth API
// replaced its three placeholder "reserved_N" fields with machine_guid /
// mac_addr / vol_serial. On Windows those come from the registry MachineGuid,
// the first adapter's MAC and the C: volume serial; Android has none of the
// three, so the launcher generates one stable value per installation and
// writes it into the session marker file (see NetworkDiagnostics.java for
// what those values are and why they are not real hardware identifiers).
//
// Both processes must send the SAME values -- to the server they are one
// installation -- so the engine reads the launcher's rather than deriving
// its own. Returns empty strings when there is no marker file (not signed in,
// or a non-Android build), which is what the API gets sent in that case.
void GeneralsOnline_GetDeviceIdentity(std::string& outMachineGuid,
	std::string& outMacAddr, std::string& outVolSerial);

// GeneralsX @bugfix Android port 03/10/2026 Session renewal. The service's session tokens last
// fifteen minutes; the PC client renews them every ten with its refresh token, and this port never
// did, so every online session older than that met 401 on every request (lobby list, match result)
// with no word to the player. Refresh tokens are single use: each renewal rotates it, and only the
// newest is accepted (the previous one for five more minutes). The launcher and the engine must
// therefore always use the newest one, and the session marker file is where it lives: the engine
// reads it before every renewal and writes the rotated one back; the launcher does the same.
//
// Reads the newest refresh token: the marker's refresh_token, or, for a launcher too old to write
// one there (1.3.0 and earlier), the one in its own preferences file. False if there is none.
bool GeneralsOnline_ReadStoredRefreshToken(std::string& outRefreshToken);

// Writes a renewed session into the marker file (session_token and refresh_token), atomically.
bool GeneralsOnline_StoreRenewedSession(const std::string& sessionToken, const std::string& refreshToken);
