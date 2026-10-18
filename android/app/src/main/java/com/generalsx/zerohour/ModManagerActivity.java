/*
**  Command & Conquer Generals Zero Hour(tm)
**  Copyright 2025 Electronic Arts Inc.
**
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
*/

package com.generalsx.zerohour;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Toast;
import android.widget.TextView;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.view.View;
import android.view.ViewGroup;
import android.view.Gravity;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.content.res.ColorStateList;
import android.text.TextUtils;

import com.google.android.material.button.MaterialButton;

import java.io.File;
import java.util.List;
import java.util.Locale;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * GeneralsX @feature Android port 04/10/2026 Mod Manager.
 * GeneralsX @feature Android port 18/10/2026 Ported from realkot's GeneralsX fork with the
 * author's permission (https://github.com/realkot/GeneralsX); opened from Home, one library per
 * game.
 *
 * Phase 1 owns reversible activation. Phase 2 adds a transactional importer:
 * folder trees and .big/.zip documents are copied to a hidden staging area,
 * validated, then atomically renamed into Mods/. No import operation writes
 * into the live game root or an already-installed mod. Import can be cancelled
 * before commit, and installed entries can be deleted only through ModManager's
 * canonical managed-root check.
 */
public class ModManagerActivity extends Activity {
    private static final int REQUEST_IMPORT_FOLDER = 2101;
    private static final int REQUEST_IMPORT_FILE = 2102;
    // GeneralsX @feature Android port 18/10/2026 "Play" on a mod: Setup (which started this screen
    // for a result) launches the game with the mod just made active.
    static final int RESULT_LAUNCH = RESULT_FIRST_USER + 1;

    private boolean importRunning;
    private AlertDialog importProgress;
    private AlertDialog detailsDialog;
    private int detailsGeneration;
    private ModImportService.CancellationSignal importCancellation;
    private Thread importThread;

    // GeneralsX @feature Android port 04/10/2026 Tactical Views-only library.
    // Filesystem discovery and size accounting never run on the UI thread.
    // GeneralsX @feature 10/10/2026 No frozen dark-only palette on the mod screen.
    private int background() { return LauncherUi.background(this); }
    private int surface() { return LauncherUi.surface(this); }
    private int outline() { return LauncherUi.outline(this); }
    private int gold() { return LauncherUi.warning(this); }
    private int textColor() { return LauncherUi.textColor(this); }
    private int muted() { return LauncherUi.muted(this); }
    private final ExecutorService libraryWorker = Executors.newSingleThreadExecutor(
        task -> new Thread(task, "GeneralsX-ModLibrary"));
    private LinearLayout library;
    private TextView countValue, storageValue, freeValue, selection;
    private MaterialButton importButton, vanillaButton;
    private ProgressBar libraryProgress;
    private Snapshot snapshot;
    private int scanGeneration;
    private boolean actionRunning;
    private String libraryQuery = "";

    private static final class Entry {
        final File file;
        final long bytes;
        final boolean directory;
        Entry(File file, long bytes) {
            this.file = file;
            this.bytes = bytes;
            directory = file.isDirectory();
        }
    }

    private static final class Snapshot {
        File root, active;
        String legacy;
        boolean storageReady;
        long total, free;
        final List<Entry> entries = new ArrayList<>();
    }

