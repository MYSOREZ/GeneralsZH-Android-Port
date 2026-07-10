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

// GeneralsX @feature Android port 10/07/2026
//
// Account login for GeneralsOnline (playgenerals.online / TheSuperHackers
// GeneralsOnlineServices), the actively-maintained GameSpy replacement for
// Zero Hour multiplayer -- see docs/port/... for how this was chosen over
// Revora/CnC-Online (which retired Generals/ZH support and redirects here).
//
// GeneralsX @bugfix Android port 10/07/2026 First cut of this screen made
// the user manually copy a code FROM the browser INTO the app -- wrong.
// The real client (github.com/GeneralsOnlineDevelopmentTeam/GameClient,
// OnlineServices_Auth.cpp: NGMP_OnlineServices_AuthInterface::BeginLogin)
// generates the code itself, embeds it directly in the URL it opens
// (playgenerals.online/login/?gamecode=<code>), and polls CheckLogin with
// that same code -- the user never sees or types the code at all, only
// picks Steam/Discord/GameReplays on the site and comes back. Ported that
// exact flow here, including the refresh_token cache so repeat logins skip
// the browser entirely (LoginWithToken).

package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;

import org.json.JSONObject;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;

public class GeneralsOnlineActivity extends Activity {

    private static final String API_BASE = "https://api.playgenerals.online/env/prod/contract/1/";
    private static final String LOGIN_URL_FMT = "https://www.playgenerals.online/login/?gamecode=%s";
    private static final String CLIENT_ID = "custom_third_party_client";

    private static final String PREFS_NAME = "generalsonline_session";
    private static final String PREF_SESSION_TOKEN = "session_token";
    private static final String PREF_REFRESH_TOKEN = "refresh_token";
    private static final String PREF_USER_ID = "user_id";
    private static final String PREF_DISPLAY_NAME = "display_name";
    private static final String PREF_WS_URI = "ws_uri";

    // Native code (once the multiplayer client is wired up) reads this --
    // same plain-marker-file convention as gamedata_path.txt.
    private static final String SESSION_MARKER_NAME = "generalsonline_session.txt";

    // Matches the reference client's own 1s poll cadence
    // (OnlineServices_Auth.cpp::Tick, timeBetweenChecks = 1000).
    private static final int POLL_INTERVAL_MS = 1000;
    private static final int POLL_MAX_ATTEMPTS = 180; // ~3 minutes

    private static final String CODE_CHARSET =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    private static final int CODE_LENGTH = 32;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final SecureRandom random = new SecureRandom();

    private TextView statusText;
    private MaterialButton signInButton;
    private MaterialButton signOutButton;

