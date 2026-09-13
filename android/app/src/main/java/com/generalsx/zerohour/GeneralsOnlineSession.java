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

// GeneralsX @bugfix Android port 12/07/2026
//
// Shared GeneralsOnline session store + HTTP auth calls, extracted from
// GeneralsOnlineActivity so the GAME activity can refresh the session too.
//
// Why: the native game reads a static session_token from the marker file
// written at sign-in time. GeneralsOnline session tokens expire server-side
// after a few hours, so a player who signed in earlier in the day got
// "Could not connect to GeneralsOnline (HTTP response code said error)"
// (WebSocket + MOTD both rejected 401, confirmed by device log) even though
// their sign-in "looked" fine. The launcher already caches a refresh_token
// and knows how to trade it for a fresh session (LoginWithToken) -- the fix
// is simply to do that on every game launch, before native code reads the
// marker file, which GeneralsZHActivity.onCreate() now does via
// refreshSessionAsync().

package com.generalsx.zerohour;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

final class GeneralsOnlineSession {

    private static final String TAG = "GeneralsOnlineSession";

    static final String API_BASE = "https://api.playgenerals.online/env/prod/contract/1/";

    // GeneralsX @bugfix Android port 08/30/2026 Mirrors Network_
    // UseAlternativeEndpoint() in the native client (GeneralsOnline_Settings.h/
    // OnlineServices_Init.cpp), which exists specifically because some
    // players can't reach api.playgenerals.online directly (ISP/DNS-level
    // filtering is the documented reason for that setting existing at all)
    // even though the login WEBSITE (www.playgenerals.online, a different
    // host) works fine for them. The native client only exposes this as a
    // manual settings toggle; this launcher has no settings screen for it
    // yet, so postJson() below falls back to it automatically whenever the
    // primary host doesn't answer.
    static final String API_BASE_ALT = "https://api-ru.playgenerals.online/env/prod/contract/1/";

    static final String PREFS_NAME = "generalsonline_session";
    static final String PREF_SESSION_TOKEN = "session_token";
    static final String PREF_REFRESH_TOKEN = "refresh_token";
    static final String PREF_USER_ID = "user_id";
    static final String PREF_DISPLAY_NAME = "display_name";
    static final String PREF_WS_URI = "ws_uri";

    // Native code reads this -- same plain-marker-file convention as
    // gamedata_path.txt (see GeneralsOnline_AndroidGlue.cpp).
    static final String SESSION_MARKER_NAME = "generalsonline_session.txt";

    // GeneralsX @bugfix Android port 13/09/2026 Moved here from
    // GeneralsOnlineActivity, and changed from "custom_third_party_client"
    // to the id the engine itself sends. That invented value is why sign-in
    // failed with the website reporting success.
    //
    // The login URL carries the game code to the website, and the website
    // hands it to the API once the user finishes with Discord/Steam. The
    // only thing that survives that round-trip is the OAuth "state":
    //
    //   {"type":1, "code":"<gamecode>", "env":"prod", "login_type":"0"}
    //
    // There is no client field in it, and the page drops our &client=
    // parameter entirely -- the string never reaches the server by that
    // route at all. So the server records the pending login under its own
    // default client, we then polled CheckLogin claiming to be
    // "custom_third_party_client", the two did not match, and every poll
    // was refused. The user saw "Welcome Back" on the site and "not signed
    // in" in the launcher, for as long as they cared to wait.
    //
    // GENERALS_ONLINE_CLIENT_ID in NextGenMP_defines.h is "gen_online_30hz"
    // and the engine has always sent exactly that, so this is not the
    // launcher claiming to be something it is not -- it is the launcher
    // finally agreeing with the game it launches. A session obtained under
    // one client id and then used by a process announcing another was never
    // going to be sound anyway.
    static final String CLIENT_ID = "gen_online_30hz";

    static class AuthResult {
        int state = -1;
        String sessionToken = "";
        String refreshToken = "";
        long userId = -1;
        String displayName = "";
        String wsUri = "";

        // GeneralsX @bugfix Android port 13/09/2026 The HTTP status that
        // carried this answer. The API uses the status and the "result"
        // field to say different things -- 403 + result:2 is "that code
        // has not been claimed yet", which is the normal answer to every
        // poll before the user finishes on the website, while 423 is a
        // ban and needs its own message. A caller that sees only "result"
        // cannot tell those apart, so it is recorded here.
        int httpStatus = -1;
        String banReason = "";
    }

    // GeneralsX @bugfix Android port 08/30/2026 A user reported the network-
    // error screen with no way to see WHY -- no adb, no logcat access, just
    // a generic "check your connection" string. This captures the actual
    // failure (host tried, HTTP status + a body snippet, or the exception)
    // from the most recent postJson() call so the caller can put it right
    // on screen. Single mutable field is fine: this launcher only ever runs
    // one login/refresh attempt at a time.
    static volatile String lastNetworkErrorDetail = "";

    private GeneralsOnlineSession() {
    }

