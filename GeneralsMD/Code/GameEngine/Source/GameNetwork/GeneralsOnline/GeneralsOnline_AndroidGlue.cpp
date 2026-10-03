// GeneralsX @feature Android port 11/07/2026 This file is NOT ported from
// upstream -- it's our own glue reading the Android launcher's session
// marker file and kicking off GeneralsOnline login. TheNGMPGame and
// OnKickedFromLobby() used to be stubbed here (upstream defines them in
// WOLGameSetupMenu.cpp, which this port originally skipped); now that the
// real WOLGameSetupMenu.cpp/WOLWelcomeMenu.cpp/etc. are ported too, those
// stubs were deleted to avoid duplicate-symbol link errors.

#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/GeneralsOnline_AndroidGlue.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Auth.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/Shell.h"
#include "Common/GameEngine.h"
#include <cstdlib>
#include <string>

#if defined(__ANDROID__)
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#endif

#if defined(__ANDROID__)
namespace
{
	struct AndroidSession
	{
		std::string sessionToken;
		std::string userId;
		std::string displayName;
		std::string wsUri;
		std::string machineGuid;
		std::string macAddr;
		std::string volSerial;
		std::string refreshToken;
	};

	// Mirrors SDL3Main.cpp's gamedata_path.txt reader: the Android launcher
	// (GeneralsOnlineActivity.java) writes this plain "key=value" marker file
	// straight into getFilesDir() -- the same directory
	// SDL_GetAndroidInternalStoragePath() resolves to -- there is no JNI
	// plumbing involved on either side, just a shared filesystem convention.
	bool ReadAndroidSession(AndroidSession& outSession)
	{
		const char* internalPath = SDL_GetAndroidInternalStoragePath();
		if (internalPath == nullptr)
		{
			fprintf(stderr, "DEBUG-ONLINE: ReadAndroidSession bail -- SDL_GetAndroidInternalStoragePath() returned null\n");
			fflush(stderr);
			return false;
		}

		char markerPath[1024];
		snprintf(markerPath, sizeof(markerPath), "%s/generalsonline_session.txt", internalPath);
		FILE* f = fopen(markerPath, "r");
		if (f == nullptr)
		{
			fprintf(stderr, "DEBUG-ONLINE: ReadAndroidSession bail -- could not open '%s' (not signed in yet?)\n", markerPath);
			fflush(stderr);
			return false;
		}

		char line[2048];
		while (fgets(line, sizeof(line), f) != nullptr)
		{
			size_t len = strlen(line);
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			{
				line[--len] = '\0';
			}

			char* eq = strchr(line, '=');
			if (eq == nullptr)
			{
				continue;
			}
			*eq = '\0';
			const char* key = line;
			const char* value = eq + 1;

			if (strcmp(key, "session_token") == 0) outSession.sessionToken = value;
			else if (strcmp(key, "user_id") == 0) outSession.userId = value;
			else if (strcmp(key, "display_name") == 0) outSession.displayName = value;
			else if (strcmp(key, "ws_uri") == 0) outSession.wsUri = value;
			else if (strcmp(key, "machine_guid") == 0) outSession.machineGuid = value;
			else if (strcmp(key, "mac_addr") == 0) outSession.macAddr = value;
			else if (strcmp(key, "vol_serial") == 0) outSession.volSerial = value;
			else if (strcmp(key, "refresh_token") == 0) outSession.refreshToken = value;
		}
		fclose(f);

		bool bValid = !outSession.sessionToken.empty() && !outSession.wsUri.empty();
		fprintf(stderr, "DEBUG-ONLINE: ReadAndroidSession parsed '%s' -- hasToken=%d hasWsUri=%d valid=%d\n",
			markerPath, !outSession.sessionToken.empty(), !outSession.wsUri.empty(), (int)bValid);
		fflush(stderr);
		return bValid;
	}
}
#endif // __ANDROID__

void GeneralsOnline_GetDeviceIdentity(std::string& outMachineGuid,
	std::string& outMacAddr, std::string& outVolSerial)
{
	outMachineGuid.clear();
	outMacAddr.clear();
	outVolSerial.clear();

#if defined(__ANDROID__)
	// Read once and keep. This is called from the auth path, which can run
	// on every session refresh; the marker file only changes at sign-in, and
	// a sign-in means a fresh process anyway.
	//
	// Deliberately ignores ReadAndroidSession's validity result: a refresh
	// runs precisely when the session token has gone stale, and the identity
	// fields are still the right ones to send in that case.
	static bool s_read = false;
	static std::string s_machineGuid;
	static std::string s_macAddr;
	static std::string s_volSerial;

	if (!s_read)
	{
		AndroidSession session;
		ReadAndroidSession(session);
		s_machineGuid = session.machineGuid;
		s_macAddr = session.macAddr;
		s_volSerial = session.volSerial;
		s_read = true;
	}

	outMachineGuid = s_machineGuid;
	outMacAddr = s_macAddr;
	outVolSerial = s_volSerial;
#endif
}

