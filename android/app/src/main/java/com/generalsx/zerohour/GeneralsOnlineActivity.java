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
import android.widget.TextView;
import android.widget.Toast;

import com.google.android.material.button.MaterialButton;

import org.json.JSONObject;

import java.io.File;


public class GeneralsOnlineActivity extends Activity {

    // GeneralsX @bugfix Android port 12/07/2026 session store + HTTP auth
    // calls moved to GeneralsOnlineSession so GeneralsZHActivity can refresh
    // the session token at game launch (they expire server-side within
    // hours; a stale marker file made the game's Online button fail with
    // "HTTP response code said error"/401 despite a "valid" local session).
    // GeneralsX @bugfix Android port 13/09/2026 &client= dropped. A note
    // here used to claim the reference client always appended it and that
    // the site needed it; upstream's BeginLogin no longer sends it, and the
    // site demonstrably ignores it -- the page is byte-identical with and
    // without, and the parameter appears nowhere in it. What actually
    // broke that July sign-in was the client id itself; see
    // GeneralsOnlineSession.CLIENT_ID.
    private static final String LOGIN_URL_FMT = "https://www.playgenerals.online/login/?gamecode=%s";
    private static final String CLIENT_ID = GeneralsOnlineSession.CLIENT_ID;

    private static final String PREFS_NAME = GeneralsOnlineSession.PREFS_NAME;
    private static final String PREF_SESSION_TOKEN = GeneralsOnlineSession.PREF_SESSION_TOKEN;
    private static final String PREF_REFRESH_TOKEN = GeneralsOnlineSession.PREF_REFRESH_TOKEN;
    private static final String PREF_USER_ID = GeneralsOnlineSession.PREF_USER_ID;
    private static final String PREF_DISPLAY_NAME = GeneralsOnlineSession.PREF_DISPLAY_NAME;
    private static final String PREF_WS_URI = GeneralsOnlineSession.PREF_WS_URI;

    // Matches the reference client's own 1s poll cadence
    // (OnlineServices_Auth.cpp::Tick, timeBetweenChecks = 1000).
    private static final int POLL_INTERVAL_MS = 1000;
    private static final int POLL_MAX_ATTEMPTS = 180; // ~3 minutes

    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView dataPackStatus;
    private com.google.android.material.button.MaterialButton dataPackButton;
    private com.google.android.material.button.MaterialButton dataPackDeleteButton;
    private com.google.android.material.materialswitch.MaterialSwitch dataPackSwitch;
    private TextView dataPackChip;
    private boolean dataPackBusy;
    private boolean dataPackPrompted;
    private TextView crossPlayPatchChip;

    private TextView statusText;
    private MaterialButton signInButton;
    private MaterialButton signOutButton;

    private int pollAttempt = 0;
    private boolean busy = false;