    // Runs on a background thread.
    //
    // GeneralsX @bugfix Android port 13/09/2026 Rewritten. This used to
    // treat every non-2xx status as a transport failure and return null,
    // on the theory that a 403 meant an ISP/WAF had eaten the request.
    // That was wrong, and it is what broke sign-in.
    //
    // The API answers a CheckLogin for a code the website has not claimed
    // yet with HTTP 403 and a perfectly normal AuthResponse body. That is
    // the answer to EVERY poll between opening the browser and the user
    // finishing the login -- the whole waiting period. Discarding it as a
    // transport failure meant the first poll, one second in, aborted the
    // sign-in that was still perfectly on track. Verified against the live
    // server: an unclaimed code returns 403 with
    // {"result":2,...}, a malformed one returns 401, and neither is a
    // network problem.
    //
    // So: the body decides, the status annotates. The reference client has
    // always worked this way -- its CheckLogin handler parses the body and
    // only ever looks at the status to catch 423 (banned). A response is a
    // transport failure now only when there is no parseable body at all.
    static AuthResult postJson(Context ctx, String endpoint, JSONObject body, String bearerToken) {
        StringBuilder errors = new StringBuilder();
        AuthResult result = postJsonOnce(ctx, API_BASE, endpoint, body, bearerToken, errors);
        if (result != null) {
            lastNetworkErrorDetail = "";
            return result;
        }
        // Only a genuine transport failure gets here, so the alternate host
        // is now a real second chance rather than a second helping of the
        // same answer.
        Log.w(TAG, "primary API endpoint (" + API_BASE + ") unreachable; retrying via alternate endpoint");
        NetworkTrace.write(ctx, "primary endpoint failed, trying " + API_BASE_ALT);
        result = postJsonOnce(ctx, API_BASE_ALT, endpoint, body, bearerToken, errors);
        lastNetworkErrorDetail = errors.toString().trim();
        if (result != null) {
            lastNetworkErrorDetail = "";
        } else {
            Log.w(TAG, "both API endpoints failed for " + endpoint + ": " + lastNetworkErrorDetail);
            NetworkTrace.write(ctx, "both endpoints failed for " + endpoint + ": " + lastNetworkErrorDetail);
        }
        return result;
    }

