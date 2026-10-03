#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/PluginInterfaces.h"
#include "GameNetwork/GeneralsOnline/GeneralsOnline_AndroidGlue.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include <algorithm>
#include <chrono>
#include <random>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <wincred.h>
#pragma comment(lib, "Crypt32.lib")
#endif
#include "GameNetwork/GameSpyOverlay.h"

#if defined(USE_TEST_ENV)
#define CREDENTIALS_FILENAME "credentials_env_test.json"
#elif !defined(DEBUG) || defined(USE_DEBUG_ON_LIVE_SERVER)
#define CREDENTIALS_FILENAME "credentials.json"
#endif

#include <curl/curl.h>
#include "GameClient/ClientInstance.h"

enum class EAuthResponseResult : int
{
	CODE_INVALID = -1,
	WAITING_USER_ACTION = 0,
	SUCCEEDED = 1,
	FAILED = 2
};

struct AuthResponse
{
	EAuthResponseResult result;
	std::string session_token;
	std::string refresh_token;
	int64_t user_id = -1;
	std::string display_name = "";
	std::string ws_uri = "";

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(AuthResponse, result, session_token, refresh_token, user_id, display_name, ws_uri)
};

struct MOTDResponse
{
	std::string MOTD;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(MOTDResponse, MOTD)
};

std::string GenerateGamecode()
{
#if defined(_DEBUG) && !defined(USE_TEST_ENV) && !defined(USE_DEBUG_ON_LIVE_SERVER)
	return "ILOVECODE";
#else
	std::string result;
	const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	const size_t max_index = sizeof(charset) - 1;

	auto seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::mt19937 generator(seed);
	std::uniform_int_distribution<> distribution(0, max_index - 1);

	for (int i = 0; i < 32; ++i) {
		result += charset[distribution(generator)];
	}

	return result;
#endif
}

void NGMP_OnlineServices_AuthInterface::GoToDetermineNetworkCaps()
{
	// GET MOTD
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("MOTD");
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				MOTDResponse motdResp = jsonObject.get<MOTDResponse>();

				NGMP_OnlineServicesManager::GetInstance()->ProcessMOTD(motdResp.MOTD.c_str());

				ELoginResult loginResult = ELoginResult::Success;

				// WS should be connected by this point
				std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();
				bool bWSConnected = pWS == nullptr ? false : pWS->IsConnected();
				if (!bWSConnected)
				{
					loginResult = ELoginResult::Failed;
				}

				// NOTE: Don't need to get stats here, PopulatePlayerInfoWindows is called as part of going to MP...
				// cache our local stats 
				// 
				// go to next screen
				ClearGSMessageBoxes();

				if (m_cb_LoginPendingCallback != nullptr)
				{
					m_cb_LoginPendingCallback(loginResult);
				}


			}
			catch (...)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "MOTD: Failed to parse response");

				// if MOTD was bad, still proceed, its a soft error
				NGMP_OnlineServicesManager::GetInstance()->ProcessMOTD("Error retrieving MOTD");

				ELoginResult loginResult = ELoginResult::Success;

				// WS should be connected by this point
				std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();;
				bool bWSConnected = pWS == nullptr ? false : pWS->IsConnected();
				if (!bWSConnected)
				{
					loginResult = ELoginResult::Failed;
				}

				// NOTE: Don't need to get stats here, PopulatePlayerInfoWindows is called as part of going to MP...
				// cache our local stats 
				// 
				// go to next screen
				ClearGSMessageBoxes();

				if (m_cb_LoginPendingCallback != nullptr)
				{
					m_cb_LoginPendingCallback(loginResult);
				}
			}
		});
}