#if defined(__ANDROID__)
namespace
{
	std::string MarkerPath()
	{
		const char* internalPath = SDL_GetAndroidInternalStoragePath();
		return internalPath != nullptr ? std::string(internalPath) + "/generalsonline_session.txt" : std::string();
	}

	// Launchers up to 1.3.0 keep the refresh token only in their SharedPreferences, an XML file in
	// this same app's data directory (files/../shared_prefs). A JWT is [A-Za-z0-9._-] only, so it
	// needs no XML unescaping.
	bool ReadRefreshTokenFromLauncherPrefs(std::string& out)
	{
		const char* internalPath = SDL_GetAndroidInternalStoragePath();
		if (internalPath == nullptr)
		{
			return false;
		}
		std::string path = std::string(internalPath) + "/../shared_prefs/generalsonline_session.xml";
		FILE* f = fopen(path.c_str(), "r");
		if (f == nullptr)
		{
			return false;
		}
		std::string xml;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		{
			xml.append(buf, n);
		}
		fclose(f);
		const std::string key = "<string name=\"refresh_token\">";
		size_t start = xml.find(key);
		if (start == std::string::npos)
		{
			return false;
		}
		start += key.size();
		size_t end = xml.find("</string>", start);
		if (end == std::string::npos || end == start)
		{
			return false;
		}
		out = xml.substr(start, end - start);
		return true;
	}
}
#endif

bool GeneralsOnline_ReadStoredRefreshToken(std::string& outRefreshToken)
{
	outRefreshToken.clear();
#if defined(__ANDROID__)
	AndroidSession session;
	ReadAndroidSession(session);
	if (!session.refreshToken.empty())
	{
		outRefreshToken = session.refreshToken;
		return true;
	}
	return ReadRefreshTokenFromLauncherPrefs(outRefreshToken);
#else
	return false;
#endif
}

bool GeneralsOnline_StoreRenewedSession(const std::string& sessionToken, const std::string& refreshToken)
{
#if defined(__ANDROID__)
	std::string path = MarkerPath();
	if (path.empty())
	{
		return false;
	}
	std::string content;
	FILE* in = fopen(path.c_str(), "r");
	if (in != nullptr)
	{
		char line[4096];
		while (fgets(line, sizeof(line), in) != nullptr)
		{
			if (strncmp(line, "session_token=", 14) == 0 || strncmp(line, "refresh_token=", 14) == 0)
			{
				continue;
			}
			content += line;
			if (!content.empty() && content.back() != '\n')
			{
				content += '\n';
			}
		}
		fclose(in);
	}
	content = "session_token=" + sessionToken + "\n" + content;
	if (!refreshToken.empty())
	{
		content += "refresh_token=" + refreshToken + "\n";
	}

	std::string tmp = path + ".tmp";
	FILE* out = fopen(tmp.c_str(), "w");
	if (out == nullptr)
	{
		return false;
	}
	bool bOk = fwrite(content.data(), 1, content.size(), out) == content.size();
	bOk = (fclose(out) == 0) && bOk;
	if (!bOk || rename(tmp.c_str(), path.c_str()) != 0)
	{
		remove(tmp.c_str());
		return false;
	}
	return true;
#else
	(void)sessionToken;
	(void)refreshToken;
	return false;
#endif
}

