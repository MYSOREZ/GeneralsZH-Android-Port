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

// GeneralsX @feature Android port launcher-ui-refresh 06/09/2026
//
// Everything the launcher can do that a player never needs to do: swapping
// the Vulkan driver out from under the game, hand-editing dxvk.conf, and
// turning on the diagnostic marker files that make the engine log more.
//
// These three used to sit on the main screen as three more cards of exactly
// the same visual weight as "Select Game Folder", between the player and
// the button they actually came for. Worse, the DXVK config card put a
// multi-line raw-text editor -- something you only ever touch when a
// maintainer tells you which line to change -- directly in the scroll path
// of someone whose only goal was to launch the game.
//
// Splitting them onto their own screen is a statement about who they are
// for, not a way of hiding them: the main screen still links here in one
// tap, the diagnostics still explain themselves in plain language, and
// nothing here changed behavior. The code below is moved, not rewritten;
// its comments are the original ones and still describe why each piece
// works the way it does.
//
// The custom-driver and dxvk.conf sections continue to appear only when the
// Vulkan render backend is selected, because the GLES paths never load DXVK
// at all -- but that condition now also has a visible explanation on screen
// (setup_advanced_vulkan_only) instead of the sections silently not being
// there.

package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import com.google.android.material.materialswitch.MaterialSwitch;

import java.io.File;

public class AdvancedActivity extends Activity {

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
    static final String CUSTOM_DRIVER_DIR_NAME = "custom_driver";
    static final String CUSTOM_DRIVER_CFG_NAME = "custom_driver.cfg";
    // GeneralsX @feature Android port 13/07/2026 Marks that custom_driver.cfg
    // was populated by applyRecommendedDriverIfNeeded() rather than by the
    // user importing their own .zip -- lets the status text and the
    // "reset" button distinguish "we picked this for you" from "you chose
    // this", without changing anything on the native loading side (both
    // cases are the same custom_driver.cfg/custom_driver/ that
    // TryLoadCustomVulkanDriver() in SDL3Main.cpp already reads).
    static final String CUSTOM_DRIVER_AUTO_MARKER_NAME = "custom_driver.auto";
    // Bundled fallback driver (staged by scripts/build/android/fetch-turnip.sh
    // into this asset folder at build time) -- see applyRecommendedDriverIfNeeded().
    private static final String DEFAULT_DRIVER_ASSET_DIR = "default_driver";
    private static final int REQUEST_IMPORT_DRIVER = 1002;

    private TextView customDriverStatusView;
    private EditText dxvkConfigEdit;
    private TextView diagnosticsNoFolderHint;

    @Override
    protected void attachBaseContext(Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.setup_advanced_title);
        // Same reasoning as SetupActivity's own guard: a screen that exists
        // to diagnose a broken install must not be the thing that breaks.
        try {
            buildUi();
        } catch (Throwable t) {
            buildFallbackUi(t);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        loadDxvkConfigIntoEditor();
        refreshDiagnosticsSwitches();
        refreshCustomDriverStatus();
    }

    private void buildUi() {
        LinearLayout column = LauncherUi.scrollingScaffold(this);
        LauncherUi.screenHeader(column,
            getString(R.string.setup_advanced_title),
            getString(R.string.setup_advanced_subtitle));

        boolean vulkan = SetupActivity.isVulkanBackendSelected(this);
        if (vulkan) {
            LauncherUi.sectionLabel(column, getString(R.string.setup_card_render_backend));
            buildCustomDriverSection(column);
            buildDxvkConfigSection(column);
        }

        LauncherUi.sectionLabel(column, getString(R.string.setup_card_diagnostics));
        buildDiagnosticsSection(column);

        if (!vulkan) {
            LauncherUi.help(LauncherUi.card(column, null),
                getString(R.string.setup_advanced_vulkan_only));
        }

        LauncherUi.navigationRow(column, getString(R.string.setup_button_view_logs),
            getString(R.string.setup_logs_row_subtitle),
            () -> startActivity(new Intent(this, LogViewerActivity.class)));
    }

    // Framework widgets only, for the same reason SetupActivity's fallback
    // uses them: whatever broke the real UI is most likely Material or the
    // theme, and a fallback built from those would break with it.
    private void buildFallbackUi(Throwable failure) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = LauncherUi.dp(this, LauncherUi.SPACE_5);
        root.setPadding(pad, pad, pad, pad);
        setContentView(root);
        InsetUtil.applySafeInsets(root);