void NGMP_OnlineServices_AuthInterface::BeginLogin()
{
	std::string strLoginURI = NGMP_OnlineServicesManager::GetAPIEndpoint("LoginWithToken");

	std::string strRefreshToken;
	bool bValidCreds = GetCredentials(strRefreshToken);
	if (bValidCreds)
	{
		// login
		std::map<std::string, std::string> mapHeaders;

		// GeneralsX @bugfix Android port 13/09/2026 reserved_0/1/2 retired
		// upstream in favour of the three identity fields; see
		// GeneralsOnline_AndroidGlue.h for where these values come from on
		// a device that has none of the hardware they name.
		std::string strMachineGuid, strMacAddr, strVolSerial;
		GeneralsOnline_GetDeviceIdentity(strMachineGuid, strMacAddr, strVolSerial);

		nlohmann::json j;
		j["machine_guid"] = strMachineGuid;
		j["mac_addr"] = strMacAddr;
		j["vol_serial"] = strVolSerial;
		j["exe_crc"] = TheGlobalData->m_exeCRC;
		j["ini_crc"] = TheGlobalData->m_iniCRC;
		std::string strPostData = j.dump();

		// attach refresh token
		mapHeaders["Authorization"] = "Bearer " + strRefreshToken;


		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strLoginURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				// if 4XX, just log in again
				if (statusCode >= 400 && statusCode < 500)
				{
					if (statusCode == 423)
					{
						ClearGSMessageBoxes();
						GSMessageBoxOk(UnicodeString(L"Account Banned"), UnicodeString(L"You are banned. You can file an appeal in Discord."), []()
							{
								TheShell->pop();
							});
						return;
					}
					else
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed due to 4XX code, trying to re-auth");
						DoReAuth();
					}
				}
				else
				{
					try
					{
						nlohmann::json jsonObject = nlohmann::json::parse(strBody, nullptr, false, true);
						AuthResponse authResp = jsonObject.get<AuthResponse>();

						if (authResp.result == EAuthResponseResult::SUCCEEDED)
						{
							ClearGSMessageBoxes();
							GSMessageBoxNoButtons(UnicodeString(L"Logging In"), UnicodeString(L"Logged in!"), true);

							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Logged in");
							m_bWaitingLogin = false;

							SaveCredentials(authResp.refresh_token.c_str());

							// store data locally
							m_strToken = authResp.session_token;
							m_userID = authResp.user_id;
							m_strDisplayName = authResp.display_name;

							// trigger callback
							OnLoginComplete(ELoginResult::Success, authResp.ws_uri.c_str());
						}
						else if (authResp.result == EAuthResponseResult::FAILED)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed, trying to re-auth");
							DoReAuth();
						}
					}
					catch (...)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Resp parse failed, trying to re-auth");
						DoReAuth();
					}
				}

			}, nullptr);
	}
	else
	{
		m_bWaitingLogin = true;
		m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		m_strCode = GenerateGamecode();

#if defined(USE_TEST_ENV)
		std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}&client={}&env=test", m_strCode.c_str(), GENERALS_ONLINE_CLIENT_ID);
#else
		std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}&client={}", m_strCode.c_str(), GENERALS_ONLINE_CLIENT_ID);
#endif

		ClearGSMessageBoxes();
		GSMessageBoxCancel(UnicodeString(L"Logging In"), UnicodeString(L"Please continue in your web browser"), []()
			{
                if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
                {
                    NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::USER_REQUESTED_SILENT);
                }

				NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
				if (pAuthInterface != nullptr)
				{
					pAuthInterface->OnLoginComplete(ELoginResult::UserCancelled, "");
				}
			});

#if defined(_WIN32) && (!defined(_DEBUG) || defined(USE_TEST_ENV) || defined(USE_DEBUG_ON_LIVE_SERVER))
		ShellExecuteA(NULL, "open", strURI.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif !defined(_WIN32)
		// GeneralsX @bugfix Android port 10/07/2026 On Android the browser is
		// already open and the login already in flight by the time the native
		// engine ever calls BeginLogin -- the Android launcher (GeneralsOnlineActivity)
		// owns code generation + browser launch + polling; the native engine
		// picks up its finished session via the marker file it writes instead
		// (see the Online-button wiring in MainMenuUtils.cpp). This ShellExecute
		// path only matters on desktop builds where the game itself is the client.
#endif
			

			
	}
}

