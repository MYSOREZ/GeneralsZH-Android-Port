#pragma once
#include "NGMP_types.h"

class NGMP_OnlineServices_AuthInterface
{
public:

	std::string GetDisplayName()
	{
		return m_strDisplayName;
	}

	std::wstring GetDisplayNameW()
	{
		return from_utf8(m_strDisplayName);
	}

	int64_t GetUserID() const { return m_userID; }

	void GoToDetermineNetworkCaps();

	void BeginLogin();
	void DoReAuth();

	void Tick();

	void OnLoginComplete(ELoginResult loginResult, const char* szWSAddr);

	void RegisterForLoginCallback(std::function<void(ELoginResult)> callback)
	{
		m_cb_LoginPendingCallback = callback;
	}

	void DeregisterForLoginCallback()
	{
		m_cb_LoginPendingCallback = nullptr;
	}

	std::string& GetAuthToken() { return m_strToken; }

	bool IsLoggedIn() const
	{
		return m_userID != -1 && !m_strToken.empty();
	}

	// GeneralsX @feature Android port 10/07/2026 the Android launcher
	// (GeneralsOnlineActivity.java) already ran its own device-code exchange
	// with playgenerals.online before the game process even starts, so this
	// module's normal BeginLogin()/OnLoginComplete() device-code flow never
	// runs on Android -- m_strToken/m_userID/m_strDisplayName would stay at
	// their unset defaults forever, so every authenticated HTTP/WS call
	// (GetFriendsList, GetBlockList, ...) sends no Authorization header and
	// gets rejected with 401. This lets GeneralsOnline_AndroidGlue.cpp feed in
	// the session the launcher already established.
	void SetExternalSession(const std::string& strToken, int64_t userID, const std::string& strDisplayName)
	{
		m_strToken = strToken;
		m_userID = userID;
		m_strDisplayName = strDisplayName;
		ScheduleTokenRefresh();
	}

	// GeneralsX @bugfix Android port 03/10/2026 Session renewal, after upstream's RefreshToken() (see
	// GeneralsOnline_AndroidGlue.h for why the refresh token comes from the session marker file).
	// Renews when the session token is five minutes from its expiry; a failed renewal is retried
	// every 30 s while the token still lives, and the online session is torn down -- with a message --
	// once it cannot be renewed. onDone, if given, runs once with the outcome.
	void RefreshToken(std::function<void(bool bRenewed)> onDone = nullptr);

	// Whether the session token expires within the next secondsAhead seconds (or cannot be read).
	bool SessionTokenExpiresWithin(int secondsAhead) const;

	void LogoutOfMyAccount();

private:
	void LoginAsSecondaryDevAccount();

	void SaveCredentials(const char* szRefreshToken);
	bool GetCredentials(std::string& strRefreshToken);

	std::string GetCredentialsFilePath();

private:
	bool m_bWaitingLogin = false;
	std::string m_strCode;
	std::int64_t m_lastCheckCode = -1;

	std::string m_strToken = std::string();
	int64_t m_userID = -1;
	std::string m_strDisplayName = "NO_USER";

	std::function<void(ELoginResult)> m_cb_LoginPendingCallback = nullptr;

	// GeneralsX @bugfix Android port 03/10/2026 Session renewal state (RefreshToken()).
	void ScheduleTokenRefresh();
	void OnRefreshTokenFailed(const char* szReason, const std::string& strBody, bool bFinal);
	static int64_t TokenExpirySeconds(const std::string& strToken);

	int64_t m_nextTokenRefreshTime = -1;   // ms since the epoch; -1 = nothing scheduled
	int64_t m_nextRefreshRetryTime = -1;
	int m_currentRefreshAttempt = 0;
	bool m_bRefreshInFlight = false;
	const int m_secondsBeforeExpiryToRefresh = 5 * 60;
	const int m_secondsUntilRefreshRetry = 30;
};