    // GeneralsX @bugfix Android port 13/09/2026 Tapping Sign In again while
    // a sign-in was already running left the first poll loop alive, so two
    // codes were polled a second apart -- double the request rate, with
    // whichever loop answered last owning the screen. Each attempt carries
    // a generation and a superseded result is dropped.
    //
    // Static, and that is the point. The first version of this was an
    // instance field, which did not help at all: the sign-in flow sends the
    // user to a browser, and coming back can bring a NEW Activity instance
    // with it. The old instance's loop kept running -- its handler is bound
    // to the main Looper, not to the Activity -- and checked its OWN
    // generation field, which of course still matched. A successful sign-in
    // log shows exactly that: the abandoned instance polled its dead code
    // for another 26 seconds after the live one had finished.
    //
    // One counter for the process means a new attempt in any instance
    // retires every older loop, while a recreation that does NOT start a
    // new attempt leaves the in-flight sign-in alone -- which matters,
    // since that sign-in is the reason we were sent to the browser.
    private static int signInGeneration = 0;

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.online_window_title);
        buildUi();
        refreshStatus();
        maybeSilentReauth();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // Only when the screen is really being left. A bare recreation --
        // which is what returning from the sign-in browser can look like --
        // must not cancel the sign-in that recreation is the result of.
        if (isFinishing()) {
            ++signInGeneration;
            handler.removeCallbacksAndMessages(null);
        }
    }

    // GeneralsX @feature Android port launcher-ui-2026 08/09/2026 Same shell
    // as every other launcher screen now: an app bar with the title, a
    // scrolling column of UiKit cards, one accent action.
    private void buildUi() {
        LinearLayout shell = new LinearLayout(this);
        shell.setOrientation(LinearLayout.VERTICAL);
        shell.setBackgroundColor(UiKit.color(this, R.color.gzh_background));
        setContentView(shell);
        InsetUtil.applySafeInsets(shell);

        UiKit.appBar(shell, getString(R.string.online_subtitle),
            getString(R.string.online_window_title), 0, null, null);

        android.widget.FrameLayout host = new android.widget.FrameLayout(this);
        shell.addView(host, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        LinearLayout page = UiKit.scrollingPage(host);

        LinearLayout statusCard = UiKit.card(page);
        UiKit.sectionHeader(statusCard, R.drawable.ic_gzh_account,
            getString(R.string.online_window_title), false);
        statusText = UiKit.body(statusCard, null);
        statusText.setTextIsSelectable(true);
        signOutButton = UiKit.button(statusCard, UiKit.BTN_DANGER, R.drawable.ic_gzh_trash,
            getString(R.string.online_button_sign_out), this::onSignOut);

        LinearLayout stepsCard = UiKit.card(page);
        UiKit.sectionHeader(stepsCard, R.drawable.ic_gzh_check,
            getString(R.string.online_card_sign_in), false);
        UiKit.supporting(stepsCard, getString(R.string.online_signin_help));
        signInButton = UiKit.button(stepsCard, UiKit.BTN_PRIMARY, R.drawable.ic_gzh_account,
            getString(R.string.online_button_sign_in), this::onSignIn);

        // GeneralsX @feature Android port 13/09/2026 The online data comes before
        // the cross-play toggle because it gates it: without the community patch
        // the INI checksum cannot match a PC lobby no matter what the toggle
        // claims for the EXE, so the thing that fixes that should be the one a
        // player reaches first.
        buildDataPackCard(page);

        // GeneralsX @feature Android port 13/09/2026 Cross-play toggle, here beside
        // Sign In rather than among the gx_* diagnostics it is implemented as.
        // Whether this device can play against a PC belongs with the account screen
        // -- that is where someone goes when they want to play online at all -- and
        // filed under diagnostics it read as one more trace switch for developers.
        buildCrossPlayCard(page);
    }

    // GeneralsX @feature Android port 13/09/2026 Maps and the community data
    // patch, fetched straight from GeneralsOnline's own published package, so
    // that getting online needs nothing but this device. What the PC does with
    // an installer, this does with the ZIP the same release is published as --
    // see DataPackInstaller for why those two directories and nothing else.
    private void buildDataPackCard(LinearLayout page) {
        LinearLayout card = UiKit.card(page);
        UiKit.sectionHeader(card, R.drawable.ic_gzh_download,
            getString(R.string.online_card_datapacks), false);
        UiKit.supporting(card, getString(R.string.online_datapacks_help));

        dataPackChip = UiKit.chip(card, R.drawable.ic_gzh_info, "",
            R.color.gzh_status_warn, R.color.gzh_surface_container_high);

        dataPackStatus = UiKit.body(card, null);
        dataPackStatus.setTextIsSelectable(true);

        dataPackButton = UiKit.button(card, UiKit.BTN_PRIMARY, R.drawable.ic_gzh_download,
            getString(R.string.online_button_datapacks_update), this::onUpdateDataPacks);
        dataPackDeleteButton = UiKit.button(card, UiKit.BTN_DANGER, R.drawable.ic_gzh_trash,
            getString(R.string.online_button_datapacks_delete), this::onDeleteDataPacks);

        // Off is a legitimate choice, and it should not mean deleting a 30MB
        // download: the PC client has the same switch. It only reaches the
        // engine on the next launch, which the description says.
        dataPackSwitch = UiKit.switchRow(card,
            getString(R.string.online_switch_datapacks),
            getString(R.string.online_switch_datapacks_desc));
        dataPackSwitch.setChecked(DataPackInstaller.isEnabled());
        dataPackSwitch.setOnCheckedChangeListener((button, checked) -> {
            if (!DataPackInstaller.setEnabled(this, checked)) {
                Toast.makeText(this, R.string.online_datapacks_switch_failed,
                    Toast.LENGTH_LONG).show();
                button.setChecked(!checked);
            }
        });

        refreshDataPackCard();
    }

    /**
     * One place decides what the card says, because three things feed it:
     * whether the account is signed in, whether the data is installed, and
     * whether it is switched on.
     */
    private void refreshDataPackCard() {
        if (dataPackStatus == null) {
            return;
        }

        final boolean signedIn = getSignedInDisplayName(this) != null;
        final String version = DataPackInstaller.installedVersion(this);
        final boolean installed = version != null && !version.isEmpty();

        dataPackStatus.setText(installed
            ? getString(R.string.online_datapacks_installed, version)
            : getString(R.string.online_datapacks_not_installed));

        // Downloading before sign-in would be allowed by the CDN, but it would
        // also be the wrong order to learn this in: the data exists to make an
        // account's games joinable, so the account comes first and the card
        // says so rather than failing quietly later.
        dataPackButton.setEnabled(signedIn && !dataPackBusy);
        dataPackDeleteButton.setEnabled(installed && !dataPackBusy);
        dataPackSwitch.setEnabled(installed);
        dataPackSwitch.setChecked(DataPackInstaller.isEnabled());

        if (!signedIn) {
            setChip(dataPackChip, R.drawable.ic_gzh_info,
                R.string.online_datapacks_chip_sign_in_first, R.color.gzh_status_warn);
        } else if (!installed) {
            setChip(dataPackChip, R.drawable.ic_gzh_info,
                R.string.online_datapacks_chip_required, R.color.gzh_status_warn);
        } else if (!DataPackInstaller.isEnabled()) {
            setChip(dataPackChip, R.drawable.ic_gzh_info,
                R.string.online_datapacks_chip_off, R.color.gzh_status_warn);
        } else {
            setChip(dataPackChip, R.drawable.ic_gzh_check,
                R.string.online_datapacks_chip_ready, R.color.gzh_status_ok);
        }

        if (crossPlayPatchChip != null) {
            final boolean havePatch = DataPackInstaller.communityPatchFile().isFile();
            setChip(crossPlayPatchChip,
                havePatch ? R.drawable.ic_gzh_check : R.drawable.ic_gzh_info,
                havePatch ? R.string.online_crossplay_patch_found
                          : R.string.online_crossplay_patch_missing,
                havePatch ? R.color.gzh_status_ok : R.color.gzh_status_warn);
        }
    }

    // UiKit.chip() builds one; this restyles it afterwards, which the card
    // needs because its state changes without the screen being rebuilt.
    private void setChip(TextView chip, int iconRes, int labelRes, int colorRes) {
        if (chip == null) {
            return;
        }
        int tint = androidx.core.content.ContextCompat.getColor(this, colorRes);
        chip.setText(labelRes);
        chip.setTextColor(tint);
        android.graphics.drawable.Drawable icon =
            androidx.core.content.ContextCompat.getDrawable(this, iconRes);
        if (icon != null) {
            int size = Math.round(15 * getResources().getDisplayMetrics().density);
            icon.setBounds(0, 0, size, size);
            icon.setTint(tint);
            chip.setCompoundDrawablesRelative(icon, null, null, null);
        }
    }

    /**
     * Asks once per sign-in, and only when there is nothing installed. The
     * card already states it permanently; this is for the case the card is
     * below the fold on a phone, which is most of them.
     */
    private void maybePromptForDataPacks() {
        if (dataPackPrompted || dataPackBusy) {
            return;
        }
        if (getSignedInDisplayName(this) == null) {
            return;
        }
        if (DataPackInstaller.installedVersion(this) != null) {
            return;
        }
        dataPackPrompted = true;
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.online_card_datapacks)
            .setMessage(R.string.online_datapacks_prompt)
            .setPositiveButton(R.string.online_button_datapacks_update,
                (dialog, which) -> onUpdateDataPacks())
            .setNegativeButton(R.string.online_datapacks_prompt_later, null)
            .show();
    }

    private void onDeleteDataPacks() {
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.online_button_datapacks_delete)
            .setMessage(R.string.online_datapacks_delete_confirm)
            .setPositiveButton(R.string.online_button_datapacks_delete, (dialog, which) -> {
                int removed = DataPackInstaller.uninstall(this);
                if (removed < 0) {
                    Toast.makeText(this, R.string.online_datapacks_delete_no_record,
                        Toast.LENGTH_LONG).show();
                } else {
                    Toast.makeText(this,
                        getString(R.string.online_datapacks_deleted, removed),
                        Toast.LENGTH_LONG).show();
                }
                // Asking again is right after a deliberate delete only if the
                // player signs in afresh, so leave dataPackPrompted set.
                refreshDataPackCard();
            })
            .setNegativeButton(R.string.common_cancel, null)
            .show();
    }

    private void onUpdateDataPacks() {
        if (dataPackBusy) {
            return;
        }
        dataPackBusy = true;
        dataPackButton.setEnabled(false);
        dataPackDeleteButton.setEnabled(false);

        new Thread(() -> {
            DataPackInstaller.Result result = DataPackInstaller.install(this,
                new DataPackInstaller.Progress() {
                    @Override public void onChecking() {
                        handler.post(() ->
                            dataPackStatus.setText(R.string.online_datapacks_checking));
                    }

                    @Override public void onDownloading(long bytes, long total) {
                        final int percent = total > 0 ? (int) (bytes * 100 / total) : 0;
                        handler.post(() -> dataPackStatus.setText(
                            getString(R.string.online_datapacks_downloading, percent)));
                    }

                    @Override public void onInstalling() {
                        handler.post(() ->
                            dataPackStatus.setText(R.string.online_datapacks_installing));
                    }
                });

            handler.post(() -> {
                dataPackBusy = false;
                // refreshDataPackCard() re-reads the installed state and fixes
                // the cross-play chip too; the result line is written after it
                // so a finished install still says what it did.
                refreshDataPackCard();
                dataPackStatus.setText(result.ok
                    ? getString(R.string.online_datapacks_done,
                        result.version, result.filesWritten)
                    : getString(R.string.online_datapacks_failed, result.error));
            });
        }).start();
    }

    // The engine reads this as a marker file in the game folder (GlobalData::init);
    // the switch just creates or deletes it. Same convention as the gx_* markers,
    // which is why it needs the game folder and says so when there is not one.
    private void buildCrossPlayCard(LinearLayout page) {
        LinearLayout card = UiKit.card(page);
        UiKit.sectionHeader(card, R.drawable.ic_gzh_globe,
            getString(R.string.online_card_crossplay), false);
        UiKit.supporting(card, getString(R.string.online_crossplay_help));

        // GeneralsX @feature Android port 13/09/2026 The EXE checksum is only half
        // of what a PC-hosted game checks; the other half is the INI checksum, and
        // that one this device can genuinely match rather than claim. The PC client
        // mounts a community data patch it downloads into its user-data folder, so a
        // retail-only install computes a different number and is turned away no
        // matter what it reports for the EXE. Nothing here can fetch that file, so
        // say plainly whether it is present -- before the game-folder check below,
        // because the patch lives in the user-data folder either way.
        final boolean havePatch = DataPackInstaller.communityPatchFile().isFile();
        crossPlayPatchChip = UiKit.chip(card,
            havePatch ? R.drawable.ic_gzh_check : R.drawable.ic_gzh_info,
            getString(havePatch
                ? R.string.online_crossplay_patch_found
                : R.string.online_crossplay_patch_missing),
            havePatch ? R.color.gzh_status_ok : R.color.gzh_status_warn,
            R.color.gzh_surface_container_high);

        final File marker = crossPlayMarkerFile();
        if (marker == null) {
            UiKit.chip(card, R.drawable.ic_gzh_info,
                getString(R.string.setup_diagnostics_no_folder),
                R.color.gzh_status_warn, R.color.gzh_surface_container_high);
            return;
        }

        com.google.android.material.materialswitch.MaterialSwitch sw = UiKit.switchRow(card,
            getString(R.string.online_switch_crossplay),
            getString(R.string.online_switch_crossplay_desc));
        sw.setChecked(marker.isFile());
        sw.setOnCheckedChangeListener((button, checked) -> {
            if (checked) {
                try {
                    marker.createNewFile();
                } catch (java.io.IOException e) {
                    Toast.makeText(this,
                        getString(R.string.setup_toast_options_save_failed, e.getMessage()),
                        Toast.LENGTH_LONG).show();
                    button.setChecked(false);
                    return;
                }
            } else {
                marker.delete();
            }
        });
    }

    private File crossPlayMarkerFile() {
        String gamePath = SetupActivity.getSavedGamePath(this);
        return gamePath != null ? new File(gamePath, "gx_pc_compat.txt") : null;
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
        statusText.setText(R.string.online_status_signing_in);
        new Thread(() -> {
            GeneralsOnlineSession.AuthResult result = callLoginWithToken(refreshToken);
            handler.post(() -> {
                busy = false;
                signInButton.setEnabled(true);
                if (result != null && result.state == 1) {
                    saveSession(result);
                    refreshStatus();
                } else if (result != null && result.state == 2) {
                    // Server explicitly says the refresh token is dead --
                    // fall back to a fresh browser sign-in next time the
                    // user taps Sign In.
                    clearSession();
                    refreshStatus();
                } else {
                    // GeneralsX @bugfix Android port 08/30/2026 result==null
                    // (or an unexpected state) means the request never got a
                    // real answer -- network error or a blocked/rejected
                    // request (see GeneralsOnlineSession.postJson). That is
                    // NOT the same as "this refresh token is invalid", so
                    // don't clearSession() here: a transient connectivity
                    // problem used to silently wipe a perfectly good cached
                    // session, forcing the full browser flow again on next
                    // launch even though nothing was actually wrong with the
                    // account. Leave the cached session alone; the game (or
                    // a later launch) will just try refreshing again.
                    refreshStatus();
                    if (result == null) {
                        statusText.setText(withNetworkErrorDetail(getString(R.string.online_status_network_error)));
                    }
                }
            });
        }).start();
    }

    // GeneralsX @bugfix Android port 10/07/2026 the sign-in flow backgrounds
    // this Activity for the whole browser round-trip; without a battery
    // exemption, some OEM battery managers throttle the poll timer hard
    // enough that CheckLogin never actually runs, so a login that visibly
    // succeeded on the website never completes here. First tap requests the
    // exemption (and stops there -- no browser yet); once granted (or if it
    // already was), the next tap proceeds with the real sign-in.
    private boolean ensureNotBatteryOptimized() {
        android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null && pm.isIgnoringBatteryOptimizations(getPackageName())) {
            return true;
        }
        try {
            Intent intent = new Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        } catch (Exception e) {
            // Some OEMs don't support this action; fall through and let sign-in
            // proceed anyway rather than blocking the user entirely.
            return true;
        }
        statusText.setText(R.string.online_status_battery_opt);
        return false;
    }

    private void onSignIn() {
        if (busy) {
            return;
        }
        if (!ensureNotBatteryOptimized()) {
            return;
        }
        busy = true;
        pollAttempt = 0;
        final int generation = ++signInGeneration;
        signInButton.setEnabled(false);

        // GeneralsX @bugfix Android port 13/09/2026 The code now comes from
        // the server instead of being invented here -- see
        // GeneralsOnlineSession.fetchLoginCode. That means a network round
        // trip before the browser can open, so the tap no longer opens it
        // directly.
        NetworkTrace.section(this, "sign-in attempt");
        statusText.setText(R.string.online_status_requesting_code);
        new Thread(() -> {
            String code = GeneralsOnlineSession.fetchLoginCode(this);
            handler.post(() -> onLoginCodeReady(code, generation));
        }, "GeneralsOnlineLoginCode").start();
    }

    private void onLoginCodeReady(String code, int generation) {
        if (generation != signInGeneration) {
            return;
        }
        if (code == null) {
            busy = false;
            signInButton.setEnabled(true);
            statusText.setText(withNetworkErrorDetail(getString(R.string.online_status_no_login_code)));
            return;
        }

        String url = String.format(LOGIN_URL_FMT, code);
        NetworkTrace.write(this, "server issued a login code (" + code.length()
            + " chars); opening browser");
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            busy = false;
            signInButton.setEnabled(true);
            Toast.makeText(this, getString(R.string.online_toast_no_browser, e.getMessage()), Toast.LENGTH_LONG).show();
            return;
        }

        statusText.setText(R.string.online_status_continue_browser);
        handler.postDelayed(() -> pollOnce(code, generation), POLL_INTERVAL_MS);
    }

    private void pollOnce(String code, int generation) {
        if (generation != signInGeneration) {
            return;
        }
        new Thread(() -> {
            GeneralsOnlineSession.AuthResult result = callCheckLogin(code);
            handler.post(() -> handlePollResult(code, generation, result));
        }).start();
    }

    // GeneralsX @bugfix Android port 13/09/2026 Rewritten around one fact:
    // until the user finishes on the website, the server's answer to every
    // poll is indistinguishable from a rejection. An unclaimed code comes
    // back as HTTP 403 with result:2 (FAILED) -- the same pair a genuinely
    // refused sign-in produces. The old loop treated both as terminal and
    // gave up on the first tick, one second after opening the browser and
    // long before anyone could have typed a password.
    //
    // So a FAILED is no longer terminal on its own; it is only terminal
    // once the whole window has elapsed. Nothing is lost by waiting: a real
    // refusal just means the user sees "timed out" three minutes later
    // instead of "sign-in failed" immediately, and the log records every
    // answer in between. What IS terminal: an explicit ban (423), and a
    // transport failure with no parseable body at all.
    private void handlePollResult(String code, int generation,
                                  GeneralsOnlineSession.AuthResult result) {
        if (generation != signInGeneration) {
            // A newer attempt has taken over; this answer is about a code
            // nobody is waiting on any more.
            return;
        }
        if (result == null) {
            busy = false;
            signInButton.setEnabled(true);
            statusText.setText(withNetworkErrorDetail(getString(R.string.online_status_network_error)));
            NetworkTrace.write(this, "sign-in aborted: no usable response from either endpoint");
            return;
        }

        if (result.httpStatus == 423) {
            busy = false;
            signInButton.setEnabled(true);
            String reason = result.banReason == null || result.banReason.isEmpty()
                ? getString(R.string.online_status_banned)
                : getString(R.string.online_status_banned_reason, result.banReason);
            statusText.setText(reason);
            NetworkTrace.write(this, "sign-in refused: account banned");
            return;
        }

        if (result.state == 1) { // SUCCEEDED
            busy = false;
            signInButton.setEnabled(true);
            saveSession(result);
            refreshStatus();
            NetworkTrace.write(this, "sign-in complete after " + pollAttempt
                + " polls, user " + result.userId);
            Toast.makeText(this, getString(R.string.online_toast_signed_in_as, result.displayName),
                Toast.LENGTH_LONG).show();
            return;
        }

        // Everything else -- WAITING_USER_ACTION, CODE_INVALID, and the
        // FAILED that an unclaimed code produces -- means "not yet".
        ++pollAttempt;
        if (pollAttempt >= POLL_MAX_ATTEMPTS) {
            busy = false;
            signInButton.setEnabled(true);
            // After a full window of nothing but refusals, the likeliest
            // cause is that the sign-in was never completed in the browser,
            // so say that rather than blaming the network.
            statusText.setText(withNetworkErrorDetail(getString(R.string.online_status_timed_out)));
            NetworkTrace.write(this, "sign-in timed out after " + pollAttempt
                + " polls; last status HTTP " + result.httpStatus
                + ", result " + result.state);
            return;
        }

        if (pollAttempt == 1 || pollAttempt % 15 == 0) {
            // One line a poll would be 180 lines of the same thing; this is
            // enough to see the loop is alive and what it is being told.
            NetworkTrace.write(this, "poll " + pollAttempt + ": HTTP " + result.httpStatus
                + ", result " + result.state + " (still waiting)");
        }
        handler.postDelayed(() -> pollOnce(code, generation), POLL_INTERVAL_MS);
    }

    // GeneralsX @bugfix Android port 08/30/2026 A user reported the network-
    // error screen with no way to see WHY it failed (no adb/logcat access).
    // GeneralsOnlineSession.lastNetworkErrorDetail now captures the actual
    // host + HTTP status/body snippet (or exception) from the failed
    // request -- surface it right on screen instead of just the generic
    // string. statusText already has setTextIsSelectable(true), so this is
    // also copyable to paste into a bug report.
    private String withNetworkErrorDetail(String baseMessage) {
        String detail = GeneralsOnlineSession.lastNetworkErrorDetail;
        if (detail == null || detail.isEmpty()) {
            return baseMessage;
        }
        return baseMessage + "\n\n" + detail;
    }

    // Runs on a background thread.
    //
    // GeneralsX @bugfix Android port 13/09/2026 reserved_0/1/2 retired in
    // favour of machine_guid/mac_addr/vol_serial, matching the current
    // upstream client (OnlineServices_Auth.cpp). exe_crc/ini_crc are the
    // engine's own checksums, which the launcher process cannot compute --
    // it never loads the game -- so they go as 0 and the game sends the
    // real ones on its own calls.
    private GeneralsOnlineSession.AuthResult callCheckLogin(String code) {
        JSONObject body = new JSONObject();
        try {
            body.put("code", code);
            body.put("client_id", CLIENT_ID);
            body.put("machine_guid", NetworkDiagnostics.installId(this));
            body.put("mac_addr", NetworkDiagnostics.syntheticMac(this));
            body.put("vol_serial", NetworkDiagnostics.syntheticVolumeSerial(this));
            body.put("exe_crc", 0);
            body.put("ini_crc", 0);
        } catch (Exception e) {
            return null;
        }
        return GeneralsOnlineSession.postJson(this, "CheckLogin", body, null);
    }

    // Runs on a background thread.
    private GeneralsOnlineSession.AuthResult callLoginWithToken(String refreshToken) {
        return GeneralsOnlineSession.loginWithToken(this, refreshToken);
    }

    private void saveSession(GeneralsOnlineSession.AuthResult result) {
        GeneralsOnlineSession.saveSession(this, result);
    }

    private void clearSession() {
        GeneralsOnlineSession.clearSession(this);
    }

    private void onSignOut() {
        // TODO: also DELETE /User/{user_id} server-side like the reference
        // client's LogoutOfMyAccount(), once we're sure "sign out" here
        // should mean "forget this device" rather than just "clear local
        // session" -- left local-only for now since that's the safer default.
        clearSession();
        refreshStatus();
        Toast.makeText(this, R.string.online_toast_signed_out, Toast.LENGTH_SHORT).show();
    }

    private void refreshStatus() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String displayName = prefs.getString(PREF_DISPLAY_NAME, null);
        String sessionToken = prefs.getString(PREF_SESSION_TOKEN, null);

        if (displayName != null && sessionToken != null && !sessionToken.isEmpty()) {
            statusText.setText(getString(R.string.online_status_signed_in_as, displayName));
            signOutButton.setEnabled(true);
        } else {
            statusText.setText(R.string.online_status_not_signed_in);
            signOutButton.setEnabled(false);
        }

        refreshDataPackCard();
        maybePromptForDataPacks();
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

}