void NGMP_OnlineServices_AuthInterface::DoReAuth()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: DoReAuth");
	ClearGSMessageBoxes();
    GSMessageBoxCancel(UnicodeString(L"Logging In"), UnicodeString(L"Please continue in your web browser"), []()
        {
            if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
            {
                NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::USER_REQUESTED_SILENT);
            }

            NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
            if (pAuthInterface != nullptr)
            {
				pAuthInterface->OnLoginComplete(ELoginResult::UserCancelled , "");
            }
        });

	// do normal login flow, token is bad or expired etc
	m_bWaitingLogin = true;
	m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	m_strCode = GenerateGamecode();

#if defined(USE_TEST_ENV)
	std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}&client={}&env=test", m_strCode.c_str(), GENERALS_ONLINE_CLIENT_ID);
#else
	std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}&client={}", m_strCode.c_str(), GENERALS_ONLINE_CLIENT_ID);
#endif

#if defined(_WIN32) && (!defined(_DEBUG) || defined(USE_TEST_ENV) || defined(USE_DEBUG_ON_LIVE_SERVER))
	ShellExecuteA(NULL, "open", strURI.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif !defined(_WIN32)
	// GeneralsX @bugfix Android port 10/07/2026 see BeginLogin() above -- the
	// Android launcher already owns the browser/code flow.
#endif
}

// GeneralsX @bugfix Android port 03/10/2026 Session renewal -- see OnlineServices_Auth.h and
// GeneralsOnline_AndroidGlue.h. Adapted from upstream's RefreshToken()/OnRefreshTokenFailed(): the
// schedule follows the token's own expiry rather than a fixed ten minutes after creation, because
// here the session can come from the launcher minutes (or, after a failed launch-time refresh, much
// longer) before the engine sees it.
static int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool Base64UrlDecode(const std::string& in, std::string& out)
{
	out.clear();
	int val = 0;
	int bits = -8;
	for (char c : in)
	{
		int d;
		if (c >= 'A' && c <= 'Z') d = c - 'A';
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
		else if (c >= '0' && c <= '9') d = c - '0' + 52;
		else if (c == '-' || c == '+') d = 62;
		else if (c == '_' || c == '/') d = 63;
		else if (c == '=') break;
		else return false;
		val = (val << 6) | d;
		bits += 6;
		if (bits >= 0)
		{
			out.push_back(static_cast<char>((val >> bits) & 0xFF));
			bits -= 8;
		}
	}
	return true;
}

int64_t NGMP_OnlineServices_AuthInterface::TokenExpirySeconds(const std::string& strToken)
{
	size_t first = strToken.find('.');
	size_t second = first == std::string::npos ? std::string::npos : strToken.find('.', first + 1);
	if (second == std::string::npos)
	{
		return -1;
	}
	std::string payload;
	if (!Base64UrlDecode(strToken.substr(first + 1, second - first - 1), payload))
	{
		return -1;
	}
	nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
	if (j.is_discarded() || !j.contains("exp") || !j["exp"].is_number())
	{
		return -1;
	}
	return j["exp"].get<int64_t>();
}

bool NGMP_OnlineServices_AuthInterface::SessionTokenExpiresWithin(int secondsAhead) const
{
	int64_t exp = TokenExpirySeconds(m_strToken);
	return exp < 0 || exp * 1000 <= NowMs() + static_cast<int64_t>(secondsAhead) * 1000;
}

void NGMP_OnlineServices_AuthInterface::ScheduleTokenRefresh()
{
	int64_t exp = TokenExpirySeconds(m_strToken);
	int64_t now = NowMs();
	// A token whose expiry cannot be read is renewed on upstream's ten-minute schedule.
	int64_t at = exp > 0 ? (exp - m_secondsBeforeExpiryToRefresh) * 1000 : now + 10 * 60 * 1000;
	m_nextTokenRefreshTime = at > now ? at : now;
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Session token expires in %llds, renewal in %llds",
		exp > 0 ? (long long)(exp - now / 1000) : -1LL, (long long)((m_nextTokenRefreshTime - now) / 1000));
}