    @Override
    protected void attachBaseContext(Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        ThemeHelper.apply(this);
        super.onCreate(savedInstanceState);
        setTitle(R.string.mods_title);
        if (savedInstanceState != null) libraryQuery = savedInstanceState.getString("mods_ui_query", "");
        buildUi();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putString("mods_ui_query", libraryQuery);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!importRunning) {
            // Re-read disk state if a file manager changed Mods/ while this
            // Activity was in the background.
            refreshLibrary();
        }
    }

    @Override
    protected void onDestroy() {
        if (importCancellation != null) importCancellation.cancel();
        if (importThread != null) importThread.interrupt();
        if (importProgress != null) {
            importProgress.dismiss();
            importProgress = null;
        }
        scanGeneration++;
        detailsGeneration++;
        if (detailsDialog != null) {
            detailsDialog.dismiss();
            detailsDialog = null;
        }
        libraryWorker.shutdownNow();
        super.onDestroy();
    }

    private void buildUi() {
        getWindow().setStatusBarColor(background());
        getWindow().setNavigationBarColor(background());
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);
        ThemeHelper.applyBars(this);
        LinearLayout shell = column();
        shell.setBackgroundColor(background());
        setContentView(shell);
        InsetUtil.applySafeInsets(shell);

        FrameLayout host = new FrameLayout(this);
        shell.addView(host, new LinearLayout.LayoutParams(-1, 0, 1f));
        LinearLayout page = UiKit.scrollingPage(host);
        page.setPadding(dp(16), dp(12), dp(16), dp(12));
        // GeneralsX @feature 10/10/2026 Premium Mod Library phase 1: compact hero, same original art.
        FrameLayout battlefield = LauncherUi.artwork(this, page, 122);
        ((ImageView) battlefield.getChildAt(0)).setImageResource(R.drawable.launcher_mods_battlefield);
        LinearLayout header = column();
        header.setPadding(dp(16), dp(12), dp(16), dp(12));
        header.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            new int[] {0xc6090d14, 0x33090d14, 0xf2090d14}));
        battlefield.addView(header, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout tools = new LinearLayout(this);
        tools.setGravity(Gravity.CENTER_VERTICAL);
        header.addView(tools, new LinearLayout.LayoutParams(-1, -2));
        TextView title = new TextView(this);
        title.setText(R.string.mods_title);
        title.setTextSize(27);
        title.setSingleLine(true);
        title.setEllipsize(TextUtils.TruncateAt.END);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(android.graphics.Color.WHITE);
        tools.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        tools.addView(UiKit.iconButton(this, android.R.drawable.ic_menu_search,
            getString(R.string.launcher_search_mods), this::searchLibrary));
        // Always white over the battlefield; light mode must not render dark ink on artwork.
        TextView subtitle = text(header, getString(R.string.mods_library_subtitle), 13,
            android.graphics.Color.WHITE, false);
        subtitle.setMaxLines(2);
        subtitle.setAlpha(0.94f);

        LinearLayout summary = panel(page, false);
        summary.setOrientation(LinearLayout.HORIZONTAL);
        summary.setPadding(dp(10), dp(12), dp(10), dp(12));
        countValue = metric(summary, R.string.mods_library_title);
        storageValue = metric(summary, R.string.mods_storage_used);
        freeValue = metric(summary, R.string.mods_free_space);
        // One clearly labelled active-mode strip, with full-name room and RTL-safe spacing.
        LinearLayout activeStrip = new LinearLayout(this);
        activeStrip.setGravity(Gravity.CENTER_VERTICAL);
        activeStrip.setPadding(dp(12), dp(10), dp(12), dp(10));
        activeStrip.setBackground(LauncherUi.shape(this,
            UiKit.color(this, R.color.gzh_surface_container_high),
            UiKit.color(this, R.color.gzh_outline_variant), 16));
        LinearLayout.LayoutParams activeParams = new LinearLayout.LayoutParams(-1, -2);
        activeParams.topMargin = dp(12);
        activeParams.bottomMargin = dp(4);
        page.addView(activeStrip, activeParams);
        ImageView activeIcon = new ImageView(this);
        activeIcon.setImageResource(R.drawable.ic_gzh_check);
        activeIcon.setImageTintList(UiKit.tint(this, R.color.gzh_primary));
        LinearLayout.LayoutParams activeIconParams = new LinearLayout.LayoutParams(dp(22), dp(22));
        activeIconParams.setMarginEnd(dp(10));
        activeStrip.addView(activeIcon, activeIconParams);
        LinearLayout activeLabels = column();
        activeStrip.addView(activeLabels, new LinearLayout.LayoutParams(0, -2, 1f));
        text(activeLabels, getString(R.string.mods_status_title), 11, muted(), false);
        selection = text(activeLabels, getString(R.string.mods_library_loading), 14, textColor(), true);
        // GeneralsX @feature Android port 18/10/2026 A mod changes the game data GeneralsOnline checks;
        // whether it allows modded games is its rule (docs/WORKDIR/planning/PLAN-027_MOD_SYSTEM.md).
        TextView onlineNote = text(page, getString(R.string.mods_online_note), 12, muted(), false);
        onlineNote.setPadding(dp(4), dp(6), dp(4), 0);
        libraryProgress = new ProgressBar(this);
        libraryProgress.setIndeterminate(true);
        libraryProgress.setIndeterminateTintList(ColorStateList.valueOf(gold()));
        page.addView(libraryProgress, new LinearLayout.LayoutParams(dp(28), dp(28)));
        library = column();
        page.addView(library, new LinearLayout.LayoutParams(-1, -2));

        LinearLayout footer = column();
        footer.setPadding(dp(16), dp(10), dp(16), dp(12));
        footer.setBackgroundColor(background());
        shell.addView(footer, new LinearLayout.LayoutParams(-1, -2));
        LinearLayout actions = UiKit.buttonRow(footer);
        importButton = action(actions, R.string.mods_import_title, R.drawable.ic_gzh_download,
            true, this::showImportChoices);
        vanillaButton = action(actions, R.string.mods_use_vanilla, R.drawable.ic_gzh_play,
            false, () -> selectMod(null, true));
        importButton.setEnabled(false);
        vanillaButton.setEnabled(false);
        LauncherUi.purple(this, importButton);
    }

    private void searchLibrary() {
        android.widget.EditText query = new android.widget.EditText(this);
        query.setSingleLine(true);
        query.setInputType(android.text.InputType.TYPE_CLASS_TEXT);
        query.setHint(R.string.launcher_search_mods);
        query.setText(libraryQuery);
        query.setSelectAllOnFocus(true);
        query.setPadding(dp(20), dp(10), dp(20), dp(10));
        new AlertDialog.Builder(this).setTitle(R.string.launcher_search_mods).setView(query)
            .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                libraryQuery = query.getText().toString().trim();
                renderLibrary();
            }).setNegativeButton(android.R.string.cancel, null).show();
    }

    private void refreshLibrary() {
        if (isFinishing() || isDestroyed() || libraryWorker.isShutdown()) return;
        final int generation = ++scanGeneration;
        libraryProgress.setVisibility(View.VISIBLE);
        libraryWorker.execute(() -> {
            Snapshot next = new Snapshot();
            File game = ModManager.gameRoot(this);
            next.root = ModManager.modsRoot(this);
            try {
                if (game != null && next.root != null) {
                    next.storageReady = next.root.isDirectory() || next.root.mkdirs();
                    if (next.storageReady) {
                        next.active = ModManager.getActiveMod(this);
                        next.legacy = DataPackInstaller.findDataMod(game.getAbsolutePath());
                        next.free = next.root.getUsableSpace();
                        for (File mod : ModManager.listMods(this)) {
                            if (Thread.currentThread().isInterrupted()) return;
                            if (!ModLibraryInfo.isManagedEntry(next.root, mod)) continue;
                            long bytes = ModLibraryInfo.measure(mod);
                            next.entries.add(new Entry(mod, bytes));
                            next.total = next.total < 0 || bytes < 0 ? -1 : next.total + bytes;
                        }
                    }
                }
            } catch (SecurityException e) {
                next.storageReady = false;
            }
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed() || generation != scanGeneration) return;
                snapshot = next;
                libraryProgress.setVisibility(View.GONE);
                renderLibrary();
            });
        });
    }

    private void renderLibrary() {
        if (snapshot == null) return;
        library.removeAllViews();
        countValue.setText(snapshot.storageReady
            ? String.format(Locale.getDefault(), "%d", snapshot.entries.size())
            : getString(R.string.mods_size_unknown));
        storageValue.setText(snapshot.storageReady ? sizeLabel(snapshot.total) : getString(R.string.mods_size_unknown));
        freeValue.setText(snapshot.storageReady ? sizeLabel(snapshot.free) : getString(R.string.mods_size_unknown));
        selection.setText(snapshot.active == null ? getString(R.string.mods_active_vanilla)
            : getString(R.string.mods_active_mod, snapshot.active.getName()));
        selection.setMaxLines(3);
        selection.setEllipsize(TextUtils.TruncateAt.END);
        boolean enabled = snapshot.storageReady && !importRunning && !actionRunning;
        importButton.setEnabled(enabled);
        vanillaButton.setEnabled(enabled);
        if (!snapshot.storageReady) {
            LinearLayout error = panel(library, false);
            text(error, getString(snapshot.root == null ? R.string.mods_no_game_title
                : R.string.mods_storage_error_title), 18, textColor(), true);
            text(error, snapshot.root == null ? getString(R.string.mods_no_game_message)
                : getString(R.string.mods_storage_error_message, snapshot.root.getAbsolutePath()), 14, muted(), false);
            return;
        }
        if (snapshot.legacy != null) {
            text(library, getString(R.string.mods_legacy_warning, snapshot.legacy), 13, gold(), false);
        }
        if (snapshot.entries.isEmpty()) {
            LinearLayout empty = panel(library, false);
            text(empty, getString(R.string.mods_empty_title), 20, textColor(), true);
            text(empty, getString(R.string.mods_library_empty), 14, muted(), false);
        }
        int shown = 0;
        List<Entry> ordered = new ArrayList<>(snapshot.entries);
        ordered.sort((a, b) -> Boolean.compare(b.file.equals(snapshot.active), a.file.equals(snapshot.active)));
        for (Entry entry : ordered) {
            if (!entry.file.getName().toLowerCase(Locale.ROOT).contains(libraryQuery.toLowerCase(Locale.ROOT))) continue;
            shown++;
            renderModCard(entry, entry.file.equals(snapshot.active), enabled);
        }
        if (shown == 0 && !snapshot.entries.isEmpty()) {
            text(library, getString(R.string.launcher_no_matches), 14, muted(), false);
        }
        TextView path = text(library, getString(R.string.mods_folder_path,
            snapshot.root.getAbsolutePath()), 11, muted(), false);
        path.setPadding(dp(2), dp(14), dp(2), dp(4));
    }

    /**
     * GeneralsX @feature 10/10/2026 Phase 1: consistent square thumbnail + full-width
     * actions. Presentation-only: delegates activate, launch and delete to the exact
     * same guarded methods as the original Mod Library. No mod file is moved here.
     */
    private void renderModCard(Entry entry, boolean active, boolean enabled) {
        LinearLayout card = panel(library, active);
        card.setPadding(dp(14), dp(14), dp(14), dp(12));

        LinearLayout heading = new LinearLayout(this);
        heading.setGravity(Gravity.TOP);
        heading.setOrientation(LinearLayout.HORIZONTAL);
        card.addView(heading, new LinearLayout.LayoutParams(-1, -2));

        // The shared battle image is deliberately not repeated as fake mod-specific artwork.
        ImageView preview = new ImageView(this);
        preview.setImageResource(R.drawable.ic_gzh_folder);
        preview.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
        preview.setImageTintList(UiKit.tint(this, R.color.gzh_primary));
        preview.setPadding(dp(17), dp(17), dp(17), dp(17));
        preview.setBackground(LauncherUi.shape(this,
            UiKit.color(this, R.color.gzh_surface_container_high),
            UiKit.color(this, R.color.gzh_outline_variant), 14));
        LinearLayout.LayoutParams thumbParams = new LinearLayout.LayoutParams(dp(72), dp(72));
        thumbParams.setMarginEnd(dp(12));
        heading.addView(preview, thumbParams);

        LinearLayout info = column();
        heading.addView(info, new LinearLayout.LayoutParams(0, -2, 1f));
        // Keep the established title-row view hierarchy for existing UI action tests.
        LinearLayout titleRow = new LinearLayout(this);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);
        info.addView(titleRow, new LinearLayout.LayoutParams(-1, -2));
        TextView name = text(titleRow, entry.file.getName(), 16, textColor(), true);
        name.setMaxLines(2);
        name.setEllipsize(TextUtils.TruncateAt.END);
        TextView meta = text(info, sizeLabel(entry.bytes) + "  ·  " + getString(entry.directory
            ? R.string.mods_type_folder : R.string.mods_type_big), 12, muted(), false);
        meta.setPadding(0, dp(5), 0, 0);
        meta.setMaxLines(2);

        TextView badge = text(info, getString(active ? R.string.mods_active_button
            : R.string.mods_installed_badge), 11,
            active ? UiKit.color(this, R.color.gzh_on_primary_container)
                : UiKit.color(this, R.color.gzh_on_surface_variant), true);
        badge.setPadding(dp(10), dp(5), dp(10), dp(5));
        badge.setBackground(LauncherUi.shape(this,
            active ? UiKit.color(this, R.color.gzh_primary_container)
                : UiKit.color(this, R.color.gzh_surface_container_high),
            active ? UiKit.color(this, R.color.gzh_primary)
                : UiKit.color(this, R.color.gzh_outline_variant), 18));
        LinearLayout.LayoutParams badgeParams = (LinearLayout.LayoutParams) badge.getLayoutParams();
        badgeParams.width = -2;
        badgeParams.topMargin = dp(8);
        badge.setLayoutParams(badgeParams);

        // Destructive action is separated from the title and retains confirmation.
        View delete = UiKit.iconButton(this, android.R.drawable.ic_menu_delete,
            getString(R.string.mods_delete_button), () -> confirmDelete(entry.file, active));
        delete.setLayoutParams(new LinearLayout.LayoutParams(dp(48), dp(48)));
        ((ImageView) delete).setImageTintList(UiKit.tint(this, R.color.gzh_on_surface_variant));
        delete.setEnabled(enabled);
        heading.addView(delete);

        View divider = new View(this);
        divider.setBackgroundColor(UiKit.color(this, R.color.gzh_outline_variant));
        LinearLayout.LayoutParams dividerParams = new LinearLayout.LayoutParams(-1, Math.max(1, dp(1)));
        dividerParams.topMargin = dp(12);
        dividerParams.bottomMargin = dp(10);
        card.addView(divider, dividerParams);

        // Full card width for actions, including long translated labels on narrow phones.
        LinearLayout actions = UiKit.buttonRow(card);
        MaterialButton launch = action(actions, R.string.mods_launch_button,
            R.drawable.ic_gzh_play, true, () -> selectMod(entry.file, true));
        MaterialButton activate = action(actions, R.string.mods_activate_button,
            R.drawable.ic_gzh_refresh, false, () -> selectMod(entry.file, false));
        MaterialButton details = action(actions, R.string.mods_details_button,
            R.drawable.ic_gzh_info, false, () -> showModDetails(entry));
        launch.setEnabled(enabled);
        activate.setEnabled(enabled && !active);
        details.setEnabled(enabled);
    }

    /**
     * GeneralsX @feature 10/10/2026 Read-only Mod Health Check.
     * UI delegates to the existing worker; all file and BIG index reads stay off
     * the main thread. This is not a gameplay/rendering validation result.
     */
    private void showModDetails(Entry entry) {
        if (entry == null || snapshot == null || !snapshot.storageReady || importRunning || actionRunning) return;
        File root = snapshot.root;
        if (!ModLibraryInfo.isManagedEntry(root, entry.file)) return;
        ++detailsGeneration;
        final int generation = detailsGeneration;
        if (detailsDialog != null) detailsDialog.dismiss();

        LinearLayout body = column();
        body.setPadding(dp(16), dp(14), dp(16), dp(12));
        TextView name = text(body, entry.file.getName(), 18, textColor(), true);
        name.setMaxLines(3);
        name.setEllipsize(TextUtils.TruncateAt.END);
        text(body, getString(entry.directory ? R.string.mods_type_folder : R.string.mods_type_big)
                + " • " + sizeLabel(entry.bytes), 12, muted(), false);
        TextView scan = text(body, getString(R.string.mods_health_scanning), 14, muted(), false);
        scan.setPadding(0, dp(14), 0, 0);
        scan.setMaxLines(20);
        scan.setEllipsize(null);

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.setFillViewport(false);
        scroll.addView(body, new android.widget.ScrollView.LayoutParams(-1, -2));
        detailsDialog = new AlertDialog.Builder(this)
            .setTitle(R.string.mods_details_title)
            .setView(scroll)
            .setPositiveButton(android.R.string.ok, null)
            .create();
        detailsDialog.setOnDismissListener(d -> {
            // Old scan results cannot repaint a dismissed or replaced details dialog.
            if (generation == detailsGeneration) {
                ++detailsGeneration;
                detailsDialog = null;
            }
        });
        detailsDialog.show();

        libraryWorker.execute(() -> {
            // Verify again on the worker. A symlink or file-manager replacement must
            // never make the scanner inspect a path outside the managed Mods root.
            final ModHealthCheck.Result report = ModLibraryInfo.isManagedEntry(root, entry.file)
                ? ModHealthCheck.scan(entry.file) : null;
            runOnUiThread(() -> {
                if (generation != detailsGeneration || isFinishing() || isDestroyed()
                        || detailsDialog == null || !detailsDialog.isShowing()) return;
                if (report == null) {
                    scan.setText(R.string.mods_health_unavailable);
                    return;
                }
                final StringBuilder summary = new StringBuilder();
                if (report.invalidArchives > 0 || report.unreadable > 0 || report.symlinks > 0) {
                    summary.append(getString(R.string.mods_health_warning));
                } else if (report.truncated || report.cancelled) {
                    summary.append(getString(R.string.mods_health_incomplete));
                } else if (report.archives == 0) {
                    summary.append(getString(R.string.mods_health_no_big));
                } else {
                    summary.append(getString(R.string.mods_health_structural_ok));
                }
                summary.append("\n\n").append(getString(R.string.mods_health_big_count, report.archives));
                summary.append("\n").append(getString(R.string.mods_health_valid_count, report.validArchives));
                summary.append("\n").append(getString(R.string.mods_health_invalid_count, report.invalidArchives));
                summary.append("\n").append(getString(R.string.mods_health_entry_count, report.archivedEntries));
                summary.append("\n").append(getString(R.string.mods_health_loose_count, report.looseFiles));
                summary.append("\n").append(getString(R.string.mods_health_nested_count, report.nestedArchives));
                summary.append("\n").append(getString(R.string.mods_health_data_folder,
                    report.dataFolder ? getString(R.string.mods_health_present) : getString(R.string.mods_health_absent)));
                summary.append("\n").append(getString(R.string.mods_health_window_folder,
                    report.windowFolder ? getString(R.string.mods_health_present) : getString(R.string.mods_health_absent)));
                if (report.symlinks > 0 || report.unreadable > 0) {
                    summary.append("\n").append(getString(R.string.mods_health_fs_issues,
                        report.symlinks, report.unreadable));
                }
                if (report.nestedArchives > 0) {
                    summary.append("\n\n").append(getString(R.string.mods_health_nested_warning));
                }
                if (!report.issues().isEmpty()) {
                    summary.append("\n\n").append(getString(R.string.mods_health_invalid_files));
                    for (String path : report.issues()) {
                        summary.append("\n• ").append(path);
                    }
                }
                summary.append("\n\n").append(getString(R.string.mods_health_disclaimer));
                scan.setText(summary.toString());
            });
        });
    }

    private void selectMod(File mod, boolean launch) {
        if (importRunning || actionRunning || snapshot == null || !snapshot.storageReady) return;
        actionRunning = true;
        renderLibrary();
        libraryWorker.execute(() -> {
            boolean success = ModManager.setActiveMod(this, mod);
            File active = ModManager.getActiveMod(this);
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                actionRunning = false;
                snapshot.active = active;
                renderLibrary();
                if (!success) {
                    Toast.makeText(this, R.string.mods_toast_activate_failed, Toast.LENGTH_LONG).show();
                } else if (launch) {
                    // Setup launches: its existing orientation/launch path, and GeneralsZHActivity
                    // adds -mod. Never construct -mod arguments here.
                    setResult(RESULT_LAUNCH);
                    finish();
                } else {
                    Toast.makeText(this, mod == null ? getString(R.string.mods_toast_vanilla)
                        : getString(R.string.mods_toast_activated, mod.getName()), Toast.LENGTH_SHORT).show();
                }
            });
        });
    }

    // GeneralsX @feature Android port 04/10/2026 Confirmed, managed-root-only deletion.
    private void confirmDelete(File mod, boolean active) {
        if (importRunning || actionRunning || snapshot == null || !snapshot.storageReady) return;
        new AlertDialog.Builder(this)
            .setTitle(R.string.mods_delete_confirm_title)
            .setMessage(getString(active ? R.string.mods_delete_active_confirm
                : R.string.mods_delete_confirm, mod.getName()))
            .setPositiveButton(R.string.mods_delete_button, (dialog, which) -> deleteMod(mod))
            .setNegativeButton(android.R.string.cancel, null)
            .show();
    }

    private void deleteMod(File mod) {
        actionRunning = true;
        renderLibrary();
        libraryWorker.execute(() -> {
            boolean deleted = ModManager.deleteManagedMod(this, mod);
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                actionRunning = false;
                Toast.makeText(this, getString(deleted ? R.string.mods_delete_success
                    : R.string.mods_delete_failed, mod.getName()), Toast.LENGTH_LONG).show();
                refreshLibrary();
            });
        });
    }

    private void showImportChoices() {
        if (importRunning || actionRunning) return;
        new AlertDialog.Builder(this).setTitle(R.string.mods_import_title)
            .setItems(new CharSequence[] { getString(R.string.mods_import_folder),
                getString(R.string.mods_import_file) }, (dialog, which) -> {
                    if (which == 0) pickModFolder(); else pickModFile();
                }).setNegativeButton(android.R.string.cancel, null).show();
    }

    private LinearLayout column() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        return column;
    }

    private int dp(int value) { return UiKit.dp(this, value); }

    private GradientDrawable shape(int fill, int stroke) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(fill);
        shape.setCornerRadius(dp(16));
        shape.setStroke(dp(1), stroke);
        return shape;
    }

    private LinearLayout panel(LinearLayout parent, boolean active) {
        LinearLayout panel = column();
        panel.setPadding(dp(14), dp(14), dp(14), dp(14));
        panel.setBackground(shape(active ? UiKit.color(this, R.color.gzh_primary_container) : surface(),
            active ? UiKit.color(this, R.color.gzh_primary) : outline()));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(12);
        parent.addView(panel, lp);
        return panel;
    }

    private TextView text(LinearLayout parent, CharSequence value, int sp, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sp);
        view.setTextColor(color);
        if (bold) view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setLineSpacing(0, 1.15f);
        parent.addView(view, new LinearLayout.LayoutParams(-1, -2));
        return view;
    }

    private TextView metric(LinearLayout parent, int label) {
        LinearLayout cell = column();
        cell.setPadding(dp(6), 0, dp(6), 0);
        parent.addView(cell, new LinearLayout.LayoutParams(0, -2, 1f));
        TextView value = text(cell, getString(R.string.mods_size_unknown), 17, textColor(), true);
        TextView caption = text(cell, getString(label), 11, muted(), false);
        caption.setMaxLines(2);
        return value;
    }

    private MaterialButton action(LinearLayout row, int label, int icon, boolean primary, Runnable run) {
        boolean first = row.getChildCount() == 0;
        MaterialButton button = UiKit.button(row, UiKit.BTN_TONAL, icon, getString(label), run);
        UiKit.share(button, first);
        button.setCornerRadius(dp(14));
        button.setTextSize(13);
        button.setStrokeWidth(dp(1));
        button.setStrokeColor(ColorStateList.valueOf(primary ? UiKit.color(this, R.color.gzh_primary) : outline()));
        int foreground = primary ? UiKit.color(this, R.color.gzh_on_primary) : textColor();
        button.setTextColor(new ColorStateList(new int[][] { {-android.R.attr.state_enabled}, {} },
            new int[] { muted(), foreground }));
        button.setIconTint(button.getTextColors());
        button.setBackgroundTintList(new ColorStateList(new int[][] { {-android.R.attr.state_enabled}, {} },
            new int[] { surface(), primary ? UiKit.color(this, R.color.gzh_primary)
                : UiKit.color(this, R.color.gzh_surface_container_high) }));
        return button;
    }

    private String sizeLabel(long bytes) {
        return bytes < 0 ? getString(R.string.mods_size_unknown) : humanBytes(bytes);
    }

    private void pickModFolder() {
        if (importRunning) {
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_IMPORT_FOLDER);
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, R.string.mods_import_picker_missing, Toast.LENGTH_LONG).show();
        }
    }

    private void pickModFile() {
        if (importRunning) {
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_IMPORT_FILE);
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, R.string.mods_import_picker_missing, Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if ((requestCode != REQUEST_IMPORT_FOLDER && requestCode != REQUEST_IMPORT_FILE)
                || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }

        Uri uri = data.getData();
        try {
            int takeFlags = data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION;
            if (takeFlags != 0) {
                // The nonzero masked value above is exactly this same read-only flag.
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            }
        } catch (SecurityException ignored) {
            // The grant is only needed while we copy. Some providers do not
            // offer persistable grants even when the picker intent requested one.
        }

        beginImport(uri, requestCode == REQUEST_IMPORT_FOLDER);
    }

    private void beginImport(Uri uri, boolean folder) {
        if (importRunning) {
            return;
        }
        importRunning = true;
        renderLibrary();
        LinearLayout progressSurface = column();
        progressSurface.setPadding(dp(24), dp(12), dp(24), dp(20));
        ProgressBar spinner = new ProgressBar(this);
        spinner.setIndeterminate(true);
        spinner.setIndeterminateTintList(ColorStateList.valueOf(gold()));
        LinearLayout.LayoutParams spinnerParams = new LinearLayout.LayoutParams(dp(48), dp(48));
        spinnerParams.gravity = Gravity.CENTER_HORIZONTAL;
        progressSurface.addView(spinner, spinnerParams);
        TextView progressMessage = text(progressSurface, getString(R.string.mods_import_working_message), 14, textColor(), false);
        progressMessage.setPadding(0, dp(18), 0, 0);
        importProgress = new AlertDialog.Builder(this)
            .setTitle(R.string.mods_import_working_title)
            .setView(progressSurface)
            .setNegativeButton(R.string.mods_import_cancel_action, null)
            .setCancelable(false)
            .create();
        importProgress.show();

        importCancellation = new ModImportService.CancellationSignal();
        importProgress.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> {
            if (importCancellation == null || importCancellation.isCancelled()) return;
            importCancellation.cancel();
            if (importThread != null) importThread.interrupt();
            progressMessage.setText(R.string.mods_import_cancelling);
            v.setEnabled(false);
        });

        importThread = new Thread(() -> {
            try {
                ModImportService.Result result = folder
                    ? ModImportService.importTree(this, uri, importCancellation)
                    : ModImportService.importDocument(this, uri, importCancellation);
                runOnUiThread(() -> finishImportSuccess(result));
            } catch (ModImportService.ImportCancelledException e) {
                runOnUiThread(this::finishImportCancelled);
            } catch (Exception e) {
                String message = e.getMessage();
                if (message == null || message.trim().isEmpty()) {
                    message = e.getClass().getSimpleName();
                }
                final String detail = message;
                runOnUiThread(() -> finishImportFailure(detail));
            }
        }, "GeneralsX-ModImport");
        importThread.start();
    }

    private void dismissImportProgress() {
        importRunning = false;
        importCancellation = null;
        importThread = null;
        if (importProgress != null) {
            importProgress.dismiss();
            importProgress = null;
        }
    }

    private void finishImportCancelled() {
        if (isFinishing() || isDestroyed()) return;
        dismissImportProgress();
        renderLibrary();
        Toast.makeText(this, R.string.mods_import_cancelled, Toast.LENGTH_LONG).show();
        refreshLibrary();
    }

    private void finishImportSuccess(ModImportService.Result result) {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        dismissImportProgress();
        refreshLibrary();

        String size = humanBytes(result.bytes);
        new AlertDialog.Builder(this)
            .setTitle(R.string.mods_import_done_title)
            .setMessage(getString(R.string.mods_import_done_message,
                result.installed.getName(), result.bigCount, result.fileCount, size))
            .setPositiveButton(R.string.mods_import_activate, (dialog, which) -> {
                if (ModManager.setActiveMod(this, result.installed)) {
                    Toast.makeText(this,
                        getString(R.string.mods_toast_activated, result.installed.getName()),
                        Toast.LENGTH_SHORT).show();
                }
                refreshLibrary();
            })
            .setNegativeButton(R.string.mods_import_later, null)
            .show();
    }

    private void finishImportFailure(String detail) {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        dismissImportProgress();
        renderLibrary();
        new AlertDialog.Builder(this)
            .setTitle(R.string.mods_import_failed_title)
            .setMessage(getString(R.string.mods_import_failed_message, detail))
            .setPositiveButton(android.R.string.ok, null)
            .show();
    }

    private static String humanBytes(long bytes) {
        if (bytes < 1024) {
            return bytes + " B";
        }
        double value = bytes;
        String[] units = { "KB", "MB", "GB", "TB" };
        int unit = -1;
        do {
            value /= 1024.0;
            unit++;
        } while (value >= 1024.0 && unit < units.length - 1);
        return String.format(Locale.US, "%.1f %s", value, units[unit]);
    }
}