        TextView warning = new TextView(this);
        warning.setText(getString(R.string.setup_fallback_warning, String.valueOf(failure)));
        root.addView(warning, LauncherUi.marginsBelow(this, LauncherUi.SPACE_4));

        Button logs = new Button(this);
        logs.setText(R.string.setup_button_view_logs);
        logs.setMinHeight(LauncherUi.dp(this, LauncherUi.TOUCH_TARGET));
        logs.setOnClickListener(v -> startActivity(new Intent(this, LogViewerActivity.class)));
        root.addView(logs, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    // ---------------------------------------------------------------- driver

    private void buildCustomDriverSection(LinearLayout column) {
        LauncherUi.Expander section = LauncherUi.expander(column,
            getString(R.string.setup_card_vulkan_driver), customDriverStatusText());

        customDriverStatusView = LauncherUi.body(section.content, customDriverStatusText());
        LauncherUi.button(section.content, LauncherUi.BUTTON_OUTLINED,
            getString(R.string.setup_button_import_driver), this::onImportCustomDriver);
        LauncherUi.button(section.content, LauncherUi.BUTTON_TEXT,
            getString(R.string.setup_button_reset_driver), this::onClearCustomDriver);
        LauncherUi.help(section.content, getString(R.string.setup_driver_help));
        customDriverSection = section;
    }

    private LauncherUi.Expander customDriverSection;

    private String customDriverStatusText() {
        File cfg = new File(getFilesDir(), CUSTOM_DRIVER_CFG_NAME);
        if (!cfg.isFile()) {
            return getString(R.string.setup_driver_status_none);
        }
        String driverName = SetupActivity.readFirstLine(cfg);
        boolean isAuto = new File(getFilesDir(), CUSTOM_DRIVER_AUTO_MARKER_NAME).isFile();
        String name = driverName != null ? driverName : getString(R.string.setup_driver_unknown);
        return getString(isAuto ? R.string.setup_driver_status_auto : R.string.setup_driver_status_manual, name);
    }

    // GeneralsX @feature Android port 13/07/2026 Auto-applies the bundled
    // Turnip driver on Adreno phones whose stock driver reports less than
    // Vulkan 1.3, without touching anything if the user already imported
    // their own driver or the phone doesn't need help. Called on every
    // launcher start (cheap no-op once satisfied) and again from
    // onClearCustomDriver() so "reset" actually restores the recommended
    // state instead of just going blank.
    //
    // GeneralsX @build Android port launcher-ui-refresh 06/09/2026 static +
    // Context: this has to keep running on every launcher start even though
    // the driver UI now lives on this screen, which most people will never
    // open. SetupActivity.onResume() calls it.
    static void applyRecommendedDriverIfNeeded(Context ctx) {
        try {
            if (new File(ctx.getFilesDir(), CUSTOM_DRIVER_CFG_NAME).isFile()) {
                return;  // already configured (auto or user) -- leave it alone
            }
            if (deviceReportsVulkan13(ctx)) {
                return;  // stock driver already handles what DXVK needs
            }
            File destDir = new File(ctx.getFilesDir(), CUSTOM_DRIVER_DIR_NAME);
            deleteRecursive(destDir);
            if (!copyDriverAssetTree(ctx, DEFAULT_DRIVER_ASSET_DIR, destDir)) {
                return;  // no bundled driver in this build -- nothing to apply
            }
            File metaFile = new File(destDir, "meta.json");
            if (!metaFile.isFile()) {
                deleteRecursive(destDir);
                return;
            }
            String libraryName;
            try {
                org.json.JSONObject meta = new org.json.JSONObject(SetupActivity.readWholeFile(metaFile));
                libraryName = meta.optString("libraryName", "");
            } catch (Exception e) {
                libraryName = "";
            }
            if (libraryName.isEmpty() || !new File(destDir, libraryName).isFile()) {
                deleteRecursive(destDir);
                return;
            }
            File cfg = new File(ctx.getFilesDir(), CUSTOM_DRIVER_CFG_NAME);
            try (java.io.FileWriter w = new java.io.FileWriter(cfg, false)) {
                w.write(libraryName);
                w.write("\n");
            }
            new File(ctx.getFilesDir(), CUSTOM_DRIVER_AUTO_MARKER_NAME).createNewFile();
        } catch (Exception e) {
            // Never let driver auto-selection take the launcher down with it
            // -- worst case the phone's stock driver loads, same as before
            // this feature existed.
        }
    }

    // FEATURE_VULKAN_HARDWARE_VERSION's reported "version" is a Vulkan
    // version int using the same VK_MAKE_API_VERSION encoding as the C API;
    // VK_API_VERSION_1_3 is (1<<22)|(3<<12) = 0x00403000. No feature entry
    // at all (some devices/emulators) is treated as "can't confirm 1.3" so
    // the recommended driver still gets a chance to help.
    private static boolean deviceReportsVulkan13(Context ctx) {
        PackageManager pm = ctx.getPackageManager();
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
    private static boolean copyDriverAssetTree(Context ctx, String assetDir, File destDir) {
        AssetManager assets = ctx.getAssets();
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
                    if (!copyDriverAssetTree(ctx, childAssetPath, childDest)) {
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
        String status = customDriverStatusText();
        if (customDriverStatusView != null) {
            customDriverStatusView.setText(status);
        }
        if (customDriverSection != null) {
            customDriverSection.setSummary(status);
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
        applyRecommendedDriverIfNeeded(this);
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
            org.json.JSONObject meta = new org.json.JSONObject(SetupActivity.readWholeFile(metaFile));
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

    static File findFileByName(File dir, String name) {
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

    static void deleteRecursive(File f) {
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

    // ------------------------------------------------------------ dxvk.conf

    // GeneralsX @feature Android port 31/07/2026 dxvk.conf editor: real-device
    // Mali-G76 performance tuning kept coming back to "edit one line of
    // dxvk.conf on the phone" (e.g. d3d9.samplerAnisotropy), which meant a
    // file manager and manual editing outside the app every time. This edits
    // the actual file the engine reads (SDL3Main.cpp/DXVK read dxvk.conf
    // relative to CWD, i.e. the selected game folder -- see
    // copyBundledRuntimeIfMissing()'s comment in SetupActivity), as raw text
    // rather than a bespoke widget per key, so any current or future DXVK
    // config option works here without launcher code changes, and existing
    // comments in the file round-trip untouched instead of being reparsed
    // away.
    private void buildDxvkConfigSection(LinearLayout column) {
        LauncherUi.Expander section = LauncherUi.expander(column,
            getString(R.string.setup_card_dxvk_config), null);

        LauncherUi.help(section.content, getString(R.string.setup_dxvk_config_help));

        dxvkConfigEdit = new EditText(this);
        dxvkConfigEdit.setTypeface(android.graphics.Typeface.MONOSPACE);
        dxvkConfigEdit.setTextSize(12);
        dxvkConfigEdit.setTextColor(LauncherUi.color(this, R.color.gzh_on_surface));
        dxvkConfigEdit.setMinLines(6);
        dxvkConfigEdit.setMaxLines(20);
        dxvkConfigEdit.setGravity(android.view.Gravity.TOP | android.view.Gravity.START);
        dxvkConfigEdit.setInputType(android.text.InputType.TYPE_CLASS_TEXT
            | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        section.content.addView(dxvkConfigEdit,
            LauncherUi.marginsBelow(this, LauncherUi.SPACE_2));

        LauncherUi.button(section.content, LauncherUi.BUTTON_OUTLINED,
            getString(R.string.setup_button_dxvk_config_save), this::onSaveDxvkConfig);
        LauncherUi.button(section.content, LauncherUi.BUTTON_TEXT,
            getString(R.string.setup_button_dxvk_config_reset), this::onResetDxvkConfig);

        loadDxvkConfigIntoEditor();
    }

    // Live copy the engine actually reads -- lives in the user-selected game
    // folder, same as DefaultOptions.ini and fonts/ (see
    // SetupActivity.copyBundledRuntimeIfMissing()).
    private File dxvkConfFile() {
        String gamePath = SetupActivity.getSavedGamePath(this);
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
                text = SetupActivity.readWholeFile(source);
            } catch (java.io.IOException e) {
                // Leave the editor empty; Save will still work and create a fresh file.
            }
        }
        dxvkConfigEdit.setText(text);
        boolean haveFolder = SetupActivity.getSavedGamePath(this) != null;
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
            text = SetupActivity.readWholeFile(bundled);
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

    // ----------------------------------------------------------- diagnostics

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
        "gx_trace.txt", "gx_perf.txt", "gx_touch_debug.txt", "dxvk_hud.txt", "dxvk_validation.txt",
        "dxvk_verbose_log.txt"
    };
    private static final int[] DIAGNOSTIC_TITLES = {
        R.string.setup_switch_gx_trace, R.string.setup_switch_gx_perf, R.string.setup_switch_touch_debug,
        R.string.setup_switch_dxvk_hud, R.string.setup_switch_dxvk_validation,
        R.string.setup_switch_dxvk_verbose_log
    };
    private static final int[] DIAGNOSTIC_DESCRIPTIONS = {
        R.string.setup_switch_gx_trace_desc, R.string.setup_switch_gx_perf_desc,
        R.string.setup_switch_touch_debug_desc, R.string.setup_switch_dxvk_hud_desc,
        R.string.setup_switch_dxvk_validation_desc, R.string.setup_switch_dxvk_verbose_log_desc
    };
    private final MaterialSwitch[] diagnosticSwitches = new MaterialSwitch[DIAGNOSTIC_MARKERS.length];

    private void buildDiagnosticsSection(LinearLayout column) {
        LinearLayout content = LauncherUi.card(column, null);

        LauncherUi.help(content, getString(R.string.setup_diagnostics_help));

        diagnosticsNoFolderHint = LauncherUi.text(content,
            getString(R.string.setup_diagnostics_no_folder), R.style.Gzh_Text_Body,
            R.color.gzh_status_warn, LauncherUi.SPACE_2);

        for (int i = 0; i < DIAGNOSTIC_MARKERS.length; i++) {
            // GeneralsX @feature Android port launcher-ui-refresh 06/09/2026
            // Title and description are one tappable block now rather than a
            // switch with a caption floating under it: the whole row toggles,
            // so the target is the full width of the card instead of the
            // switch thumb, and the description can no longer look like it
            // belongs to the switch below it.
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);
            row.setMinimumHeight(LauncherUi.dp(this, LauncherUi.TOUCH_TARGET));
            int vPad = LauncherUi.dp(this, LauncherUi.SPACE_2);
            row.setPadding(0, vPad, 0, vPad);

            LinearLayout labels = new LinearLayout(this);
            labels.setOrientation(LinearLayout.VERTICAL);
            row.addView(labels, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

            LauncherUi.text(labels, getString(DIAGNOSTIC_TITLES[i]), R.style.Gzh_Text_Body,
                R.color.gzh_on_surface, 0);
            LauncherUi.text(labels, getString(DIAGNOSTIC_DESCRIPTIONS[i]), R.style.Gzh_Text_BodySmall,
                R.color.gzh_on_surface_variant, 0);

            MaterialSwitch sw = new MaterialSwitch(this);
            LinearLayout.LayoutParams swLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            swLp.setMarginStart(LauncherUi.dp(this, LauncherUi.SPACE_3));
            row.addView(sw, swLp);
            diagnosticSwitches[i] = sw;

            // Disabled (no game folder picked yet) means the row is inert
            // too, not just the thumb -- otherwise tapping the description
            // silently does nothing and reads as a bug.
            row.setOnClickListener(v -> {
                if (sw.isEnabled()) {
                    sw.toggle();
                }
            });
            content.addView(row, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        }

        refreshDiagnosticsSwitches();
    }

    private File diagnosticMarkerFile(String name) {
        String gamePath = SetupActivity.getSavedGamePath(this);
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
        boolean haveFolder = SetupActivity.getSavedGamePath(this) != null;
        if (diagnosticsNoFolderHint != null) {
            diagnosticsNoFolderHint.setVisibility(haveFolder ? View.GONE : View.VISIBLE);
        }
        for (int i = 0; i < DIAGNOSTIC_MARKERS.length; i++) {
            final int index = i;
            MaterialSwitch sw = diagnosticSwitches[index];
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

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_IMPORT_DRIVER && resultCode == Activity.RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                importCustomDriver(uri);
            }
        }
    }
}
