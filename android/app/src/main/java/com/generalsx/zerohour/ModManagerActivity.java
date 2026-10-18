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
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.widget.SwitchCompat;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * GeneralsX @feature Android port 18/10/2026 The Mods screen (PLAN-027 step 1): the mods of the
 * game picked on Home, each a folder under mods/, with an on/off switch, an order (top wins), its
 * picture and description from the folder, a file check, deletion, and import of a folder, a
 * .big or a .zip. The game is started from Home as always; the engine gets the enabled list at
 * launch (ModManager.writeActiveList). The importer and the file check come from the GeneralsX
 * fork by realkot, used with permission.
 */
public class ModManagerActivity extends Activity {
    private static final int REQUEST_IMPORT_FOLDER = 2101;
    private static final int REQUEST_IMPORT_FILE = 2102;

    private final ExecutorService worker = Executors.newSingleThreadExecutor(
        task -> new Thread(task, "GeneralsX-Mods"));
    private final Map<String, Long> sizes = new HashMap<>();
    private LinearLayout list;
    private List<ModManager.Mod> mods = new ArrayList<>();
    private boolean busy;
    private AlertDialog importProgress;
    private ModImportService.CancellationSignal importCancellation;
    private Thread importThread;

    @Override
    protected void attachBaseContext(Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        ThemeHelper.apply(this);
        super.onCreate(savedInstanceState);
        ThemeHelper.applyBars(this);
        setTitle(R.string.mods_title);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(UiKit.color(this, R.color.gzh_background));
        UiKit.appBar(root, getString(SetupActivity.isGeneralsSelected(this)
                ? R.string.mods_overline_generals : R.string.mods_overline_zh),
            getString(R.string.mods_title), R.drawable.ic_gzh_download,
            getString(R.string.mods_import_title), this::showImportChoices);
        LinearLayout page = UiKit.scrollingPage(root);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        page.addView(list, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(root);
        InsetUtil.applySafeInsets(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (importThread == null) {
            refresh();
        }
    }

    @Override
    protected void onDestroy() {
        if (importCancellation != null) {
            importCancellation.cancel();
        }
        if (importThread != null) {
            importThread.interrupt();
        }
        if (importProgress != null) {
            importProgress.dismiss();
        }
        worker.shutdownNow();
        super.onDestroy();
    }

    // ------------------------------------------------------------------ list

    private void refresh() {
        mods = ModManager.listMods(this);
        render();
        final List<ModManager.Mod> measuring = new ArrayList<>(mods);
        worker.execute(() -> {
            Map<String, Long> measured = new HashMap<>();
            for (ModManager.Mod mod : measuring) {
                if (Thread.currentThread().isInterrupted()) {
                    return;
                }
                measured.put(mod.dir.getAbsolutePath(), ModLibraryInfo.measure(mod.dir));
            }
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) {
                    return;
                }
                sizes.putAll(measured);
                render();
            });
        });
    }

    private void render() {
        list.removeAllViews();
        if (ModManager.gameRoot(this) == null) {
            LinearLayout card = UiKit.card(list);
            UiKit.sectionHeader(card, R.drawable.ic_gzh_folder, getString(R.string.mods_no_game_title), false);
            UiKit.supporting(card, getString(R.string.mods_no_game_message));
            return;
        }

        LinearLayout intro = UiKit.card(list);
        UiKit.sectionHeader(intro, R.drawable.ic_gzh_sliders, getString(R.string.mods_order_title), false);
        UiKit.supporting(intro, getString(R.string.mods_order_help));
        boolean anyEnabled = false;
        for (ModManager.Mod mod : mods) {
            anyEnabled |= mod.enabled;
        }
        if (anyEnabled) {
            UiKit.chip(intro, R.drawable.ic_gzh_info, getString(R.string.mods_online_note),
                R.color.gzh_status_warn, R.color.gzh_surface_container_high);
        }

        if (mods.isEmpty()) {
            LinearLayout empty = UiKit.card(list);
            UiKit.sectionHeader(empty, R.drawable.ic_gzh_folder, getString(R.string.mods_empty_title), false);
            UiKit.supporting(empty, getString(R.string.mods_library_empty));
        }
        for (int i = 0; i < mods.size(); i++) {
            renderMod(i);
        }

        List<String> legacy = ModManager.legacyArchives(this);
        if (!legacy.isEmpty()) {
            LinearLayout card = UiKit.card(list);
            UiKit.sectionHeader(card, R.drawable.ic_gzh_info, getString(R.string.mods_legacy_title), false);
            UiKit.body(card, TextUtils.join("\n", legacy));
            UiKit.supporting(card, getString(R.string.mods_legacy_help));
        }

        LinearLayout actions = UiKit.card(list);
        UiKit.button(actions, UiKit.BTN_TONAL, R.drawable.ic_gzh_download,
            getString(R.string.mods_import_title), this::showImportChoices).setEnabled(!busy);
        File root = ModManager.modsRoot(this);
        UiKit.helpText(actions, getString(R.string.mods_folder_help,
            root == null ? "" : root.getAbsolutePath()));
    }

    private void renderMod(int index) {
        final ModManager.Mod mod = mods.get(index);
        LinearLayout card = UiKit.card(list);

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        card.addView(head, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        head.addView(picture(mod));

        LinearLayout labels = new LinearLayout(this);
        labels.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(0,
            LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        labelParams.setMarginStart(UiKit.dp(this, 12));
        head.addView(labels, labelParams);
        TextView name = UiKit.body(labels, mod.name);
        name.setTypeface(Typeface.DEFAULT_BOLD);
        name.setMaxLines(2);
        name.setEllipsize(TextUtils.TruncateAt.END);
        UiKit.caption(labels, meta(mod));

        SwitchCompat toggle = new SwitchCompat(this);
        toggle.setChecked(mod.enabled);
        toggle.setContentDescription(mod.name);
        toggle.setEnabled(!busy);
        toggle.setOnCheckedChangeListener((button, checked) -> {
            mod.enabled = checked;
            ModManager.saveOrder(this, mods);
            render();
        });
        head.addView(toggle);

        if (!mod.description.isEmpty()) {
            TextView description = UiKit.supporting(card, mod.description);
            description.setMaxLines(4);
            description.setEllipsize(TextUtils.TruncateAt.END);
        }

        LinearLayout tools = new LinearLayout(this);
        tools.setOrientation(LinearLayout.HORIZONTAL);
        tools.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams toolParams = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        toolParams.topMargin = UiKit.dp(this, 8);
        card.addView(tools, toolParams);
        View up = UiKit.iconButton(this, R.drawable.ic_gzh_chevron, getString(R.string.mods_move_up),
            () -> move(index, -1));
        up.setRotation(-90f);
        up.setEnabled(!busy && index > 0);
        up.setAlpha(up.isEnabled() ? 1f : 0.35f);
        tools.addView(up);
        View down = UiKit.iconButton(this, R.drawable.ic_gzh_chevron, getString(R.string.mods_move_down),
            () -> move(index, 1));
        down.setRotation(90f);
        down.setEnabled(!busy && index < mods.size() - 1);
        down.setAlpha(down.isEnabled() ? 1f : 0.35f);
        tools.addView(down);
        tools.addView(UiKit.iconButton(this, R.drawable.ic_gzh_info, getString(R.string.mods_details_button),
            () -> showDetails(mod)));
        tools.addView(UiKit.iconButton(this, R.drawable.ic_gzh_trash, getString(R.string.mods_delete_button),
            () -> confirmDelete(mod)));
    }

    private String meta(ModManager.Mod mod) {
        List<String> parts = new ArrayList<>();
        if (!mod.version.isEmpty()) {
            parts.add(mod.version);
        }
        if (!mod.author.isEmpty()) {
            parts.add(mod.author);
        }
        Long bytes = sizes.get(mod.dir.getAbsolutePath());
        if (bytes != null && bytes >= 0) {
            parts.add(humanBytes(bytes));
        }
        parts.add(getString(mod.enabled ? R.string.mods_state_on : R.string.mods_state_off));
        return TextUtils.join("  ·  ", parts);
    }

    /** The mod's own picture (mod.ini Image=, cover/preview/logo/icon), else its initials. */
    private View picture(ModManager.Mod mod) {
        int size = UiKit.dp(this, 56);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(UiKit.dp(this, 12));
        bg.setColor(UiKit.color(this, R.color.gzh_surface_container_high));
        Bitmap bitmap = mod.picture == null ? null : decode(mod.picture, size);
        if (bitmap != null) {
            ImageView image = new ImageView(this);
            image.setImageBitmap(bitmap);
            image.setScaleType(ImageView.ScaleType.CENTER_CROP);
            image.setBackground(bg);
            image.setClipToOutline(true);
            image.setLayoutParams(new LinearLayout.LayoutParams(size, size));
            return image;
        }
        TextView initials = new TextView(this);
        initials.setText(initials(mod.name));
        initials.setGravity(Gravity.CENTER);
        initials.setTypeface(Typeface.DEFAULT_BOLD);
        initials.setTextSize(18f);
        initials.setTextColor(UiKit.color(this, R.color.gzh_primary));
        initials.setBackground(bg);
        initials.setLayoutParams(new LinearLayout.LayoutParams(size, size));
        return initials;
    }

    private static Bitmap decode(File file, int size) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            return null;
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = 1;
        while (Math.min(bounds.outWidth, bounds.outHeight) / (options.inSampleSize * 2) >= size) {
            options.inSampleSize *= 2;
        }
        return BitmapFactory.decodeFile(file.getAbsolutePath(), options);
    }

    private static String initials(String name) {
        StringBuilder sb = new StringBuilder();
        for (String word : name.trim().split("[\\s_\\-]+")) {
            if (!word.isEmpty() && sb.length() < 2) {
                sb.appendCodePoint(Character.toUpperCase(word.codePointAt(0)));
            }
        }
        return sb.length() == 0 ? "?" : sb.toString();
    }

    private void move(int index, int delta) {
        int target = index + delta;
        if (busy || target < 0 || target >= mods.size()) {
            return;
        }
        ModManager.Mod mod = mods.remove(index);
        mods.add(target, mod);
        ModManager.saveOrder(this, mods);
        render();
    }

    // --------------------------------------------------------- details, delete

    private void showDetails(ModManager.Mod mod) {
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        int pad = UiKit.dp(this, 20);
        body.setPadding(pad, UiKit.dp(this, 8), pad, 0);
        if (!mod.description.isEmpty()) {
            UiKit.supporting(body, mod.description);
        }
        TextView report = UiKit.body(body, getString(R.string.mods_health_scanning));
        report.setTextIsSelectable(true);
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle(mod.name)
            .setView(scroll)
            .setPositiveButton(android.R.string.ok, null)
            .show();
        worker.execute(() -> {
            ModHealthCheck.Result r = ModHealthCheck.scan(mod.dir);
            runOnUiThread(() -> {
                if (!dialog.isShowing()) {
                    return;
                }
                report.setText(healthText(r));
            });
        });
    }

    private String healthText(ModHealthCheck.Result report) {
        StringBuilder summary = new StringBuilder();
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
            getString(report.dataFolder ? R.string.mods_health_present : R.string.mods_health_absent)));
        summary.append("\n").append(getString(R.string.mods_health_window_folder,
            getString(report.windowFolder ? R.string.mods_health_present : R.string.mods_health_absent)));
        if (report.symlinks > 0 || report.unreadable > 0) {
            summary.append("\n").append(getString(R.string.mods_health_fs_issues, report.symlinks, report.unreadable));
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
        return summary.toString();
    }

    private void confirmDelete(ModManager.Mod mod) {
        if (busy) {
            return;
        }
        new AlertDialog.Builder(this)
            .setTitle(R.string.mods_delete_confirm_title)
            .setMessage(getString(R.string.mods_delete_confirm, mod.name))
            .setPositiveButton(R.string.mods_delete_button, (d, w) -> {
                busy = true;
                render();
                worker.execute(() -> {
                    boolean deleted = ModManager.deleteMod(this, mod.dir);
                    runOnUiThread(() -> {
                        if (isFinishing() || isDestroyed()) {
                            return;
                        }
                        busy = false;
                        Toast.makeText(this, getString(deleted ? R.string.mods_delete_success
                            : R.string.mods_delete_failed, mod.name), Toast.LENGTH_LONG).show();
                        refresh();
                    });
                });
            })
            .setNegativeButton(android.R.string.cancel, null)
            .show();
    }

    // ------------------------------------------------------------------ import

    private void showImportChoices() {
        if (busy) {
            return;
        }
        if (ModManager.gameRoot(this) == null) {
            Toast.makeText(this, R.string.mods_no_game_message, Toast.LENGTH_LONG).show();
            return;
        }
        new AlertDialog.Builder(this).setTitle(R.string.mods_import_title)
            .setItems(new CharSequence[] { getString(R.string.mods_import_folder),
                getString(R.string.mods_import_file) }, (dialog, which) -> {
                    if (which == 0) {
                        pick(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_IMPORT_FOLDER);
                    } else {
                        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                        intent.addCategory(Intent.CATEGORY_OPENABLE);
                        intent.setType("*/*");
                        pick(intent, REQUEST_IMPORT_FILE);
                    }
                })
            .setNegativeButton(android.R.string.cancel, null)
            .show();
    }

    private void pick(Intent intent, int request) {
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(intent, request);
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
        beginImport(data.getData(), requestCode == REQUEST_IMPORT_FOLDER);
    }

    private void beginImport(Uri uri, boolean folder) {
        busy = true;
        render();
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        int pad = UiKit.dp(this, 20);
        body.setPadding(pad, UiKit.dp(this, 8), pad, 0);
        ProgressBar spinner = new ProgressBar(this);
        spinner.setIndeterminate(true);
        body.addView(spinner);
        TextView message = UiKit.supporting(body, getString(R.string.mods_import_working_message));
        importProgress = new AlertDialog.Builder(this)
            .setTitle(R.string.mods_import_working_title)
            .setView(body)
            .setNegativeButton(R.string.mods_import_cancel_action, null)
            .setCancelable(false)
            .show();
        importCancellation = new ModImportService.CancellationSignal();
        importProgress.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> {
            importCancellation.cancel();
            if (importThread != null) {
                importThread.interrupt();
            }
            message.setText(R.string.mods_import_cancelling);
            v.setEnabled(false);
        });
        final ModImportService.CancellationSignal cancellation = importCancellation;
        importThread = new Thread(() -> {
            try {
                ModImportService.Result result = folder
                    ? ModImportService.importTree(this, uri, cancellation)
                    : ModImportService.importDocument(this, uri, cancellation);
                runOnUiThread(() -> finishImport(result, null));
            } catch (ModImportService.ImportCancelledException e) {
                runOnUiThread(() -> finishImport(null, getString(R.string.mods_import_cancelled)));
            } catch (ModImportService.ImportProblem e) {
                runOnUiThread(() -> finishImport(null, getString(e.messageRes)));
            } catch (Exception e) {
                String detail = e.getMessage() == null || e.getMessage().trim().isEmpty()
                    ? e.getClass().getSimpleName() : e.getMessage();
                runOnUiThread(() -> finishImport(null, getString(R.string.mods_import_failed_message, detail)));
            }
        }, "GeneralsX-ModImport");
        importThread.start();
    }

    private void finishImport(ModImportService.Result result, String failure) {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        busy = false;
        importThread = null;
        importCancellation = null;
        if (importProgress != null) {
            importProgress.dismiss();
            importProgress = null;
        }
        refresh();
        if (result == null) {
            new AlertDialog.Builder(this)
                .setTitle(R.string.mods_import_failed_title)
                .setMessage(failure)
                .setPositiveButton(android.R.string.ok, null)
                .show();
            return;
        }
        new AlertDialog.Builder(this)
            .setTitle(R.string.mods_import_done_title)
            .setMessage(getString(R.string.mods_import_done_message, result.installed.getName(),
                result.bigCount, result.fileCount, humanBytes(result.bytes)))
            .setPositiveButton(R.string.mods_import_activate, (d, w) -> enableOnTop(result.installed))
            .setNegativeButton(R.string.mods_import_later, null)
            .show();
    }

    /** "Turn on now" after an import: the new mod switched on, at the top of the list. */
    private void enableOnTop(File dir) {
        for (int i = 0; i < mods.size(); i++) {
            ModManager.Mod mod = mods.get(i);
            if (mod.dir.getAbsolutePath().equals(dir.getAbsolutePath())) {
                mods.remove(i);
                mod.enabled = true;
                mods.add(0, mod);
                ModManager.saveOrder(this, mods);
                render();
                return;
            }
        }
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
