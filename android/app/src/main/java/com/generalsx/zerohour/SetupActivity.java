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

// GeneralsX @build Android port 07/07/2026
//
// A standalone launcher icon ("GeneralsZH Setup"), separate from the game
// itself, so configuring the game folder or reading a crash log never
// depends on the game having launched successfully first — and never
// requires adb. This is the practical answer to "there's no launcher": a
// full mod-manager-style launcher (à la GenLauncher) is future scope, but
// picking where the game lives and seeing why it crashed are needed on
// every single install, so they live here now.

package com.generalsx.zerohour;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.style.StyleSpan;
import android.text.style.UnderlineSpan;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.widget.SwitchCompat;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.slider.Slider;

import java.io.File;

public class SetupActivity extends Activity {

    static final String PREFS_NAME = "generalszh_setup";
    static final String PREF_GAME_PATH = "game_path";

    // TheSuperHackers @bugfix Android port 07/07/2026 SharedPreferences and
    // getFilesDir() both live under /data/data/<pkg>/ and are wiped the
    // moment the app is uninstalled -- which is exactly what a sideloaded
    // APK update often requires if the installer treats it as a fresh
    // install rather than an in-place update. Mirror the chosen path into a
    // small marker file on shared external storage (survives uninstall,
    // since it's outside the app's private/package-scoped directories) so a
    // fresh install can recover it automatically instead of re-prompting.
    private static final String EXTERNAL_MARKER_NAME = ".generalszh_gamepath.txt";

    // Marker files SDL3Main.cpp / GeneralsZHActivity check for on launch —
    // must match GameEngine/CMake's GeneralsMD/Code/Main/SDL3Main.cpp exactly.
    private static final String[] REQUIRED_GAME_FILES = { "INIZH.big", "INI.big" };

    private TextView statusText;

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // GeneralsX @bugfix Android port 31/07/2026 No longer forced to
        // landscape here -- see the matching AndroidManifest.xml comment.
        // This screen now starts portrait-first like every other non-game
        // screen; onLaunchGame()/onConfigurationChanged() below handle the
        // Setup -> Launch rotation race that used to be sidestepped by never
        // rotating Setup at all.
        super.onCreate(savedInstanceState);
        setTitle(R.string.setup_window_title);