bool TryStartGeneralsOnline()
{
#if defined(__ANDROID__)
	fprintf(stderr, "DEBUG-ONLINE: TryStartGeneralsOnline enter\n");
	fflush(stderr);
	AndroidSession session;
	if (!ReadAndroidSession(session))
	{
		// Not signed in yet -- the player needs to use the GeneralsOnline
		// Account screen in the Settings app first. Fall through to the
		// legacy path (which will show its own "can't connect" message);
		// a friendlier prompt here is a follow-up.
		fprintf(stderr, "DEBUG-ONLINE: TryStartGeneralsOnline -- no valid session, falling through to legacy path\n");
		fflush(stderr);
		return false;
	}
	fprintf(stderr, "DEBUG-ONLINE: TryStartGeneralsOnline -- session found, userId=%s displayName=%s\n",
		session.userId.c_str(), session.displayName.c_str());
	fflush(stderr);

	if (NGMP_OnlineServicesManager::GetInstance() == nullptr)
	{
		NGMP_OnlineServicesManager::CreateInstance();
		NGMP_OnlineServicesManager::GetInstance()->Init();
	}

	// GeneralsX @bugfix Android port 10/07/2026 the launcher already exchanged
	// its device code for a session token before the game even started; that
	// token was being read from the marker file and then silently discarded
	// here, so NGMP_OnlineServices_AuthInterface::IsLoggedIn() stayed false
	// and every authenticated call (GetFriendsList/GetBlockList, fired right
	// after WS connect below) went out with no Authorization header and got
	// rejected 401 by the server (confirmed via a real device log).
	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	if (pAuthInterface != nullptr)
	{
		int64_t userID = static_cast<int64_t>(std::strtoll(session.userId.c_str(), nullptr, 10));
		pAuthInterface->SetExternalSession(session.sessionToken, userID, session.displayName);

		// GeneralsX @bugfix Android port 11/07/2026 upstream's real login
		// flow (NGMP_OnlineServices_AuthInterface::BeginLogin()) always calls
		// this before reaching the Welcome screen -- it's what actually
		// fetches the MOTD (ProcessMOTD()) that WOLWelcomeMenu.cpp's news
		// listbox reads. Our Android glue bypasses BeginLogin() entirely
		// (the launcher already has a session token), so the MOTD was never
		// fetched and the listbox only ever showed its fallback string.
		// This call doesn't need auth (empty headers) and doesn't block --
		// it races the WebSocket connect below, but a single small MOTD GET
		// is comfortably faster in practice.
		pAuthInterface->GoToDetermineNetworkCaps();
	}

	ClearGSMessageBoxes();
	GSMessageBoxNoButtons(UnicodeString(L"GeneralsOnline"), UnicodeString(L"Connecting..."), false);

	const std::string wsUri = session.wsUri;
	auto connect = [wsUri]()
	{
		fprintf(stderr, "DEBUG-ONLINE: TryStartGeneralsOnline -- calling OnLogin, wsUri=%s\n", wsUri.c_str());
		fflush(stderr);
		NGMP_OnlineServicesManager::GetInstance()->OnLogin(ELoginResult::Success, wsUri.c_str(), []()
			{
				// GeneralsX @feature Android port 11/07/2026 the real upstream
				// WOLWelcomeMenu (ported from GeneralsOnlineDevelopmentTeam/
				// GameClient) replaces our earlier hand-rolled GeneralsOnlineHome
				// screen -- same entry point upstream's own MainMenu -> Online
				// button flow uses.
				fprintf(stderr, "DEBUG-ONLINE: OnLogin callback fired, pushing WOLWelcomeMenu.wnd\n");
				fflush(stderr);
				ClearGSMessageBoxes();
				TheShell->push("Menus/WOLWelcomeMenu.wnd");
			});
		fprintf(stderr, "DEBUG-ONLINE: TryStartGeneralsOnline -- OnLogin call returned, exiting\n");
		fflush(stderr);
	};

	// GeneralsX @bugfix Android port 03/10/2026 The marker's session token can be close to its
	// fifteen-minute expiry, or past it (the launcher's launch-time refresh failed, or the game sat
	// in the main menu): renew it before connecting with it. If it cannot be renewed and is already
	// dead, connecting would only be refused (HTTP 401 on the WebSocket upgrade): say so instead,
	// and give the main menu back.
	if (pAuthInterface != nullptr && pAuthInterface->SessionTokenExpiresWithin(60))
	{
		fprintf(stderr, "DEBUG-ONLINE: TryStartGeneralsOnline -- session token (nearly) expired, renewing first\n");
		fflush(stderr);
		pAuthInterface->RefreshToken([connect, pAuthInterface](bool bRenewed)
			{
				if (!bRenewed && pAuthInterface->SessionTokenExpiresWithin(0))
				{
					fprintf(stderr, "DEBUG-ONLINE: TryStartGeneralsOnline -- session could not be renewed, not connecting\n");
					fflush(stderr);
					AbortGeneralsOnlineStart(true, nullptr);
					return;
				}
				connect();
			});
	}
	else
	{
		connect();
	}

	return true;
#else
	return false;
#endif
}