    private int pollAttempt = 0;
    private boolean busy = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onCreate(savedInstanceState);
        setTitle("GeneralsOnline Account");
        buildUi();
        refreshStatus();
        maybeSilentReauth();
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);
        setContentView(scroll);

        TextView title = new TextView(this);
        title.setText("GeneralsOnline Account");
        title.setTextSize(22);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        title.setPadding(dp(4), dp(8), dp(4), dp(4));
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("Sign in for online multiplayer (playgenerals.online)");
        subtitle.setTextSize(14);
        subtitle.setAlpha(0.7f);
        subtitle.setPadding(dp(4), 0, dp(4), dp(16));
        root.addView(subtitle);

        LinearLayout statusCard = startCard(root, null);
        statusText = new TextView(this);
        statusText.setTextIsSelectable(true);
        statusCard.addView(statusText);
        signOutButton = addButton(statusCard, "Sign Out", this::onSignOut);

        LinearLayout stepsCard = startCard(root, "Sign in");
        TextView help = new TextView(this);
        help.setText(
            "Tap Sign In -- a browser opens to playgenerals.online, already carrying a "
            + "one-time code for this device. Pick Steam, Discord, or GameReplays there, "
            + "then just come back to this app; it finishes on its own."
        );
        stepsCard.addView(help);
        signInButton = addButton(stepsCard, "Sign In", this::onSignIn);
    }

    private LinearLayout startCard(LinearLayout root, String header) {
        MaterialCardView card = new MaterialCardView(this);
        LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        cardLp.setMargins(0, 0, 0, dp(12));
        card.setLayoutParams(cardLp);
        card.setRadius(dp(12));
        card.setCardElevation(dp(2));
        card.setCardBackgroundColor(getColor(R.color.gzh_surface));
        card.setContentPadding(dp(16), dp(14), dp(16), dp(14));

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        card.addView(content);
        root.addView(card);

        if (header != null) {
            TextView headerView = new TextView(this);
            headerView.setText(header);
            headerView.setTextSize(15);
            headerView.setTypeface(headerView.getTypeface(), android.graphics.Typeface.BOLD);
            headerView.setPadding(0, 0, 0, dp(8));
            content.addView(headerView);
        }
        return content;
    }

    private MaterialButton addButton(LinearLayout root, String label, Runnable action) {
        MaterialButton b = new MaterialButton(this);
        b.setText(label);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(4), 0, dp(4));
        root.addView(b, lp);
        return b;
    }

    private String generateGameCode() {
        StringBuilder sb = new StringBuilder(CODE_LENGTH);
        for (int i = 0; i < CODE_LENGTH; ++i) {
            sb.append(CODE_CHARSET.charAt(random.nextInt(CODE_CHARSET.length())));
        }
        return sb.toString();
    }

    // If we already have a refresh_token from a previous sign-in, try to
    // silently re-authenticate instead of making the user go through the
    // browser again -- mirrors BeginLogin()'s GetCredentials()/LoginWithToken
    // branch in the reference client.
    private void maybeSilentReauth() {
        String refreshToken = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .getString(PREF_REFRESH_TOKEN, null);
        if (refreshToken == null || refreshToken.isEmpty() || busy) {
            return;
        }
        busy = true;
        signInButton.setEnabled(false);
        statusText.setText("Signing in...");
        new Thread(() -> {
            AuthResult result = callLoginWithToken(refreshToken);
            handler.post(() -> {
                busy = false;
                signInButton.setEnabled(true);
                if (result != null && result.state == 1) {
                    saveSession(result);
                    refreshStatus();
                } else {
                    // Refresh token expired/invalid -- fall back to a fresh
                    // browser sign-in next time the user taps Sign In.
                    clearSession();
                    refreshStatus();
                }
            });
        }).start();
    }

    private void onSignIn() {
        if (busy) {
            return;
        }
        busy = true;
        pollAttempt = 0;
        signInButton.setEnabled(false);

        String code = generateGameCode();
        String url = String.format(LOGIN_URL_FMT, code);
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            busy = false;
            signInButton.setEnabled(true);
            Toast.makeText(this, "No browser available: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }

        statusText.setText("Continue in your browser -- come back here once you've signed in.");
        handler.postDelayed(() -> pollOnce(code), POLL_INTERVAL_MS);
    }

    private void pollOnce(String code) {
        new Thread(() -> {
            AuthResult result = callCheckLogin(code);
            handler.post(() -> handlePollResult(code, result));
        }).start();
    }

    private void handlePollResult(String code, AuthResult result) {
        if (result == null) {
            busy = false;
            signInButton.setEnabled(true);
            statusText.setText("Network error -- check your connection and try again.");
            return;
        }

        switch (result.state) {
            case 1: // SUCCEEDED
                busy = false;
                signInButton.setEnabled(true);
                saveSession(result);
                refreshStatus();
                Toast.makeText(this, "Signed in as " + result.displayName, Toast.LENGTH_LONG).show();
                break;
            case 2: // FAILED
                busy = false;
                signInButton.setEnabled(true);
                statusText.setText("Sign-in failed or was cancelled -- tap Sign In to try again.");
                break;
            case 0: // WAITING_USER_ACTION
            case -1: // CODE_INVALID (not registered yet server-side -- keep polling, it's a timing thing)
                ++pollAttempt;
                if (pollAttempt >= POLL_MAX_ATTEMPTS) {
                    busy = false;
                    signInButton.setEnabled(true);
                    statusText.setText("Timed out waiting -- tap Sign In to try again.");
                } else {
                    handler.postDelayed(() -> pollOnce(code), POLL_INTERVAL_MS);
                }
                break;
            default:
                busy = false;
                signInButton.setEnabled(true);
                statusText.setText("Unexpected response -- tap Sign In to try again.");
                break;
        }
    }

    private static class AuthResult {
        int state = -1;
        String sessionToken = "";
        String refreshToken = "";
        long userId = -1;
        String displayName = "";
        String wsUri = "";
    }

    // Runs on a background thread.
    private AuthResult callCheckLogin(String code) {
        JSONObject body = new JSONObject();
        try {
            body.put("code", code);
            body.put("client_id", CLIENT_ID);
            body.put("reserved_0", "");
            body.put("reserved_1", "");
            body.put("reserved_2", "");
        } catch (Exception e) {
            return null;
        }
        return postJson("CheckLogin", body, null);
    }

    // Runs on a background thread.
    private AuthResult callLoginWithToken(String refreshToken) {
        JSONObject body = new JSONObject();
        try {
            body.put("reserved_0", "");
            body.put("reserved_1", "");
            body.put("reserved_2", "");
        } catch (Exception e) {
            return null;
        }
        return postJson("LoginWithToken", body, refreshToken);
    }

    private AuthResult postJson(String endpoint, JSONObject body, String bearerToken) {
        HttpURLConnection conn = null;
        try {
            URL url = new URL(API_BASE + endpoint);
            conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "application/json");
            if (bearerToken != null) {
                conn.setRequestProperty("Authorization", "Bearer " + bearerToken);
            }
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(10000);
            conn.setDoOutput(true);

            try (OutputStream os = conn.getOutputStream()) {
                os.write(body.toString().getBytes(StandardCharsets.UTF_8));
            }

            int status = conn.getResponseCode();
            java.io.InputStream in = (status >= 200 && status < 300) ? conn.getInputStream() : conn.getErrorStream();
            if (in == null) {
                return null;
            }
            JSONObject json = new JSONObject(readAll(in));

            AuthResult result = new AuthResult();
            result.state = json.optInt("result", -1);
            result.sessionToken = json.optString("session_token", "");
            result.refreshToken = json.optString("refresh_token", "");
            result.userId = json.optLong("user_id", -1);
            result.displayName = json.optString("display_name", "");
            result.wsUri = json.optString("ws_uri", "");
            return result;
        } catch (Exception e) {
            return null;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
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

    private void saveSession(AuthResult result) {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
            .putString(PREF_SESSION_TOKEN, result.sessionToken)
            .putString(PREF_REFRESH_TOKEN, result.refreshToken)
            .putLong(PREF_USER_ID, result.userId)
            .putString(PREF_DISPLAY_NAME, result.displayName)
            .putString(PREF_WS_URI, result.wsUri)
            .apply();

        // Plain marker file for native code, mirroring gamedata_path.txt --
        // one "key=value" per line, no secrets beyond what's already only
        // readable by this app's own uid (same sandboxing as every other
        // marker file this app writes).
        File marker = new File(getFilesDir(), SESSION_MARKER_NAME);
        try (FileWriter w = new FileWriter(marker, false)) {
            w.write("session_token=" + result.sessionToken + "\n");
            w.write("user_id=" + result.userId + "\n");
            w.write("display_name=" + result.displayName + "\n");
            w.write("ws_uri=" + result.wsUri + "\n");
        } catch (IOException e) {
            // Not fatal: native multiplayer code isn't wired up yet anyway.
        }
    }

    private void clearSession() {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().clear().apply();
        new File(getFilesDir(), SESSION_MARKER_NAME).delete();
    }

    private void onSignOut() {
        // TODO: also DELETE /User/{user_id} server-side like the reference
        // client's LogoutOfMyAccount(), once we're sure "sign out" here
        // should mean "forget this device" rather than just "clear local
        // session" -- left local-only for now since that's the safer default.
        clearSession();
        refreshStatus();
        Toast.makeText(this, "Signed out.", Toast.LENGTH_SHORT).show();
    }

    private void refreshStatus() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String displayName = prefs.getString(PREF_DISPLAY_NAME, null);
        String sessionToken = prefs.getString(PREF_SESSION_TOKEN, null);

        if (displayName != null && sessionToken != null && !sessionToken.isEmpty()) {
            statusText.setText("Signed in as " + displayName + ".");
            signOutButton.setEnabled(true);
        } else {
            statusText.setText("Not signed in.");
            signOutButton.setEnabled(false);
        }
    }

    // Static helper so other screens (SetupActivity) can show a one-line
    // status without duplicating the SharedPreferences keys.
    static String getSignedInDisplayName(android.content.Context ctx) {
        SharedPreferences prefs = ctx.getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String displayName = prefs.getString(PREF_DISPLAY_NAME, null);
        String sessionToken = prefs.getString(PREF_SESSION_TOKEN, null);
        if (displayName != null && sessionToken != null && !sessionToken.isEmpty()) {
            return displayName;
        }
        return null;
    }

    private int dp(int value) {
        float density = getResources().getDisplayMetrics().density;
        return (int) (value * density + 0.5f);
    }
}