    private static AuthResult postJsonOnce(Context ctx, String base, String endpoint, JSONObject body,
                                           String bearerToken, StringBuilder errorOut) {
        HttpURLConnection conn = null;
        long t0 = System.currentTimeMillis();
        try {
            URL url = new URL(base + endpoint);
            conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "application/json");
            if (bearerToken != null) {
                conn.setRequestProperty("Authorization", "Bearer " + bearerToken);
            }
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(10000);
            conn.setDoOutput(true);

            NetworkTrace.write(ctx, "POST " + hostOf(base) + "/" + endpoint
                + "  body=" + NetworkTrace.snippet(body.toString(), 300)
                + (bearerToken != null ? "  auth=Bearer " + bearerToken : "  auth=none"));

            try (OutputStream os = conn.getOutputStream()) {
                os.write(body.toString().getBytes(StandardCharsets.UTF_8));
            }

            int status = conn.getResponseCode();
            long ms = System.currentTimeMillis() - t0;

            // A non-2xx response still carries its body on the error stream,
            // and for this API that body is the answer.
            java.io.InputStream in = (status >= 200 && status < 300)
                ? conn.getInputStream() : conn.getErrorStream();
            String raw = in != null ? readAll(in) : "";

            NetworkTrace.write(ctx, "  <- HTTP " + status + " (" + ms + " ms)  "
                + NetworkTrace.snippet(raw, 300));

            if (raw.isEmpty()) {
                // No body at all: nothing to interpret. This is the shape a
                // proxy/WAF rejection actually takes, and the case the
                // alternate-endpoint retry was added for.
                errorOut.append(hostOf(base)).append(": HTTP ").append(status)
                    .append(" with no body; ");
                return null;
            }

            JSONObject json;
            try {
                json = new JSONObject(raw);
            } catch (Exception notJson) {
                // An HTML error page from something between us and the API.
                errorOut.append(hostOf(base)).append(": HTTP ").append(status).append(' ')
                    .append(NetworkTrace.snippet(raw, 200)).append("; ");
                return null;
            }

            AuthResult result = new AuthResult();
            result.httpStatus = status;
            result.state = json.optInt("result", -1);
            result.sessionToken = json.optString("session_token", "");
            result.refreshToken = json.optString("refresh_token", "");
            result.userId = json.optLong("user_id", -1);
            result.displayName = json.optString("display_name", "");
            result.wsUri = json.optString("ws_uri", "");
            result.banReason = json.optString("ban_reason", "");
            return result;
        } catch (Exception e) {
            NetworkTrace.write(ctx, "  <- " + e.getClass().getSimpleName()
                + (e.getMessage() != null ? ": " + e.getMessage() : ""));
            errorOut.append(hostOf(base)).append(": ").append(e.getClass().getSimpleName());
            if (e.getMessage() != null) {
                errorOut.append(": ").append(e.getMessage());
            }
            errorOut.append("; ");
            return null;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private static String hostOf(String base) {
        try {
            return new URL(base).getHost();
        } catch (Exception e) {
            return base;
        }
    }

    private static String readAll(java.io.InputStream in) throws IOException {
        java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream();
        byte[] chunk = new byte[4096];
        int n;
        while ((n = in.read(chunk)) != -1) {
            buf.write(chunk, 0, n);
        }
        return buf.toString("UTF-8");
    }

    // Runs on a background thread. Mirrors the reference client's
    // GetCredentials()/LoginWithToken silent-reauth branch.
    //
    // GeneralsX @bugfix Android port 13/09/2026 Body updated to the current
    // wire contract. Upstream replaced the three placeholder reserved_N
    // fields this used to send with machine_guid / mac_addr / vol_serial
    // (GeneralsOnline GameClient, OnlineServices_Auth.cpp BeginLogin) --
    // we were still sending the retired shape. See NetworkDiagnostics for
    // what those three values are on a device that has none of the three
    // things they name.
    static AuthResult loginWithToken(Context ctx, String refreshToken) {
        JSONObject body = new JSONObject();
        try {
            body.put("machine_guid", NetworkDiagnostics.installId(ctx));
            body.put("mac_addr", NetworkDiagnostics.syntheticMac(ctx));
            body.put("vol_serial", NetworkDiagnostics.syntheticVolumeSerial(ctx));
            body.put("exe_crc", 0);
            body.put("ini_crc", 0);
        } catch (Exception e) {
            return null;
        }
        return postJson(ctx, "LoginWithToken", body, refreshToken);
    }

    static void saveSession(Context ctx, AuthResult result) {
        ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
            .putString(PREF_SESSION_TOKEN, result.sessionToken)
            .putString(PREF_REFRESH_TOKEN, result.refreshToken)
            .putLong(PREF_USER_ID, result.userId)
            .putString(PREF_DISPLAY_NAME, result.displayName)
            .putString(PREF_WS_URI, result.wsUri)
            .apply();

        // Plain marker file for native code -- one "key=value" per line, no
        // secrets beyond what's already only readable by this app's own uid.
        File marker = new File(ctx.getFilesDir(), SESSION_MARKER_NAME);
        try (FileWriter w = new FileWriter(marker, false)) {
            w.write("session_token=" + result.sessionToken + "\n");
            w.write("user_id=" + result.userId + "\n");
            w.write("display_name=" + result.displayName + "\n");
            w.write("ws_uri=" + result.wsUri + "\n");
            // GeneralsX @bugfix Android port 13/09/2026 The engine makes its
            // own auth calls (session refresh, and the login flow if it ever
            // runs without the launcher), and the API now wants the same
            // three identity fields on those. They have to be the SAME
            // values the launcher sent or the server sees one installation
            // as two, so they travel with the session rather than being
            // derived twice -- the engine cannot read SharedPreferences.
            w.write("machine_guid=" + NetworkDiagnostics.installId(ctx) + "\n");
            w.write("mac_addr=" + NetworkDiagnostics.syntheticMac(ctx) + "\n");
            w.write("vol_serial=" + NetworkDiagnostics.syntheticVolumeSerial(ctx) + "\n");
        } catch (IOException e) {
            // Not fatal: the game will report the connection failure itself.
        }
    }

    static void clearSession(Context ctx) {
        ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().clear().apply();
        new File(ctx.getFilesDir(), SESSION_MARKER_NAME).delete();
    }

    /**
     * Fire-and-forget session refresh at game launch: if a refresh_token is
     * cached, trade it for a fresh session token and rewrite the marker file
     * BEFORE the player can reach the Online button (one small HTTPS POST vs
     * tens of seconds of engine startup -- the race is theoretical). On any
     * failure the existing marker is left untouched: if the old token is
     * still valid the game works as before, and if it expired the game shows
     * the same connect error it always did (nothing gets worse offline).
     */
    static void refreshSessionAsync(Context appContext) {
        final Context ctx = appContext.getApplicationContext();
        new Thread(() -> {
            SharedPreferences prefs = ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
            String refreshToken = prefs.getString(PREF_REFRESH_TOKEN, null);
            if (refreshToken == null || refreshToken.isEmpty()) {
                Log.i(TAG, "no cached refresh_token; skipping launch-time session refresh");
                return;
            }
            NetworkTrace.section(ctx, "session refresh at game launch");
            AuthResult result = loginWithToken(ctx, refreshToken);
            if (result != null && result.state == 1) {
                saveSession(ctx, result);
                Log.i(TAG, "session refreshed at launch for user " + result.userId);
                NetworkTrace.write(ctx, "refresh OK for user " + result.userId);
            } else {
                NetworkTrace.write(ctx, "refresh FAILED -- the game will start with the "
                    + "existing session marker, which may be expired");
                Log.w(TAG, "launch-time session refresh failed (state="
                    + (result != null ? result.state : "network-error")
                    + "); keeping existing session marker");
            }
        }, "GeneralsOnlineSessionRefresh").start();
    }
}