        // GeneralsX @bugfix Android port 08/07/2026 This screen is the ONLY
        // way to reach "View Logs" without adb, so it must never be the thing
        // that crashes. Any future Material/theme incompatibility falls back
        // to a bare-bones plain-widget UI (same actions, no styling) instead
        // of taking the whole Settings app down with it.
        try {
            buildUi();
        } catch (Throwable t) {
            buildFallbackUi(t);
        }
    }

    private void buildFallbackUi(Throwable failure) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        root.setPadding(pad, pad, pad, pad);
        setContentView(root);
        InsetUtil.applySafeInsets(root);

        TextView warning = new TextView(this);
        warning.setText(getString(R.string.setup_fallback_warning, String.valueOf(failure)));
        warning.setPadding(0, 0, 0, dp(16));
        root.addView(warning);

        statusText = new TextView(this);
        statusText.setTextIsSelectable(true);
        statusText.setPadding(0, 0, 0, dp(24));
        root.addView(statusText);

        addButton(root, getString(R.string.setup_button_select_game_folder), this::onSelectGameFolder);
        addButton(root, getString(R.string.setup_button_view_logs), this::onViewLogs);
        addButton(root, getString(R.string.setup_button_launch_game), this::onLaunchGame);
        addButton(root, getString(R.string.setup_button_clear_game_folder), this::onClearGameFolder);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // GeneralsX @bugfix Android port 31/07/2026 onLaunchGame() forces this
        // Activity to landscape right before starting the game (see its
        // comment) so the rotation settles before GeneralsZHActivity's native
        // window-size probe runs. That request otherwise sticks on this
        // Activity instance indefinitely, so coming back here (Back from the
        // game, or from any child screen) left Setup stuck landscape instead
        // of returning to its normal portrait-first state. Reset it every
        // time this screen comes back to the foreground; onLaunchGame()
        // re-applies the landscape lock itself the next time it's needed.
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        refreshStatus();
        refreshGeneralsOnlineStatus();
        loadDxvkConfigIntoEditor();
        refreshDiagnosticsSwitches();
    }

    // GeneralsX @feature Android port 08/07/2026 Material redesign: each
    // logical section lives in its own MaterialCardView instead of a flat
    // wall of buttons/text on the raw window background.
    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);
        setContentView(scroll);
        InsetUtil.applySafeInsets(scroll);

        TextView title = new TextView(this);
        title.setText(R.string.setup_title);
        title.setTextSize(22);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        title.setPadding(dp(4), dp(8), dp(4), dp(4));
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText(R.string.setup_subtitle);
        subtitle.setTextSize(14);
        subtitle.setAlpha(0.7f);
        subtitle.setPadding(dp(4), 0, dp(4), dp(16));
        root.addView(subtitle);

        LinearLayout statusCard = startCard(root, null);
        statusText = new TextView(this);
        statusText.setTextIsSelectable(true);
        statusCard.addView(statusText);

        LinearLayout actionsCard = startCard(root, getString(R.string.setup_card_game_folder));
        addButton(actionsCard, getString(R.string.setup_button_select_game_folder), this::onSelectGameFolder);
        addButton(actionsCard, getString(R.string.setup_button_launch_game), this::onLaunchGame);
        addButton(actionsCard, getString(R.string.setup_button_view_logs), this::onViewLogs);
        addButton(actionsCard, getString(R.string.setup_button_clear_game_folder), this::onClearGameFolder);

        // GeneralsX @bugfix Android port 01/08/2026 moved up from below the
        // language/UI-scale/driver/dxvk-config cards -- signing into
        // GeneralsOnline is a primary action most people want right after
        // picking their game folder, not buried under advanced settings most
        // players never touch.
        buildGeneralsOnlineSection(root);

        buildLanguageSection(root);
        buildUiScaleSection(root);
        buildRenderBackendSection(root);
        // Custom Vulkan driver / dxvk.conf only matter when Vulkan is the
        // selected backend -- the GLES/GLES+ANGLE paths never touch DXVK at
        // all, see Core/Libraries/Source/d3d8gles/CMakeLists.txt.
        if (RENDER_BACKEND_VULKAN.equals(getRenderBackendChoice())) {
            applyRecommendedDriverIfNeeded();
            buildCustomDriverSection(root);
            buildDxvkConfigSection(root);
        }
        buildDiagnosticsSection(root);

        LinearLayout helpCard = startCard(root, getString(R.string.setup_card_how_it_works));
        TextView help = new TextView(this);
        help.setText(R.string.setup_how_it_works_body);
        helpCard.addView(help);
    }

    // GeneralsX @feature Android port 13/07/2026 GitHub issue #4: in-app
    // language override for this launcher (Setup/Log Viewer/folder browser)
    // -- see LocaleHelper for why it's a manual attachBaseContext() wrap
    // rather than androidx.appcompat's per-app language API, and why
    // "System Default" needs no explicit handling.
    private void buildLanguageSection(LinearLayout root) {
        LinearLayout content = startCard(root, getString(R.string.setup_card_language));

        TextView status = new TextView(this);
        status.setText(getString(R.string.setup_language_status,
            LocaleHelper.displayNameFor(this, LocaleHelper.getSavedLanguageTag(this))));
        status.setPadding(0, 0, 0, dp(8));
        content.addView(status);

        addButton(content, getString(R.string.setup_button_change_language), this::onChangeLanguage);

        gameLanguageStatusView = new TextView(this);
        gameLanguageStatusView.setPadding(0, dp(8), 0, 0);
        updateGameLanguageStatusView();
        content.addView(gameLanguageStatusView);

        TextView help = new TextView(this);
        help.setAlpha(0.8f);
        help.setText(R.string.setup_language_help);
        content.addView(help);
    }

    // GeneralsX @feature Android port 13/07/2026 GitHub issue #4 follow-up:
    // status line showing whether the launcher's chosen language is also
    // being applied to the game's own text (see applyGameLanguageOverride()).
    private TextView gameLanguageStatusView;

    private void updateGameLanguageStatusView() {
        if (gameLanguageStatusView == null) {
            return;
        }
        String launcherTag = LocaleHelper.getSavedLanguageTag(this);
        String engineToken = LocaleHelper.gameDataLanguageFor(launcherTag);
        if (engineToken == null) {
            gameLanguageStatusView.setText(R.string.setup_language_game_status_default);
            return;
        }
        String gamePath = getSavedGamePath();
        boolean found = gamePath != null && new File(gamePath, "data/" + engineToken + "/generals.csf").isFile();
        gameLanguageStatusView.setText(getString(
            found ? R.string.setup_language_game_status_override : R.string.setup_language_game_status_missing,
            engineToken));
    }

    private void onChangeLanguage() {
        String[] tags = LocaleHelper.SUPPORTED_TAGS;
        String[] labels = new String[tags.length];
        for (int i = 0; i < tags.length; i++) {
            labels[i] = LocaleHelper.displayNameFor(this, tags[i]);
        }
        String currentTag = LocaleHelper.getSavedLanguageTag(this);
        int currentIndex = 0;
        for (int i = 0; i < tags.length; i++) {
            if (tags[i].equals(currentTag)) {
                currentIndex = i;
                break;
            }
        }
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.setup_language_dialog_title)
            .setSingleChoiceItems(labels, currentIndex, (dialog, which) -> {
                LocaleHelper.setSavedLanguageTag(this, tags[which]);
                applyGameLanguageOverride(tags[which]);
                dialog.dismiss();
                recreate();
            })
            .setNegativeButton(R.string.common_cancel, null)
            .show();
    }

    // GeneralsX @feature Android port 13/07/2026 GitHub issue #4 follow-up:
    // if the launcher's chosen UI language has a corresponding engine
    // language-folder token (LocaleHelper.gameDataLanguageFor()) AND the
    // user's own selected game folder actually has that data
    // (data/<token>/generals.csf -- lowercase "data", matching g_csfFile
    // in SDL3Main.cpp and the Linux case-sensitivity fix it documents), we
    // write <filesDir>/game_language.cfg so SDL3Main.cpp exports
    // CNC_ZH_LANGUAGE before any engine subsystem reads it
    // (GetRegistryLanguage() in registry.cpp checks that env var first,
    // ahead of registry.ini and the BIG-file auto-detect). If the data
    // isn't present -- most commonly Russian, which Zero Hour never
    // shipped officially and only exists via the user's own fan/licensed
    // copy -- we deliberately leave the game on its own default/auto-detect
    // rather than force a language with no text to show
    // (GameTextManager::init() in GameText.cpp fails gracefully, blank UI
    // text, if the CSF is missing -- silently wrong is worse than
    // untouched). Called both when the language changes and when the game
    // folder changes, since either one can flip the answer.
    private void applyGameLanguageOverride(String launcherTag) {
        File marker = new File(getFilesDir(), "game_language.cfg");
        String engineToken = LocaleHelper.gameDataLanguageFor(launcherTag);
        if (engineToken == null) {
            marker.delete();
            return;
        }
        String gamePath = getSavedGamePath();
        if (gamePath == null) {
            marker.delete();
            return;
        }
        File csf = new File(gamePath, "data/" + engineToken + "/generals.csf");
        if (!csf.isFile()) {
            marker.delete();
            return;
        }
        try (java.io.FileWriter w = new java.io.FileWriter(marker, false)) {
            w.write(engineToken);
            w.write("\n");
        } catch (java.io.IOException e) {
            // Not fatal: worst case the game just keeps its own default
            // language, same as before this feature existed.
        }
    }

    // Creates a MaterialCardView appended to `root`, with an optional bold
    // header line, and returns its inner vertical content LinearLayout so
    // callers can just addView() into it like before.
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

    // TheSuperHackers @feature Android port 07/07/2026 Menu text size scaling
    // (GlobalLanguage::adjustFontSize()) has no in-game slider yet -- the
    // Options screen lives in the user's own game data (.wnd layout), not in
    // this engine source tree, so a real in-game control can't be added from
    // here. Expose the same "ResolutionFontAdjustment" percentage here
    // instead, writing straight into the Options.ini this Android build
    // actually reads (see SDL3Main.cpp: HOME=<internal storage>, so the file
    // is <filesDir>/.local/share/GeneralsX/GeneralsZH/Options.ini) -- no need
    // to wait for the game to visit its own Options menu first.
    private Slider uiScaleSlider;
    private TextView uiScaleLabel;

    // GeneralsX @bugfix Android port 08/07/2026 A prior "Interface Size (whole
    // UI)" option here rendered at a reduced internal resolution and let the
    // pillarbox blit upscale to the full panel, expecting buttons/controlbar
    // to grow along with text. On real devices only text actually changed
    // size — widget layout geometry is itself computed as a ratio of the
    // current resolution, so shrinking that resolution and then stretching
    // the result back up is a no-op for anything but text (whose *requested
    // font point size* is a genuinely bigger asset, not just a stretched
    // rect). Removed rather than ship a slider that visibly does nothing;
    // "Menu Text Size" below is the one scaling option that actually works.

    private void buildUiScaleSection(LinearLayout root) {
        LinearLayout content = startCard(root, getString(R.string.setup_card_text_size));

        uiScaleLabel = new TextView(this);
        content.addView(uiScaleLabel);

        int startPercent = readUiScalePercent();
        uiScaleSlider = new Slider(this);
        uiScaleSlider.setValueFrom(0f);
        uiScaleSlider.setValueTo(150f);
        uiScaleSlider.setStepSize(1f);
        uiScaleSlider.setValue(startPercent);
        updateUiScaleLabel(startPercent);
        uiScaleSlider.addOnChangeListener((slider, value, fromUser) -> updateUiScaleLabel((int) value));
        content.addView(uiScaleSlider);

        addButton(content, getString(R.string.setup_button_apply_text_size), () -> {
            writeUiScalePercent((int) uiScaleSlider.getValue());
            Toast.makeText(this, R.string.setup_toast_text_size_saved, Toast.LENGTH_LONG).show();
        });

        TextView uiScaleHelp = new TextView(this);
        uiScaleHelp.setAlpha(0.8f);
        uiScaleHelp.setText(R.string.setup_text_size_help);
        content.addView(uiScaleHelp);
    }

    private void updateUiScaleLabel(int percent) {
        uiScaleLabel.setText(getString(R.string.setup_text_size_label, percent));
    }

    // GeneralsX @feature Android port render-backend picker 07/09/2026 -
    // three render backends this branch supports, replacing the previous
    // adb-only GENERALSX_RENDER_BACKEND/GENERALSX_GLES_ANGLE env vars with a
    // real in-app choice (most users have no adb at all). Read/written the
    // same way as CUSTOM_DRIVER_CFG_NAME above: a plain-text marker file in
    // getFilesDir(), one line, read natively by SDL3Main.cpp's
    // UseVulkanBackend()/UseANGLE() (and duplicated in gles_pipeline.cpp's
    // initContext() -- see its comment for why) before the game's video
    // subsystem starts.
    private static final String RENDER_BACKEND_CFG_NAME = "render_backend.cfg";
    private static final String RENDER_BACKEND_VULKAN = "vulkan";
    private static final String RENDER_BACKEND_GLES = "gles";
    private static final String RENDER_BACKEND_GLES_ANGLE = "gles_angle";

    private TextView renderBackendStatusView;

    // No config file yet (fresh install) means "whatever UseVulkanBackend()/
    // UseANGLE() default to today when their env vars are unset" -- GLES,
    // per SDL3Main.cpp's own comment on why this branch defaults there.
    // Deliberately NOT auto-detecting "the best backend for this device"
    // here: preserves today's actual behavior for existing installs instead
    // of silently switching anyone's renderer the next time Setup runs.
    private String getRenderBackendChoice() {
        File cfg = new File(getFilesDir(), RENDER_BACKEND_CFG_NAME);
        if (!cfg.isFile()) {
            return RENDER_BACKEND_GLES;
        }
        String value = readFirstLine(cfg);
        if (RENDER_BACKEND_VULKAN.equals(value) || RENDER_BACKEND_GLES_ANGLE.equals(value)) {
            return value;
        }
        return RENDER_BACKEND_GLES;
    }

    private String renderBackendLabel(String choice) {
        switch (choice) {
            case RENDER_BACKEND_VULKAN:
                return getString(R.string.setup_render_backend_vulkan);
            case RENDER_BACKEND_GLES_ANGLE:
                return getString(R.string.setup_render_backend_gles_angle);
            default:
                return getString(R.string.setup_render_backend_gles);
        }
    }

    private void buildRenderBackendSection(LinearLayout root) {
        LinearLayout content = startCard(root, getString(R.string.setup_card_render_backend));

        renderBackendStatusView = new TextView(this);
        renderBackendStatusView.setText(getString(R.string.setup_render_backend_status, renderBackendLabel(getRenderBackendChoice())));
        renderBackendStatusView.setPadding(0, 0, 0, dp(8));
        content.addView(renderBackendStatusView);

        addButton(content, getString(R.string.setup_button_change_render_backend), this::onChangeRenderBackend);

        TextView help = new TextView(this);
        help.setAlpha(0.8f);
        help.setText(R.string.setup_render_backend_help);
        content.addView(help);
    }

    private void onChangeRenderBackend() {
        String[] choices = { RENDER_BACKEND_VULKAN, RENDER_BACKEND_GLES, RENDER_BACKEND_GLES_ANGLE };
        String[] labels = new String[choices.length];
        for (int i = 0; i < choices.length; i++) {
            labels[i] = renderBackendLabel(choices[i]);
        }
        String current = getRenderBackendChoice();
        int currentIndex = 0;
        for (int i = 0; i < choices.length; i++) {
            if (choices[i].equals(current)) {
                currentIndex = i;
                break;
            }
        }
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.setup_render_backend_dialog_title)
            .setSingleChoiceItems(labels, currentIndex, (dialog, which) -> {
                File cfg = new File(getFilesDir(), RENDER_BACKEND_CFG_NAME);
                try (java.io.FileWriter w = new java.io.FileWriter(cfg, false)) {
                    w.write(choices[which]);
                    w.write("\n");
                } catch (java.io.IOException e) {
                    Toast.makeText(this, getString(R.string.setup_toast_render_backend_failed, e.getMessage()), Toast.LENGTH_LONG).show();
                    dialog.dismiss();
                    return;
                }
                dialog.dismiss();
                Toast.makeText(this, R.string.setup_toast_render_backend_saved, Toast.LENGTH_LONG).show();
                // The Custom Vulkan Driver / DXVK Config cards below are only
                // relevant for the Vulkan backend -- recreate() re-runs
                // buildUi() so they show/hide immediately, same pattern
                // onChangeLanguage() already uses for its own dialog.
                recreate();
            })
            .setNegativeButton(R.string.common_cancel, null)
            .show();
    }

    // GeneralsX @feature Android port 10/07/2026 Optional custom Vulkan
    // driver (e.g. Mesa Turnip for Adreno GPUs), loaded natively via
    // libadrenotools -- see TryLoadCustomVulkanDriver() in SDL3Main.cpp.
    // Package format matches the convention Winlator/AetherSX2/PPSSPP all
    // use: a .zip containing meta.json (schemaVersion/name/description/
    // author/packageVersion/vendor/driverVersion/minApi/libraryName) plus
    // the driver .so (and any dependency .so's) alongside it. We never
    // bundle a driver ourselves -- the user supplies one (e.g. from
    // K11MCH1/AdrenoToolsDrivers or The412Banner/Banners-Turnip on GitHub),
    // matching every other app that uses this technique.
    private static final String CUSTOM_DRIVER_DIR_NAME = "custom_driver";
    private static final String CUSTOM_DRIVER_CFG_NAME = "custom_driver.cfg";
    // GeneralsX @feature Android port 13/07/2026 Marks that custom_driver.cfg
    // was populated by applyRecommendedDriverIfNeeded() rather than by the
    // user importing their own .zip -- lets the status text and the
    // "reset" button distinguish "we picked this for you" from "you chose
    // this", without changing anything on the native loading side (both
    // cases are the same custom_driver.cfg/custom_driver/ that
    // TryLoadCustomVulkanDriver() in SDL3Main.cpp already reads).
    private static final String CUSTOM_DRIVER_AUTO_MARKER_NAME = "custom_driver.auto";
    // Bundled fallback driver (staged by scripts/build/android/fetch-turnip.sh
    // into this asset folder at build time) -- see applyRecommendedDriverIfNeeded().
    private static final String DEFAULT_DRIVER_ASSET_DIR = "default_driver";
    private static final int REQUEST_IMPORT_DRIVER = 1002;

    private TextView customDriverStatusView;

    private void buildCustomDriverSection(LinearLayout root) {
        LinearLayout content = startCard(root, getString(R.string.setup_card_vulkan_driver));

        customDriverStatusView = new TextView(this);
        customDriverStatusView.setText(customDriverStatusText());
        customDriverStatusView.setPadding(0, 0, 0, dp(8));
        content.addView(customDriverStatusView);

        addButton(content, getString(R.string.setup_button_import_driver), this::onImportCustomDriver);
        addButton(content, getString(R.string.setup_button_reset_driver), this::onClearCustomDriver);

        TextView help = new TextView(this);
        help.setAlpha(0.8f);
        help.setText(R.string.setup_driver_help);
        content.addView(help);
    }

    private String customDriverStatusText() {
        File cfg = new File(getFilesDir(), CUSTOM_DRIVER_CFG_NAME);
        if (!cfg.isFile()) {
            return getString(R.string.setup_driver_status_none);
        }
        String driverName = readFirstLine(cfg);
        boolean isAuto = new File(getFilesDir(), CUSTOM_DRIVER_AUTO_MARKER_NAME).isFile();
        String name = driverName != null ? driverName : getString(R.string.setup_driver_unknown);
        return getString(isAuto ? R.string.setup_driver_status_auto : R.string.setup_driver_status_manual, name);
    }

    // GeneralsX @feature Android port 13/07/2026 Auto-applies the bundled
    // Turnip driver on Adreno phones whose stock driver reports less than
    // Vulkan 1.3, without touching anything if the user already imported
    // their own driver or the phone doesn't need help. Called on every
    // Setup launch (cheap no-op once satisfied) and again from
    // onClearCustomDriver() so "reset" actually restores the recommended
    // state instead of just going blank.
    private void applyRecommendedDriverIfNeeded() {
        try {
            if (new File(getFilesDir(), CUSTOM_DRIVER_CFG_NAME).isFile()) {
                return;  // already configured (auto or user) -- leave it alone
            }
            if (deviceReportsVulkan13()) {
                return;  // stock driver already handles what DXVK needs
            }
            File destDir = new File(getFilesDir(), CUSTOM_DRIVER_DIR_NAME);
            deleteRecursive(destDir);
            if (!copyDriverAssetTree(DEFAULT_DRIVER_ASSET_DIR, destDir)) {
                return;  // no bundled driver in this build -- nothing to apply
            }
            File metaFile = new File(destDir, "meta.json");
            if (!metaFile.isFile()) {
                deleteRecursive(destDir);
                return;
            }
            String libraryName;
            try {
                org.json.JSONObject meta = new org.json.JSONObject(readWholeFile(metaFile));
                libraryName = meta.optString("libraryName", "");
            } catch (Exception e) {
                libraryName = "";
            }
            if (libraryName.isEmpty() || !new File(destDir, libraryName).isFile()) {
                deleteRecursive(destDir);
                return;
            }
            File cfg = new File(getFilesDir(), CUSTOM_DRIVER_CFG_NAME);
            try (java.io.FileWriter w = new java.io.FileWriter(cfg, false)) {
                w.write(libraryName);
                w.write("\n");
            }
            new File(getFilesDir(), CUSTOM_DRIVER_AUTO_MARKER_NAME).createNewFile();
        } catch (Exception e) {
            // Never let driver auto-selection take Setup down with it --
            // worst case the phone's stock driver loads, same as before
            // this feature existed.
        }
    }

    // FEATURE_VULKAN_HARDWARE_VERSION's reported "version" is a Vulkan
    // version int using the same VK_MAKE_API_VERSION encoding as the C API;
    // VK_API_VERSION_1_3 is (1<<22)|(3<<12) = 0x00403000. No feature entry
    // at all (some devices/emulators) is treated as "can't confirm 1.3" so
    // the recommended driver still gets a chance to help.
    private boolean deviceReportsVulkan13() {
        PackageManager pm = getPackageManager();
        FeatureInfo[] features = pm.getSystemAvailableFeatures();
        if (features == null) {
            return false;
        }
        for (FeatureInfo fi : features) {
            if (fi.name != null && fi.name.equals(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION)) {
                return fi.version >= 0x00403000;
            }
        }
        return false;
    }

    // Recursively copies an assets/ subtree (raw files, not a zip) into
    // destDir. Returns false if the source asset folder doesn't exist/is
    // empty -- lets callers tell "not bundled in this build" apart from a
    // real I/O failure without throwing.
    private boolean copyDriverAssetTree(String assetDir, File destDir) {
        AssetManager assets = getAssets();
        String[] children;
        try {
            children = assets.list(assetDir);
        } catch (java.io.IOException e) {
            return false;
        }
        if (children == null || children.length == 0) {
            return false;
        }
        if (!destDir.mkdirs() && !destDir.isDirectory()) {
            return false;
        }
        for (String child : children) {
            String childAssetPath = assetDir + "/" + child;
            File childDest = new File(destDir, child);
            try {
                String[] grandchildren = assets.list(childAssetPath);
                if (grandchildren != null && grandchildren.length > 0) {
                    if (!copyDriverAssetTree(childAssetPath, childDest)) {
                        return false;
                    }
                    continue;
                }
            } catch (java.io.IOException e) {
                return false;
            }
            try (java.io.InputStream in = assets.open(childAssetPath);
                 java.io.FileOutputStream out = new java.io.FileOutputStream(childDest)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            } catch (java.io.IOException e) {
                return false;
            }
        }
        return true;
    }

    private void refreshCustomDriverStatus() {
        if (customDriverStatusView != null) {
            customDriverStatusView.setText(customDriverStatusText());
        }
    }

    private void onImportCustomDriver() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
            "application/zip", "application/x-zip-compressed", "application/octet-stream"});
        try {
            startActivityForResult(intent, REQUEST_IMPORT_DRIVER);
        } catch (Exception e) {
            Toast.makeText(this, getString(R.string.setup_toast_no_file_picker, e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    private void onClearCustomDriver() {
        new File(getFilesDir(), CUSTOM_DRIVER_CFG_NAME).delete();
        new File(getFilesDir(), CUSTOM_DRIVER_AUTO_MARKER_NAME).delete();
        deleteRecursive(new File(getFilesDir(), CUSTOM_DRIVER_DIR_NAME));
        applyRecommendedDriverIfNeeded();
        refreshCustomDriverStatus();
        boolean autoApplied = new File(getFilesDir(), CUSTOM_DRIVER_AUTO_MARKER_NAME).isFile();
        Toast.makeText(this, autoApplied
            ? R.string.setup_toast_reset_auto
            : R.string.setup_toast_reset_stock,
            Toast.LENGTH_SHORT).show();
    }

    // customDriverDir passed to adrenotools_open_libvulkan() MUST NOT be on
    // sdcard/external storage (dlopen refuses world-writable paths) -- unzip
    // straight into getFilesDir() (app-private internal storage), the same
    // directory SDL_GetAndroidInternalStoragePath() resolves to in native code.
    private void importCustomDriver(Uri uri) {
        File destDir = new File(getFilesDir(), CUSTOM_DRIVER_DIR_NAME);
        File tmpDir = new File(getFilesDir(), CUSTOM_DRIVER_DIR_NAME + ".tmp");
        deleteRecursive(tmpDir);
        if (!tmpDir.mkdirs()) {
            Toast.makeText(this, R.string.setup_toast_driver_import_no_tmp, Toast.LENGTH_LONG).show();
            return;
        }

        try (java.io.InputStream in = getContentResolver().openInputStream(uri);
             java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(in)) {
            java.util.zip.ZipEntry entry;
            byte[] buf = new byte[8192];
            String tmpCanonical = tmpDir.getCanonicalPath();
            while ((entry = zip.getNextEntry()) != null) {
                File out = new File(tmpDir, entry.getName()).getCanonicalFile();
                // Zip-slip guard: never let an archive entry write outside tmpDir.
                if (!out.getPath().equals(tmpCanonical) && !out.getPath().startsWith(tmpCanonical + File.separator)) {
                    throw new java.io.IOException("zip entry escapes target folder: " + entry.getName());
                }
                if (entry.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null) {
                    parent.mkdirs();
                }
                try (java.io.FileOutputStream fos = new java.io.FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buf)) > 0) {
                        fos.write(buf, 0, n);
                    }
                }
            }
        } catch (Exception e) {
            deleteRecursive(tmpDir);
            Toast.makeText(this, getString(R.string.setup_toast_driver_import_failed, e.getMessage()), Toast.LENGTH_LONG).show();
            return;
        }

        File metaFile = findFileByName(tmpDir, "meta.json");
        if (metaFile == null) {
            deleteRecursive(tmpDir);
            Toast.makeText(this, R.string.setup_toast_driver_import_no_meta, Toast.LENGTH_LONG).show();
            return;
        }

        String libraryName;
        try {
            org.json.JSONObject meta = new org.json.JSONObject(readWholeFile(metaFile));
            libraryName = meta.optString("libraryName", "");
            if (libraryName.isEmpty()) {
                throw new org.json.JSONException("meta.json has no libraryName");
            }
            if (!new File(metaFile.getParentFile(), libraryName).isFile()) {
                throw new org.json.JSONException("meta.json names '" + libraryName + "' but that file isn't in the package");
            }
        } catch (Exception e) {
            deleteRecursive(tmpDir);
            Toast.makeText(this, getString(R.string.setup_toast_driver_import_bad_meta, e.getMessage()), Toast.LENGTH_LONG).show();
            return;
        }

        // The driver .so + meta.json might be nested inside a subfolder of
        // the zip -- move THAT folder into place as custom_driver/ (not
        // tmpDir itself), so the native side's customDriverDir points at
        // exactly the folder containing libraryName.
        File driverSourceDir = metaFile.getParentFile();
        deleteRecursive(destDir);
        boolean moved = driverSourceDir.renameTo(destDir);
        deleteRecursive(tmpDir);  // no-op if driverSourceDir WAS tmpDir (already moved away)
        if (!moved) {
            Toast.makeText(this, R.string.setup_toast_driver_import_no_finalize, Toast.LENGTH_LONG).show();
            return;
        }

        File cfg = new File(getFilesDir(), CUSTOM_DRIVER_CFG_NAME);
        try (java.io.FileWriter w = new java.io.FileWriter(cfg, false)) {
            w.write(libraryName);
            w.write("\n");
        } catch (java.io.IOException e) {
            Toast.makeText(this, getString(R.string.setup_toast_driver_config_failed, e.getMessage()), Toast.LENGTH_LONG).show();
            return;
        }
        // This is an explicit user choice, not the auto-selected default --
        // see applyRecommendedDriverIfNeeded() / CUSTOM_DRIVER_AUTO_MARKER_NAME.
        new File(getFilesDir(), CUSTOM_DRIVER_AUTO_MARKER_NAME).delete();

        refreshCustomDriverStatus();
        Toast.makeText(this, getString(R.string.setup_toast_driver_imported, libraryName), Toast.LENGTH_LONG).show();
    }

    private static File findFileByName(File dir, String name) {
        File[] children = dir.listFiles();
        if (children == null) {
            return null;
        }
        for (File c : children) {
            if (c.isDirectory()) {
                File found = findFileByName(c, name);
                if (found != null) {
                    return found;
                }
            } else if (c.getName().equals(name)) {
                return c;
            }
        }
        return null;
    }

    private static void deleteRecursive(File f) {
        if (f == null || !f.exists()) {
            return;
        }
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) {
                    deleteRecursive(c);
                }
            }
        }
        f.delete();
    }

    private static String readWholeFile(File f) throws java.io.IOException {
        StringBuilder sb = new StringBuilder();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f))) {
            char[] buf = new char[4096];
            int n;
            while ((n = r.read(buf)) > 0) {
                sb.append(buf, 0, n);
            }
        }
        return sb.toString();
    }

    private static String readFirstLine(File f) {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f))) {
            String line = r.readLine();
            return line != null ? line.trim() : null;
        } catch (java.io.IOException e) {
            return null;
        }
    }

    // GeneralsX @feature Android port 31/07/2026 dxvk.conf editor: real-device
    // Mali-G76 performance tuning kept coming back to "edit one line of
    // dxvk.conf on the phone" (e.g. d3d9.samplerAnisotropy), which meant a
    // file manager and manual editing outside the app every time. This edits
    // the actual file the engine reads (SDL3Main.cpp/DXVK read dxvk.conf
    // relative to CWD, i.e. the selected game folder -- see
    // copyBundledRuntimeIfMissing()'s comment above), as raw text rather than
    // a bespoke widget per key, so any current or future DXVK config option
    // works here without launcher code changes, and existing comments in the
    // file round-trip untouched instead of being reparsed away.
    private EditText dxvkConfigEdit;

    private void buildDxvkConfigSection(LinearLayout root) {
        LinearLayout content = startCard(root, getString(R.string.setup_card_dxvk_config));

        TextView help = new TextView(this);
        help.setAlpha(0.8f);
        help.setText(R.string.setup_dxvk_config_help);
        help.setPadding(0, 0, 0, dp(8));
        content.addView(help);

        dxvkConfigEdit = new EditText(this);
        dxvkConfigEdit.setTypeface(android.graphics.Typeface.MONOSPACE);
        dxvkConfigEdit.setTextSize(12);
        dxvkConfigEdit.setMinLines(6);
        dxvkConfigEdit.setMaxLines(20);
        dxvkConfigEdit.setGravity(android.view.Gravity.TOP | android.view.Gravity.START);
        dxvkConfigEdit.setInputType(android.text.InputType.TYPE_CLASS_TEXT
            | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        content.addView(dxvkConfigEdit);

        addButton(content, getString(R.string.setup_button_dxvk_config_save), this::onSaveDxvkConfig);
        addButton(content, getString(R.string.setup_button_dxvk_config_reset), this::onResetDxvkConfig);

        loadDxvkConfigIntoEditor();
    }

    // Live copy the engine actually reads -- lives in the user-selected game
    // folder, same as DefaultOptions.ini and fonts/ (see
    // copyBundledRuntimeIfMissing()).
    private File dxvkConfFile() {
        String gamePath = getSavedGamePath();
        return gamePath != null ? new File(gamePath, "dxvk.conf") : null;
    }

    // Pristine template this build ships, staged into getExternalFilesDir()
    // by GeneralsZHActivity on first run -- same source
    // copyBundledRuntimeIfMissing() copies from when a game folder is first
    // selected.
    private File bundledDxvkConfFile() {
        File root = getExternalFilesDir(null);
        return root != null ? new File(root, "dxvk.conf") : null;
    }

    private void loadDxvkConfigIntoEditor() {
        if (dxvkConfigEdit == null) {
            return;
        }
        File live = dxvkConfFile();
        File source = (live != null && live.isFile()) ? live : bundledDxvkConfFile();
        String text = "";
        if (source != null && source.isFile()) {
            try {
                text = readWholeFile(source);
            } catch (java.io.IOException e) {
                // Leave the editor empty; Save will still work and create a fresh file.
            }
        }
        dxvkConfigEdit.setText(text);
        boolean haveFolder = getSavedGamePath() != null;
        dxvkConfigEdit.setEnabled(haveFolder);
        dxvkConfigEdit.setHint(haveFolder ? null : getString(R.string.setup_dxvk_config_no_folder));
    }

    private void onSaveDxvkConfig() {
        File dest = dxvkConfFile();
        if (dest == null) {
            Toast.makeText(this, R.string.setup_dxvk_config_no_folder, Toast.LENGTH_LONG).show();
            return;
        }
        try (java.io.Writer w = new java.io.FileWriter(dest, false)) {
            w.write(dxvkConfigEdit.getText().toString());
        } catch (java.io.IOException e) {
            Toast.makeText(this, getString(R.string.setup_toast_options_save_failed, e.getMessage()), Toast.LENGTH_LONG).show();
            return;
        }
        Toast.makeText(this, R.string.setup_toast_dxvk_config_saved, Toast.LENGTH_SHORT).show();
    }

    private void onResetDxvkConfig() {
        File bundled = bundledDxvkConfFile();
        if (bundled == null || !bundled.isFile()) {
            Toast.makeText(this, R.string.setup_toast_dxvk_config_no_default, Toast.LENGTH_LONG).show();
            return;
        }
        String text;
        try {
            text = readWholeFile(bundled);
        } catch (java.io.IOException e) {
            Toast.makeText(this, getString(R.string.setup_toast_options_save_failed, e.getMessage()), Toast.LENGTH_LONG).show();
            return;
        }
        dxvkConfigEdit.setText(text);
        File dest = dxvkConfFile();
        if (dest != null) {
            try (java.io.Writer w = new java.io.FileWriter(dest, false)) {
                w.write(text);
            } catch (java.io.IOException e) {
                Toast.makeText(this, getString(R.string.setup_toast_options_save_failed, e.getMessage()), Toast.LENGTH_LONG).show();
                return;
            }
        }
        Toast.makeText(this, R.string.setup_toast_dxvk_config_reset, Toast.LENGTH_SHORT).show();
    }

    // GeneralsX @feature Android port 02/08/2026 Diagnostic marker toggles:
    // GXTrace.h (gx_trace.txt/gx_perf.txt) and SDL3Main.cpp (dxvk_hud.txt/
    // dxvk_validation.txt/dxvk_verbose_log.txt) all gate opt-in logging
    // behind a plain marker file dropped into the selected game folder,
    // checked relative to CWD after the engine chdir()s there --
    // deliberately no-adb, no-rebuild, so a tester can enable them (see
    // docs/port/ANDROID_PORT.md's "Diagnostic marker files" table, the
    // canonical list this mirrors). In practice almost nobody who isn't
    // already comfortable with a file manager knows to create an empty file
    // with an exact name, so this just does it for them: each switch
    // creates/deletes the marker directly, no new native code needed since
    // the engine side only ever checked "does this file exist", never its
    // contents. Each switch gets its own plain-language title AND a "when to
    // turn this on" description (translated, not just the raw filename) --
    // the filename itself still appears at the end of the description in
    // parentheses so a tester can match it up with exact instructions from
    // an issue reporter/maintainer.
    private static final String[] DIAGNOSTIC_MARKERS = {
        "gx_trace.txt", "gx_perf.txt", "dxvk_hud.txt", "dxvk_validation.txt", "dxvk_verbose_log.txt"
    };
    private static final int[] DIAGNOSTIC_TITLES = {
        R.string.setup_switch_gx_trace, R.string.setup_switch_gx_perf, R.string.setup_switch_dxvk_hud,
        R.string.setup_switch_dxvk_validation, R.string.setup_switch_dxvk_verbose_log
    };
    private static final int[] DIAGNOSTIC_DESCRIPTIONS = {
        R.string.setup_switch_gx_trace_desc, R.string.setup_switch_gx_perf_desc, R.string.setup_switch_dxvk_hud_desc,
        R.string.setup_switch_dxvk_validation_desc, R.string.setup_switch_dxvk_verbose_log_desc
    };
    private final SwitchCompat[] diagnosticSwitches = new SwitchCompat[DIAGNOSTIC_MARKERS.length];

    private void buildDiagnosticsSection(LinearLayout root) {
        LinearLayout content = startCard(root, getString(R.string.setup_card_diagnostics));

        TextView help = new TextView(this);
        help.setAlpha(0.8f);
        help.setText(R.string.setup_diagnostics_help);
        help.setPadding(0, 0, 0, dp(8));
        content.addView(help);

        diagnosticsNoFolderHint = new TextView(this);
        diagnosticsNoFolderHint.setAlpha(0.8f);
        diagnosticsNoFolderHint.setText(R.string.setup_diagnostics_no_folder);
        diagnosticsNoFolderHint.setPadding(0, 0, 0, dp(8));
        content.addView(diagnosticsNoFolderHint);

        for (int i = 0; i < DIAGNOSTIC_MARKERS.length; i++) {
            SwitchCompat sw = new SwitchCompat(this);
            sw.setText(getString(DIAGNOSTIC_TITLES[i]));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, dp(4), 0, 0);
            content.addView(sw, lp);
            diagnosticSwitches[i] = sw;

            TextView desc = new TextView(this);
            desc.setAlpha(0.7f);
            desc.setTextSize(12);
            desc.setText(getString(DIAGNOSTIC_DESCRIPTIONS[i]));
            LinearLayout.LayoutParams descLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            descLp.setMargins(dp(4), 0, 0, dp(10));
            content.addView(desc, descLp);
        }

        refreshDiagnosticsSwitches();
    }

    private TextView diagnosticsNoFolderHint;

    private File diagnosticMarkerFile(String name) {
        String gamePath = getSavedGamePath();
        return gamePath != null ? new File(gamePath, name) : null;
    }

    private void setDiagnosticMarker(String name, boolean enabled) {
        File marker = diagnosticMarkerFile(name);
        if (marker == null) {
            return;
        }
        if (enabled) {
            try {
                marker.createNewFile();
            } catch (java.io.IOException e) {
                Toast.makeText(this, getString(R.string.setup_toast_options_save_failed, e.getMessage()), Toast.LENGTH_LONG).show();
            }
        } else {
            marker.delete();
        }
    }

    private void refreshDiagnosticsSwitches() {
        boolean haveFolder = getSavedGamePath() != null;
        if (diagnosticsNoFolderHint != null) {
            diagnosticsNoFolderHint.setVisibility(haveFolder ? android.view.View.GONE : android.view.View.VISIBLE);
        }
        for (int i = 0; i < DIAGNOSTIC_MARKERS.length; i++) {
            final int index = i;
            SwitchCompat sw = diagnosticSwitches[index];
            if (sw == null) {
                continue;
            }
            File marker = diagnosticMarkerFile(DIAGNOSTIC_MARKERS[index]);
            sw.setOnCheckedChangeListener(null);
            sw.setChecked(marker != null && marker.isFile());
            sw.setEnabled(haveFolder);
            sw.setOnCheckedChangeListener((button, checked) -> setDiagnosticMarker(DIAGNOSTIC_MARKERS[index], checked));
        }
    }

    // GeneralsX @feature Android port 10/07/2026 GeneralsOnline (playgenerals.online)
    // account status -- the actual sign-in flow lives in GeneralsOnlineActivity,
    // this is just a status line + entry point.
    private TextView onlineStatusView;

    private void buildGeneralsOnlineSection(LinearLayout root) {
        LinearLayout content = startCard(root, getString(R.string.setup_card_online));

        onlineStatusView = new TextView(this);
        content.addView(onlineStatusView);

        addButton(content, getString(R.string.setup_button_online_account), () ->
            startActivity(new Intent(this, GeneralsOnlineActivity.class)));
    }

    private void refreshGeneralsOnlineStatus() {
        if (onlineStatusView == null) {
            return;
        }
        String displayName = GeneralsOnlineActivity.getSignedInDisplayName(this);
        onlineStatusView.setText(displayName != null
            ? getString(R.string.setup_online_signed_in, displayName)
            : getString(R.string.setup_online_signed_out));
    }

    private File optionsIniFile() {
        return new File(getFilesDir(), ".local/share/GeneralsX/GeneralsZH/Options.ini");
    }

    private File defaultOptionsIniFile() {
        String gamePath = getSavedGamePath();
        return gamePath != null ? new File(gamePath, "DefaultOptions.ini") : null;
    }

    private int readUiScalePercent() {
        java.util.Map<String, String> prefs = readKeyValueFile(optionsIniFile());
        String val = prefs.get("ResolutionFontAdjustment");
        if (val != null) {
            try {
                return Math.max(0, Math.min(150, Integer.parseInt(val.trim())));
            } catch (NumberFormatException ignored) {
                // Fall through to the engine's own default below.
            }
        }
        return 70;
    }

    private void writeUiScalePercent(int percent) {
        File file = optionsIniFile();

        // TheSuperHackers @bugfix Android port 07/07/2026 SDL3Main.cpp only
        // seeds Options.ini from the game folder's DefaultOptions.ini (full
        // GPU-detail defaults) the FIRST time it doesn't already exist. If we
        // create a bare Options.ini containing only ResolutionFontAdjustment
        // before the user ever launches the game once, that seeding is
        // permanently skipped and they silently lose those defaults. Seed
        // from DefaultOptions.ini ourselves first when the file is new.
        java.util.LinkedHashMap<String, String> prefs;
        if (!file.isFile()) {
            prefs = new java.util.LinkedHashMap<>(readKeyValueFile(defaultOptionsIniFile()));
        } else {
            prefs = new java.util.LinkedHashMap<>(readKeyValueFile(file));
        }
        prefs.put("ResolutionFontAdjustment", String.valueOf(percent));

        writeKeyValueFile(file, prefs);
    }

    private void writeKeyValueFile(File file, java.util.Map<String, String> prefs) {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            Toast.makeText(this, R.string.setup_toast_options_dir_failed, Toast.LENGTH_LONG).show();
            return;
        }
        try (java.io.PrintWriter w = new java.io.PrintWriter(new java.io.FileWriter(file, false))) {
            for (java.util.Map.Entry<String, String> e : prefs.entrySet()) {
                w.print(e.getKey());
                w.print(" = ");
                w.print(e.getValue());
                w.print('\n');
            }
        } catch (java.io.IOException e) {
            Toast.makeText(this, getString(R.string.setup_toast_options_save_failed, e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    // Matches UserPreferences::load()'s own format exactly ("key = value"
    // lines) so a file the engine already wrote round-trips untouched.
    private static java.util.Map<String, String> readKeyValueFile(File file) {
        java.util.LinkedHashMap<String, String> result = new java.util.LinkedHashMap<>();
        if (file == null || !file.isFile()) {
            return result;
        }
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(file))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq < 0) continue;
                String key = line.substring(0, eq).trim();
                String val = line.substring(eq + 1).trim();
                if (key.isEmpty() || val.isEmpty()) continue;
                result.put(key, val);
            }
        } catch (java.io.IOException ignored) {
            // Treat as empty; caller falls back to defaults.
        }
        return result;
    }

    private void addButton(LinearLayout root, String label, Runnable action) {
        MaterialButton b = new MaterialButton(this);
        b.setText(label);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(4), 0, dp(4));
        root.addView(b, lp);
    }

    // GeneralsX @feature Android port game-folder-integrity-check 07/09/2026
    // Plain text made "not set"/"looks valid"/"looks incomplete" too easy to
    // miss right after picking a folder -- the status line is now bold,
    // underlined and colored (green/amber/red) so it reads as a clear
    // verdict at a glance instead of blending into the paragraph around it.
    // The invalid/incomplete cases additionally get a blocking AlertDialog
    // right after picking (see onActivityResult()), since even a colored
    // line can be scrolled past unnoticed but a dialog can't.
    private void refreshStatus() {
        String path = getSavedGamePath();
        SpannableStringBuilder sb = new SpannableStringBuilder();
        if (path == null) {
            sb.append(getString(R.string.setup_status_folder_not_set));
        } else {
            File dir = new File(path);
            boolean valid = isValidGameFolder(dir);
            sb.append(getString(R.string.setup_status_folder_line, path));
            int statusStart = sb.length();
            int statusColorRes;
            if (!valid) {
                sb.append(getString(R.string.setup_status_folder_invalid));
                statusColorRes = R.color.gzh_status_error;
            } else {
                java.util.List<String> issues = findGameFolderIntegrityIssues(dir);
                if (issues.isEmpty()) {
                    sb.append(getString(R.string.setup_status_folder_valid));
                    statusColorRes = R.color.gzh_status_ok;
                } else {
                    sb.append(getString(R.string.setup_status_folder_incomplete, String.join("; ", issues)));
                    statusColorRes = R.color.gzh_status_warn;
                }
            }
            int statusEnd = sb.length();
            sb.setSpan(new ForegroundColorSpan(ContextCompat.getColor(this, statusColorRes)),
                statusStart, statusEnd, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            sb.setSpan(new StyleSpan(Typeface.BOLD), statusStart, statusEnd, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            sb.setSpan(new UnderlineSpan(), statusStart, statusEnd, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        sb.append('\n');
        sb.append(getString(R.string.setup_status_logs_note));
        statusText.setText(sb);
        updateGameLanguageStatusView();
    }

    private void showFolderProblemDialog(String message) {
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.setup_dialog_folder_problem_title)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show();
    }

    private String getSavedGamePath() {
        return getSavedGamePath(this);
    }

    // Public + static so GeneralsZHActivity uses the exact same recovery
    // logic instead of its own copy that only ever checked SharedPreferences.
    static String getSavedGamePath(android.content.Context ctx) {
        SharedPreferences prefs = ctx.getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String path = prefs.getString(PREF_GAME_PATH, null);
        if (path != null) {
            return path;
        }

        // Not in this install's private prefs (fresh install after an
        // uninstall, most likely) -- check the external marker left by a
        // previous install and self-heal by restoring it into prefs.
        String recovered = readExternalMarker();
        if (recovered != null && isValidGameFolder(new File(recovered))) {
            prefs.edit().putString(PREF_GAME_PATH, recovered).apply();
            File nativeMarker = new File(ctx.getFilesDir(), "gamedata_path.txt");
            try (java.io.FileWriter w = new java.io.FileWriter(nativeMarker, false)) {
                w.write(recovered);
                w.write("\n");
            } catch (java.io.IOException e) {
                // Not fatal: native code just won't see the recovered path
                // until the user re-saves it once via Setup.
            }
            return recovered;
        }
        return null;
    }

    private static File externalMarkerFile() {
        File root = Environment.getExternalStorageDirectory();
        return root != null ? new File(root, EXTERNAL_MARKER_NAME) : null;
    }

    private static String readExternalMarker() {
        File marker = externalMarkerFile();
        if (marker == null || !marker.isFile()) {
            return null;
        }
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(marker))) {
            String line = r.readLine();
            return (line != null && !line.isEmpty()) ? line.trim() : null;
        } catch (java.io.IOException e) {
            return null;
        }
    }

    static boolean isValidGameFolder(File dir) {
        if (dir == null || !dir.isDirectory()) {
            return false;
        }
        for (String name : REQUIRED_GAME_FILES) {
            if (new File(dir, name).exists()) {
                return true;
            }
        }
        return false;
    }

    // GeneralsX @feature Android port game-folder-integrity-check 07/09/2026
    // isValidGameFolder() above only checks that INIZH.big/INI.big EXIST --
    // a truncated/incomplete copy (interrupted transfer, bad extraction,
    // etc.) can pass that check while still crashing the engine deep in
    // INI::loadFileDirectory() the first time it hits a directory whose
    // data never made it into the archive: a real user report hit
    // "[INI] ERROR: No files read from directory 'Data\INI\Default\Weather'"
    // followed by an uncaught C++ exception during GameEngine::init() --
    // well before any graphics/Vulkan code runs, so it isn't a driver bug.
    // The same broken copy also failed the same way in Winlator (a
    // completely different Wine/DXVK compatibility layer), confirming it's
    // the user's own file set, not this port's code.
    //
    // These two checks stay cheap by only reading each .big's small BIGF
    // directory table (a header + a null-terminated path per entry), never
    // the actual multi-hundred-MB file payloads the table points at:
    //   1. Every *.big already present must start with the "BIGF" magic
    //      and be non-zero size (catches an empty/corrupted download).
    //   2. Whichever ini archive is present (INI.big or INIZH.big) must
    //      contain Data/INI/Default/Weather.ini specifically -- the exact
    //      file the real crash above needed, and a reasonable proxy for
    //      "this archive's directory table wasn't truncated partway
    //      through", since an interrupted copy would most likely lose
    //      entries somewhere in the middle of the table, not leave out
    //      exactly and only this one file.
    //
    // BIG format read from this project's own parser --
    // Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp,
    // openArchiveFile(): "BIGF" magic at offset 0, file count (big-endian)
    // at offset 8, directory table starting at offset 0x10, each entry is
    // [4-byte offset, big-endian][4-byte size, big-endian][null-terminated
    // backslash-separated path].
    private static final String BIG_CRITICAL_ENTRY = "data\\ini\\default\\weather.ini";

    // GeneralsX @bugfix Android port game-folder-integrity-check 08/30/2026
    // Some retail/Deluxe layouts don't put the base Generals archives (incl.
    // INI.big) next to the ZH ones -- they nest an entire base-game copy in
    // a subfolder instead (confirmed via real "Generals Deluxe" install
    // screenshots: root has INIZH.big + *ZH.big, and a "ZH_Generals"
    // subfolder has a plain INI.big alongside unsuffixed archives). The
    // engine itself already expects layouts like this -- see
    // loadBaseGeneralsAssetsForZH() in StdBIGFileSystem.cpp, which tries a
    // handful of base-game locations including a "ZH_Generals" sibling --
    // but that folder name isn't the only one real installers have used
    // over the years, so rather than hardcode it, scan every immediate
    // subfolder of the selected directory for ini archives. One level deep
    // only: cheap (just a directory listing per subfolder), and matches
    // what the engine itself is willing to look for automatically.
    private java.util.List<String> findGameFolderIntegrityIssues(File dir) {
        java.util.List<String> issues = new java.util.ArrayList<>();
        if (dir == null || !dir.isDirectory()) {
            return issues;
        }
        java.util.List<File> scanRoots = new java.util.ArrayList<>();
        scanRoots.add(dir);
        File[] subdirs = dir.listFiles(File::isDirectory);
        if (subdirs != null) {
            for (File sub : subdirs) {
                scanRoots.add(sub);
            }
        }

        // GeneralsX @bugfix Android port game-folder-integrity-check 07/09/2026
        // INI.big (base Generals) and INIZH.big (the Zero Hour expansion)
        // both get loaded and merged into one virtual file list by the
        // engine's ArchiveFileSystem -- they don't replace each other, ZH
        // only overrides/adds specific entries on top of the base game's.
        // Weather.ini is base-game content, not something Zero Hour
        // replaces, so on an install with both archives it can legitimately
        // live in INI.big while INIZH.big itself is missing/corrupted --
        // checking only whichever one was found last would have produced a
        // false "missing data" report. Collect every present ini archive
        // (root and any subfolder alike) and only flag a problem if NONE of
        // them have the entry, matching how the actual merged filesystem
        // resolves it.
        java.util.List<File> iniArchives = new java.util.ArrayList<>();
        java.util.Set<String> presentArchives = new java.util.HashSet<>();
        for (File root : scanRoots) {
            File[] bigFiles = root.listFiles((d, name) -> name.toLowerCase(java.util.Locale.ROOT).endsWith(".big"));
            if (bigFiles == null) {
                continue;
            }
            String prefix = (root == dir) ? "" : (root.getName() + "\\");
            for (File f : bigFiles) {
                if (f.length() == 0) {
                    issues.add(getString(R.string.setup_folder_issue_empty_file, prefix + f.getName()));
                    continue;
                }
                if (!hasBigFHeader(f)) {
                    issues.add(getString(R.string.setup_folder_issue_bad_header, prefix + f.getName()));
                    continue;
                }
                String lower = f.getName().toLowerCase(java.util.Locale.ROOT);
                if (lower.equals("ini.big") || lower.equals("inizh.big")) {
                    iniArchives.add(f);
                }
                presentArchives.add(lower);
            }
        }
        if (!iniArchives.isEmpty()) {
            boolean found = false;
            for (File f : iniArchives) {
                if (bigArchiveHasEntry(f, BIG_CRITICAL_ENTRY)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                StringBuilder names = new StringBuilder();
                for (File f : iniArchives) {
                    if (names.length() > 0) {
                        names.append(", ");
                    }
                    File parent = f.getParentFile();
                    boolean nested = parent != null && !parent.equals(dir);
                    names.append(nested ? (parent.getName() + "\\" + f.getName()) : f.getName());
                }
                issues.add(getString(R.string.setup_folder_issue_missing_data, names.toString()));
            }
        }

        // GeneralsX @bugfix Android port game-folder-integrity-check 06/09/2026
        // A copy holding only *ZH.big archives passes every check above -- the
        // archives are all present, valid and complete -- and then renders its
        // terrain and half its units in solid magenta, because Zero Hour is an
        // expansion: its archives carry only what it added or changed, and the
        // original game's artwork lives in the base game's Terrain.big /
        // Textures.big / W3D.big. Traced from a real report: every archive in
        // the folder mounted successfully, yet trdirtroad, trsidewalk,
        // trtwolane, trtraintrack and their neighbours were nowhere in the
        // merged filesystem, so the engine substituted the magenta
        // missing-texture placeholder for each of them.
        //
        // Only flag it when ALL of the base asset archives are absent from
        // every scanned root: a copy missing one of the three is unusual but
        // could still be deliberate, whereas none of the three is
        // unambiguously a Zero-Hour-only folder. Deliberately keyed on the
        // asset archives rather than "any archive without a ZH suffix" --
        // Music.big is commonly present on its own and would mask the problem.
        StringBuilder missing = new StringBuilder();
        for (String name : BASE_GAME_REQUIRED_ARCHIVES) {
            if (!presentArchives.contains(name.toLowerCase(java.util.Locale.ROOT))) {
                if (missing.length() > 0) {
                    missing.append(", ");
                }
                missing.append(name);
            }
        }
        if (missing.length() > 0) {
            issues.add(getString(R.string.setup_folder_issue_no_base_game, missing.toString()));
        }
        return issues;
    }

    // GeneralsX @bugfix Android port game-folder-integrity-check 06/09/2026
    // The base Generals archives, named as they ship. Zero Hour's own archives
    // all carry a ZH suffix and hold only what the expansion added or changed,
    // so a copy with nothing but *ZH.big passes every other check here -- all
    // archives valid, none truncated -- and then renders the original game's
    // terrain and units as solid magenta. Reported as the exact list of names
    // that are absent, because "your copy is incomplete" leaves the user with
    // nothing to act on.
    //
    // Deliberately limited to archives whose base-game names are certain.
    // Language-specific ones (English.big and the AudioEnglish/SpeechEnglish/
    // W3DEnglish family) are left out: which of them a given release ships
    // varies by SKU and language, and naming one that a perfectly good copy
    // never had would send people hunting for a file that does not exist.
    // A missing language archive also degrades far more gracefully than a
    // missing Textures.big.
    private static final String[] BASE_GAME_REQUIRED_ARCHIVES = {
        "INI.big", "Terrain.big", "Textures.big", "W3D.big", "Window.big",
        "Shaders.big", "Audio.big", "Speech.big", "Maps.big", "Music.big"
    };

    private static boolean hasBigFHeader(File f) {
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(f, "r")) {
            byte[] magic = new byte[4];
            if (raf.read(magic) != 4) {
                return false;
            }
            return magic[0] == 'B' && magic[1] == 'I' && magic[2] == 'G' && magic[3] == 'F';
        } catch (java.io.IOException e) {
            return false;
        }
    }

    private static int readBigEndianInt(java.io.RandomAccessFile raf) throws java.io.IOException {
        int b0 = raf.read(), b1 = raf.read(), b2 = raf.read(), b3 = raf.read();
        if ((b0 | b1 | b2 | b3) < 0) {
            throw new java.io.EOFException();
        }
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }

    private static boolean bigArchiveHasEntry(File bigFile, String targetLowerPath) {
        if (!bigFile.isFile() || bigFile.length() < 0x10) {
            return false;
        }
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(bigFile, "r")) {
            if (!hasBigFHeader(bigFile)) {
                return false;
            }
            raf.seek(8);
            int numFiles = readBigEndianInt(raf);
            if (numFiles < 0 || numFiles > 500000) {
                return false;  // sanity guard against a corrupted/garbage header
            }
            raf.seek(0x10);
            byte[] nameBuf = new byte[512];
            for (int i = 0; i < numFiles; i++) {
                raf.skipBytes(8);  // per-entry offset + size, not needed for this check
                int len = 0;
                int b;
                while ((b = raf.read()) > 0) {
                    if (len < nameBuf.length - 1) {
                        nameBuf[len++] = (byte) b;
                    }
                }
                if (b < 0) {
                    return false;  // ran off the end of the file mid-entry -- truncated archive
                }
                String path = new String(nameBuf, 0, len, java.nio.charset.StandardCharsets.US_ASCII)
                    .toLowerCase(java.util.Locale.ROOT);
                if (path.equals(targetLowerPath)) {
                    return true;
                }
            }
        } catch (java.io.IOException e) {
            return false;
        }
        return false;
    }

    private static final int REQUEST_LEGACY_STORAGE_PERMISSION = 1003;

    private void onSelectGameFolder() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Toast.makeText(this, R.string.setup_toast_grant_all_files, Toast.LENGTH_LONG).show();
                try {
                    Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                    intent.setData(Uri.parse("package:" + getPackageName()));
                    startActivity(intent);
                } catch (Exception e) {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
                return;
            }
        } else if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            // MANAGE_EXTERNAL_STORAGE / isExternalStorageManager() don't exist
            // before API 30 -- this is the pre-R equivalent, otherwise
            // FolderPickerActivity opens with no storage permission at all
            // and its File.listFiles() silently comes back empty.
            ActivityCompat.requestPermissions(this,
                new String[] { Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE },
                REQUEST_LEGACY_STORAGE_PERMISSION);
            return;
        }
        startActivityForResult(new Intent(this, FolderPickerActivity.class), 1001);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_LEGACY_STORAGE_PERMISSION) {
            boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            if (granted) {
                startActivityForResult(new Intent(this, FolderPickerActivity.class), 1001);
            } else {
                Toast.makeText(this, R.string.setup_toast_storage_permission_denied, Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 1001 && resultCode == Activity.RESULT_OK && data != null) {
            String path = data.getStringExtra(FolderPickerActivity.EXTRA_SELECTED_PATH);
            if (path != null) {
                saveGamePath(path);
                refreshStatus();
                File dir = new File(path);
                boolean valid = isValidGameFolder(dir);
                // GeneralsX @feature Android port game-folder-integrity-check
                // 07/09/2026 - a Toast auto-dismisses in a couple of seconds
                // and is easy to miss entirely; a real problem here means
                // the game WILL crash on launch, so it gets a blocking
                // dialog the user has to acknowledge instead. The colored
                // status line in refreshStatus() (visible right below) still
                // covers "I want to re-check the status later" without
                // re-triggering the dialog every time the screen redraws.
                if (!valid) {
                    showFolderProblemDialog(getString(R.string.setup_status_folder_invalid).trim());
                } else {
                    java.util.List<String> issues = findGameFolderIntegrityIssues(dir);
                    if (!issues.isEmpty()) {
                        showFolderProblemDialog(getString(R.string.setup_status_folder_incomplete,
                            String.join("; ", issues)).trim());
                    } else {
                        Toast.makeText(this, R.string.setup_toast_folder_saved, Toast.LENGTH_LONG).show();
                    }
                }
            }
        } else if (requestCode == REQUEST_IMPORT_DRIVER && resultCode == Activity.RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                importCustomDriver(uri);
            }
        }
    }

    private void saveGamePath(String path) {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
            .putString(PREF_GAME_PATH, path)
            .apply();
        // GeneralsZHActivity/SDL3Main.cpp read this plain-text marker on the
        // NEXT launch (native code has no Android SharedPreferences access).
        File marker = new File(getFilesDir(), "gamedata_path.txt");
        try (java.io.FileWriter w = new java.io.FileWriter(marker, false)) {
            w.write(path);
            w.write("\n");
        } catch (java.io.IOException e) {
            Toast.makeText(this, getString(R.string.setup_toast_marker_save_failed, e.getMessage()), Toast.LENGTH_LONG).show();
        }
        File externalMarker = externalMarkerFile();
        if (externalMarker != null) {
            try (java.io.FileWriter w = new java.io.FileWriter(externalMarker, false)) {
                w.write(path);
                w.write("\n");
            } catch (java.io.IOException e) {
                // Not fatal: uninstall-survival just won't work for this
                // install; the private-prefs/marker-file path above still
                // covers normal in-place updates.
            }
        }
        File bundledRoot = getExternalFilesDir(null);
        if (bundledRoot != null) {
            copyBundledRuntimeIfMissing(bundledRoot, path);
        }
        applyGameLanguageOverride(LocaleHelper.getSavedLanguageTag(this));
    }

    // GeneralsX @bugfix Android port 07/07/2026 dxvk.conf, DefaultOptions.ini,
    // AND the entire fonts/ directory are all read by the engine relative to
    // its CWD (see SDL3Main.cpp and render2dsentence.cpp's
    // Locate_Font_FontConfig, which does access("fonts/<name>.ttf", R_OK) with
    // no absolute-path fallback on Android/iOS). CWD is now whatever folder
    // the user picked — not the external-files-dir path package-android-zh.sh
    // originally extracted the APK's bundled copies into. Missing fonts/
    // specifically means EVERY W3DFont load fails ("load miss" for every
    // single font in the log) and every button in the UI renders with no
    // text at all — copy all three, not just dxvk.conf. Static + takes
    // bundledRoot explicitly so GeneralsZHActivity can also call this on
    // every launch (an already-configured install needs the fix applied
    // retroactively, not just at folder-selection time).
    static void copyBundledRuntimeIfMissing(File bundledRoot, String gameFolderPath) {
        copyFileIfMissing(new File(bundledRoot, "dxvk.conf"), new File(gameFolderPath, "dxvk.conf"));
        copyFileIfMissing(new File(bundledRoot, "DefaultOptions.ini"), new File(gameFolderPath, "DefaultOptions.ini"));
        copyDirIfMissing(new File(bundledRoot, "fonts"), new File(gameFolderPath, "fonts"));
        syncEngineWindowOverrides(bundledRoot, gameFolderPath);
    }

    // GeneralsX @bugfix Android port 02/08/2026 GroupPanel.wnd (the native
    // in-engine unit-group panel) is a loose Window\ override WE inject --
    // see GameWindowManagerScript.cpp's Window\ path resolution -- not a
    // user file, so unlike dxvk.conf/DefaultOptions.ini/fonts/ above it must
    // NOT use copy-if-missing: every edit to it (this exact feature has
    // already gone through several rounds of position/behavior/style fixes)
    // would otherwise silently never reach an existing install, since the
    // very first copy would win forever. Always overwrite this one
    // subdirectory; bundledRoot's own copy is kept fresh the same way, see
    // GeneralsZHActivity.copyAssetTree's ALWAYS_OVERWRITE_PREFIX.
    private static void syncEngineWindowOverrides(File bundledRoot, String gameFolderPath) {
        File srcDir = new File(bundledRoot, "Window");
        File[] children = srcDir.listFiles();
        if (children == null) {
            return;
        }
        File destDir = new File(gameFolderPath, "Window");
        if (!destDir.isDirectory() && !destDir.mkdirs()) {
            return;
        }
        for (File child : children) {
            if (child.isFile()) {
                copyFileOverwrite(child, new File(destDir, child.getName()));
            }
        }
    }

    private static void copyFileOverwrite(File src, File dest) {
        try (java.io.InputStream in = new java.io.FileInputStream(src);
             java.io.OutputStream out = new java.io.FileOutputStream(dest)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        } catch (java.io.IOException e) {
            // Not fatal: worst case the engine keeps using a stale or
            // missing override, same as before this fix existed.
        }
    }

    private static void copyFileIfMissing(File bundled, File dest) {
        if (dest.exists() || !bundled.exists()) {
            return;
        }
        try (java.io.InputStream in = new java.io.FileInputStream(bundled);
             java.io.OutputStream out = new java.io.FileOutputStream(dest)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        } catch (java.io.IOException e) {
            // Not fatal: caller falls back to its own defaults without this file.
        }
    }

    private static void copyDirIfMissing(File bundledDir, File destDir) {
        if (destDir.exists() || !bundledDir.isDirectory()) {
            return;
        }
        if (!destDir.mkdirs()) {
            return;
        }
        File[] children = bundledDir.listFiles();
        if (children == null) {
            return;
        }
        for (File child : children) {
            copyFileIfMissing(child, new File(destDir, child.getName()));
        }
    }

    private void onClearGameFolder() {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().remove(PREF_GAME_PATH).apply();
        new File(getFilesDir(), "gamedata_path.txt").delete();
        new File(getFilesDir(), "game_language.cfg").delete();
        File externalMarker = externalMarkerFile();
        if (externalMarker != null) {
            externalMarker.delete();
        }
        refreshStatus();
        refreshDiagnosticsSwitches();
        Toast.makeText(this, R.string.setup_toast_folder_cleared, Toast.LENGTH_SHORT).show();
    }

    private void onViewLogs() {
        startActivity(new Intent(this, LogViewerActivity.class));
    }

    // GeneralsX @bugfix Android port 31/07/2026 Setup is portrait-first now
    // (see AndroidManifest.xml/onCreate() comments), so launching straight
    // into GeneralsZHActivity (locked landscape) can trigger a real
    // portrait->landscape rotation right as the game's native window-size
    // probe (WW3D::Init()) runs -- previously sidestepped entirely by never
    // letting Setup rotate. Force landscape here and wait for
    // onConfigurationChanged() to confirm the OS has actually applied it
    // before starting the game, instead of guessing with a fixed delay. If
    // we're already landscape (e.g. a tablet, or the user physically
    // rotated the phone), there's nothing to wait for.
    private boolean pendingLaunchAfterRotation = false;

    private void onLaunchGame() {
        if (getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE) {
            startActivity(new Intent(this, GeneralsZHActivity.class));
            return;
        }
        pendingLaunchAfterRotation = true;
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (pendingLaunchAfterRotation && newConfig.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            pendingLaunchAfterRotation = false;
            startActivity(new Intent(this, GeneralsZHActivity.class));
        }
    }

    private int dp(int value) {
        float density = getResources().getDisplayMetrics().density;
        return (int) (value * density + 0.5f);
    }
}