void NGMP_OnlineServices_AuthInterface::OnRefreshTokenFailed(const char* szReason, const std::string& strBody, bool bFinal)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Token renewal attempt %d failed (%s): %s",
		m_currentRefreshAttempt, szReason, strBody.substr(0, 256).c_str());

	// The session token stays usable until its expiry, so keep trying until then; a renewal that
	// fails once the token is gone (and has had a retry) ends the online session.
	bool bTokenAlive = !SessionTokenExpiresWithin(0);
	if (!bFinal && (bTokenAlive || m_currentRefreshAttempt < 2))
	{
		m_nextRefreshRetryTime = NowMs() + m_secondsUntilRefreshRetry * 1000;
		return;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Session could not be renewed, ending the online session");
	m_nextRefreshRetryTime = -1;
	m_nextTokenRefreshTime = -1;
	m_currentRefreshAttempt = 0;
	NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::AUTH_FAILED);
}

void NGMP_OnlineServices_AuthInterface::RefreshToken(std::function<void(bool bRenewed)> onDone)
{
	if (m_bRefreshInFlight)
	{
		if (onDone) onDone(false);
		return;
	}

	// Read every time: the launcher may have rotated it since the last renewal.
	std::string strRefreshToken;
	if (!GeneralsOnline_ReadStoredRefreshToken(strRefreshToken) || strRefreshToken.empty())
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] No refresh token stored, the session cannot be renewed");
		m_nextTokenRefreshTime = -1;
		m_nextRefreshRetryTime = -1;
		if (onDone) onDone(false);
		return;
	}

	++m_currentRefreshAttempt;
	m_nextRefreshRetryTime = -1;
	m_nextTokenRefreshTime = -1;
	m_bRefreshInFlight = true;
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Renewing the session token (attempt %d)", m_currentRefreshAttempt);

	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("RefreshToken");
	std::map<std::string, std::string> mapHeaders;
	mapHeaders["Authorization"] = "Bearer " + strRefreshToken;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "",
		[=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			m_bRefreshInFlight = false;

			if (statusCode == 423)
			{
				OnRefreshTokenFailed("account suspended (HTTP 423)", strBody, true);
				if (onDone) onDone(false);
				return;
			}
			if (!bSuccess || statusCode < 200 || statusCode >= 300)
			{
				char reason[64];
				snprintf(reason, sizeof(reason), "HTTP %d", statusCode);
				OnRefreshTokenFailed(reason, strBody, false);
				if (onDone) onDone(false);
				return;
			}

			nlohmann::json j = nlohmann::json::parse(strBody, nullptr, false);
			std::string strSession = (!j.is_discarded() && j.contains("session_token") && j["session_token"].is_string())
				? j["session_token"].get<std::string>() : std::string();
			std::string strRefresh = (!j.is_discarded() && j.contains("refresh_token") && j["refresh_token"].is_string())
				? j["refresh_token"].get<std::string>() : std::string();
			if (strSession.empty())
			{
				OnRefreshTokenFailed("no session_token in the response", strBody, false);
				if (onDone) onDone(false);
				return;
			}

			m_strToken = strSession;
			m_currentRefreshAttempt = 0;
			// The old refresh token is single use now: the new one must be on disk before anything
			// else can need it, or the next renewal -- ours or the launcher's -- is refused.
			if (!GeneralsOnline_StoreRenewedSession(strSession, strRefresh.empty() ? strRefreshToken : strRefresh))
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Session renewed, but writing it to the session file failed");
			}
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Session renewed");
			ScheduleTokenRefresh();
			if (onDone) onDone(true);
		}, nullptr, -1, true /* the refresh token authenticates this request, not the session token */);
}

