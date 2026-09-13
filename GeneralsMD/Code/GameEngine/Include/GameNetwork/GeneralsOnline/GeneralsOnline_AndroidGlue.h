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