void NGMP_OnlineServices_AuthInterface::Tick()
{
	if (IsLoggedIn() && !m_bRefreshInFlight)
	{
		int64_t now = NowMs();
		if (m_nextRefreshRetryTime != -1 ? now >= m_nextRefreshRetryTime
			: (m_nextTokenRefreshTime != -1 && now >= m_nextTokenRefreshTime))
		{
			RefreshToken();
		}
	}

	if (m_bWaitingLogin)
	{
		const int64_t timeBetweenChecks = 1000;
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		if (currTime - m_lastCheckCode >= timeBetweenChecks)
		{
			m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

			// check again
			std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("CheckLogin");
			std::map<std::string, std::string> mapHeaders;

			std::string strMachineGuid, strMacAddr, strVolSerial;
			GeneralsOnline_GetDeviceIdentity(strMachineGuid, strMacAddr, strVolSerial);

			nlohmann::json j;
			j["code"] = m_strCode.c_str();
			j["client_id"] = GENERALS_ONLINE_CLIENT_ID;
			j["machine_guid"] = strMachineGuid;
			j["mac_addr"] = strMacAddr;
			j["vol_serial"] = strVolSerial;
			j["exe_crc"] = TheGlobalData->m_exeCRC;
            j["ini_crc"] = TheGlobalData->m_iniCRC;
			std::string strPostData = j.dump();

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{
					try
					{
						if (statusCode == 423)
						{
							m_bWaitingLogin = false;
							ClearGSMessageBoxes();
							GSMessageBoxOk(UnicodeString(L"Account Banned"), UnicodeString(L"You are banned. You can file an appeal in Discord."), []()
								{
									TheShell->pop();
								});
							return;
						}

						nlohmann::json jsonObject = nlohmann::json::parse(strBody);
						AuthResponse authResp = jsonObject.get<AuthResponse>();

						// GeneralsX @bugfix Android port 02/10/2026 The auth reply carries the session and
						// refresh tokens; players share these logs. Upstream dabb98b81.
#if _DEBUG
						NetworkLog(ELogVerbosity::LOG_RELEASE, "PageBody: %s", strBody.c_str());
#endif
						if (authResp.result == EAuthResponseResult::CODE_INVALID)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Code didnt exist, trying again soon");
						}
						else if (authResp.result == EAuthResponseResult::WAITING_USER_ACTION)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Waiting for user action");
						}
						else if (authResp.result == EAuthResponseResult::SUCCEEDED)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Logged in");
							m_bWaitingLogin = false;

							SaveCredentials(authResp.refresh_token.c_str());

							// store data locally
							m_strToken = authResp.session_token;
							m_userID = authResp.user_id;
							m_strDisplayName = authResp.display_name;

							// trigger callback
							OnLoginComplete(ELoginResult::Success, authResp.ws_uri.c_str());
						}
						else if (authResp.result == EAuthResponseResult::FAILED)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed");
							m_bWaitingLogin = false;

							// trigger callback
							OnLoginComplete(ELoginResult::Failed, "");
						}
					}
					catch (...)
					{

					}

				}, nullptr);
		}
	}
}

void NGMP_OnlineServices_AuthInterface::OnLoginComplete(ELoginResult loginResult, const char* szWSAddr)
{
	if (loginResult == ELoginResult::Success)
	{
		// GeneralsX @bugfix Android port 11/07/2026 - Match upstream: notify the anticheat
		// plugin interface of login. This is a no-op on the non-Windows stub (no real plugin
		// is ever loaded on Android), kept for parity in case the server tracks this signal.
		AnticheatPlugInterface::Authenticate();

		NGMP_OnlineServicesManager::GetInstance()->OnLogin(loginResult, szWSAddr, [=]() // wait for WS to connect
			{
                // move on to network capabilities section
                ClearGSMessageBoxes();
                GoToDetermineNetworkCaps();
			});
	}
	else
	{
		if (m_cb_LoginPendingCallback != nullptr)
		{
			m_cb_LoginPendingCallback(loginResult);
		}

		TheShell->pop();
	}
}

void NGMP_OnlineServices_AuthInterface::LogoutOfMyAccount()
{
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("User"), m_userID);
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendDELETERequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "", nullptr);

	// delete local credentials cache
	std::string strCredentialsCachePath = GetCredentialsFilePath();

	if (std::filesystem::exists(strCredentialsCachePath))
	{
		std::filesystem::remove(strCredentialsCachePath);
	}
}

void NGMP_OnlineServices_AuthInterface::LoginAsSecondaryDevAccount()
{

}

void NGMP_OnlineServices_AuthInterface::SaveCredentials(const char* szRefreshToken)
{
	// store in data dir
	nlohmann::json root = { {"refresh_token", szRefreshToken} };

	std::string strData = root.dump(1);

	FILE* file = fopen(GetCredentialsFilePath().c_str(), "wb");
	if (file)
	{
#if defined(GENERALS_ONLINE_ENCRYPT_CREDENTIALS)
		DATA_BLOB inputBlob;
		DATA_BLOB outputBlob;

		inputBlob.pbData = (BYTE*)strData.c_str();
		inputBlob.cbData = static_cast<DWORD>(strData.size());

		if (CryptProtectData(&inputBlob, L"GO Credentials", nullptr, nullptr, nullptr, 0, &outputBlob))
		{
			fwrite(outputBlob.pbData, 1, outputBlob.cbData, file);
		}
		else
		{
			// TODO_JWT: Handle failure case
		}
#else
		fwrite(strData.data(), 1, strData.size(), file);
#endif

		fclose(file);
	}
}

bool NGMP_OnlineServices_AuthInterface::GetCredentials(std::string& strRefreshToken)
{
#if defined(_DEBUG) && !defined(USE_TEST_ENV) && !defined(USE_DEBUG_ON_LIVE_SERVER)
	return false;
#endif
	std::vector<uint8_t> vecBytes;
	FILE* file = fopen(GetCredentialsFilePath().c_str(), "rb");
	if (file)
	{
		fseek(file, 0, SEEK_END);
		long fileSize = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (fileSize > 0)
		{
			vecBytes.resize(fileSize);
			fread(vecBytes.data(), 1, fileSize, file);
		}
		fclose(file);
	}


	if (!vecBytes.empty())
	{
		// needs decrypt first
#if defined(GENERALS_ONLINE_ENCRYPT_CREDENTIALS)
		DATA_BLOB encryptedBlob;
		encryptedBlob.pbData = const_cast<BYTE*>(vecBytes.data());
		encryptedBlob.cbData = static_cast<DWORD>(vecBytes.size());
		std::string strJSON;

		DATA_BLOB decryptedBlob = { 0 };
		if (CryptUnprotectData(&encryptedBlob, nullptr, nullptr, nullptr, nullptr, 0, &decryptedBlob))
		{
			strJSON = std::string((char*)decryptedBlob.pbData, decryptedBlob.cbData);
			LocalFree(decryptedBlob.pbData); // Free memory allocated by CryptUnprotectData
		}
		else
		{
			// TODO_JWT: Handle failure
		}
#else
		std::string strJSON = std::string((char*)vecBytes.data(), vecBytes.size());
#endif

		
		nlohmann::json jsonCredentials = nullptr;

		try
		{
			jsonCredentials = nlohmann::json::parse(strJSON);

			if (jsonCredentials != nullptr)
			{
				if (jsonCredentials.contains("refresh_token"))
				{
					strRefreshToken = jsonCredentials["refresh_token"];

					if (strRefreshToken.empty())
					{
						return false;
					}

					return true;
				}
			}

		}
		catch (...)
		{
			return false;
		}
	}

	return false;
}

std::string NGMP_OnlineServices_AuthInterface::GetCredentialsFilePath()
{
	// debug supports multi inst, so needs seperate tokens
#if defined(_DEBUG) && !defined(USE_TEST_ENV) && !defined(USE_DEBUG_ON_LIVE_SERVER)
	std::string strCredsPath = std::format("{}/GeneralsOnlineData/credentials_dev_env_{}.json", TheGlobalData->getPath_UserData().str(), rts::ClientInstance::getInstanceIndex());
#else
	std::string strCredsPath = std::format("{}/GeneralsOnlineData/{}", TheGlobalData->getPath_UserData().str(), CREDENTIALS_FILENAME);
#endif
	return strCredsPath;
}
