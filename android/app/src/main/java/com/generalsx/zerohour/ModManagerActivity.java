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
import android.text.Editable;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.widget.SwitchCompat;
import androidx.core.content.FileProvider;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * GeneralsX @feature Android port 18/10/2026 The Mods screen (PLAN-027): the mods of the game
 * picked on Home, each a folder under mods/, with an on/off switch, an order (top wins), the
 * mod's picture, kind and game, and what it does with the other mods switched on -- red (will not
 * start together), amber (changes the same game rules), grey (replaces the same art). A page per
 * mod (screenshots, readme, file groups, file check, share, delete), filters, a search, the old
 * '!' archives with a button to move one into mods/, and import of a folder, a .big, a .zip or a
 * shared mod set. The game is started from Home as always; the engine gets the list at launch
 * (ModManager.writeActiveList). The importer and the file check come from the GeneralsX fork by
 * realkot, used with permission.
 */
public class ModManagerActivity extends Activity {
    private static final int REQUEST_IMPORT_FOLDER = 2101;
    private static final int REQUEST_IMPORT_FILE = 2102;

    private final ExecutorService worker = Executors.newSingleThreadExecutor(
        task -> new Thread(task, "GeneralsX-Mods"));
    private final Map<String, Long> sizes = new HashMap<>();
    private final Map<String, ModAnalysis.Info> infos = new HashMap<>();
    private List<ModAnalysis.Problem> problems = new ArrayList<>();
    private boolean analyzed;
    private int analysisGeneration;
    private LinearLayout list;
    private List<ModManager.Mod> mods = new ArrayList<>();
    private boolean busy;
    private boolean onlyEnabled;
    private String query = "";
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

    // -------------------------------------------------------------- analysis

    private void refresh() {
        mods = ModManager.listMods(this);
        render();
        analyze(true);
    }

    /** Sizes, kinds, games and problems, off the UI thread; the file tables are cached. */
    private void analyze(boolean measure) {
        final int generation = ++analysisGeneration;
        final List<ModManager.Mod> snapshot = new ArrayList<>(mods);
        worker.execute(() -> {
            Map<String, Long> measured = new HashMap<>();
            Map<String, ModAnalysis.Info> found = new HashMap<>();
            Map<String, Map<String, String>> inis = new HashMap<>();
            for (ModManager.Mod mod : snapshot) {
                if (Thread.currentThread().isInterrupted()) {
                    return;
                }
                String key = mod.dir.getAbsolutePath();
                if (measure) {
                    measured.put(key, ModLibraryInfo.measure(mod.dir));
                }
                found.put(key, ModAnalysis.analyze(this, mod, mod.ini));
                inis.put(key, mod.ini);
            }
            List<ModAnalysis.Problem> next = ModAnalysis.problems(this, snapshot, found, inis);
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed() || generation != analysisGeneration) {
                    return;
                }
                sizes.putAll(measured);
                infos.clear();
                infos.putAll(found);
                problems = next;
                analyzed = true;
                render();
            });
        });
    }

    private void changed() {
        ModManager.saveOrder(this, mods);
        render();
        analyze(false);
    }

    // ------------------------------------------------------------------ list

    private void render() {
        list.removeAllViews();
        if (ModManager.gameRoot(this) == null) {
            LinearLayout card = UiKit.card(list);
            UiKit.sectionHeader(card, R.drawable.ic_gzh_folder, getString(R.string.mods_no_game_title), false);
            UiKit.supporting(card, getString(R.string.mods_no_game_message));
            return;
        }
        renderSummary();
        renderMods();
        renderLegacy();

        LinearLayout actions = UiKit.card(list);
        UiKit.button(actions, UiKit.BTN_TONAL, R.drawable.ic_gzh_download,
            getString(R.string.mods_import_title), this::showImportChoices).setEnabled(!busy);
        boolean anyOn = false;
        for (ModManager.Mod mod : mods) {
            anyOn |= mod.enabled;
        }
        UiKit.button(actions, UiKit.BTN_TONAL, R.drawable.ic_gzh_share,
            getString(R.string.mods_export_set), this::exportSet).setEnabled(!busy && anyOn);
        File root = ModManager.modsRoot(this);
        UiKit.helpText(actions, getString(R.string.mods_folder_help,
            root == null ? "" : root.getAbsolutePath()));
    }

    private void renderMods() {
        int shown = 0;
        for (int i = 0; i < mods.size(); i++) {
            ModManager.Mod mod = mods.get(i);
            if ((onlyEnabled && !mod.enabled)
                    || (!query.isEmpty() && !mod.name.toLowerCase(Locale.ROOT).contains(query))) {
                continue;
            }
            renderMod(i);
            shown++;
        }
        if (mods.isEmpty()) {
            LinearLayout empty = UiKit.card(list);
            UiKit.sectionHeader(empty, R.drawable.ic_gzh_folder, getString(R.string.mods_empty_title), false);
            UiKit.supporting(empty, getString(R.string.mods_library_empty));
        } else if (shown == 0) {
            UiKit.supporting(UiKit.card(list), getString(R.string.mods_filter_nothing));
        }
    }

    private void renderSummary() {
        LinearLayout intro = UiKit.card(list);
        UiKit.sectionHeader(intro, R.drawable.ic_gzh_sliders, getString(R.string.mods_order_title), false);
        UiKit.supporting(intro, getString(R.string.mods_order_help));

        boolean anyEnabled = false, anyIni = false;
        for (ModManager.Mod mod : mods) {
            if (mod.enabled) {
                anyEnabled = true;
                ModAnalysis.Info info = infos.get(mod.dir.getAbsolutePath());
                anyIni |= info != null && info.hasIni();
            }
        }
        if (anyEnabled) {
            int worst = -1;
            for (ModAnalysis.Problem p : problems) {
                worst = Math.max(worst, p.level);
            }
            if (!analyzed) {
                UiKit.chip(intro, R.drawable.ic_gzh_refresh, getString(R.string.mods_summary_checking),
                    R.color.gzh_on_surface_variant, R.color.gzh_surface_container_high);
            } else if (worst == ModAnalysis.LEVEL_RED) {
                UiKit.chip(intro, R.drawable.ic_gzh_info, getString(R.string.mods_summary_red),
                    R.color.gzh_status_error, R.color.gzh_surface_container_high).setMaxLines(4);
            } else if (worst == ModAnalysis.LEVEL_AMBER) {
                UiKit.chip(intro, R.drawable.ic_gzh_info, getString(R.string.mods_summary_amber),
                    R.color.gzh_status_warn, R.color.gzh_surface_container_high).setMaxLines(4);
            } else {
                UiKit.chip(intro, R.drawable.ic_gzh_check, getString(R.string.mods_summary_ok),
                    R.color.gzh_status_ok, R.color.gzh_surface_container_high).setMaxLines(4);
            }
            if (anyIni) {
                UiKit.supporting(intro, getString(R.string.mods_patch_off));
            }
            UiKit.supporting(intro, getString(R.string.mods_online_note));
        }

        if (mods.size() > 1) {
            UiKit.segmented(intro, new CharSequence[] {
                    getString(R.string.mods_filter_all), getString(R.string.mods_filter_on) },
                onlyEnabled ? 1 : 0, index -> {
                    onlyEnabled = index == 1;
                    render();
                });
        }
        if (mods.size() > 4) {
            EditText search = new EditText(this);
            search.setSingleLine(true);
            search.setHint(R.string.mods_search_hint);
            search.setText(query);
            search.setSelection(query.length());
            search.addTextChangedListener(new TextWatcher() {
                @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) { }
                @Override public void onTextChanged(CharSequence s, int a, int b, int c) { }
                @Override public void afterTextChanged(Editable s) {
                    String next = s.toString().trim().toLowerCase(Locale.ROOT);
                    if (!next.equals(query)) {
                        query = next;
                        // Keep the summary card (and the field with the keyboard) while the list changes.
                        while (list.getChildCount() > 1) {
                            list.removeViewAt(1);
                        }
                        renderMods();
                    }
                }
            });
            intro.addView(search);
        }
    }

    private void renderMod(int index) {
        final ModManager.Mod mod = mods.get(index);
        final ModAnalysis.Info info = infos.get(mod.dir.getAbsolutePath());
        LinearLayout card = UiKit.card(list);

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        card.addView(head, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        head.addView(picture(mod, 56));

        LinearLayout labels = new LinearLayout(this);
        labels.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(0,
            LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        labelParams.setMarginStart(UiKit.dp(this, 12));
        head.addView(labels, labelParams);
        labels.setClickable(true);
        labels.setOnClickListener(v -> showPage(mod));
        TextView name = UiKit.body(labels, mod.name);
        name.setTypeface(Typeface.DEFAULT_BOLD);
        name.setMaxLines(2);
        name.setEllipsize(TextUtils.TruncateAt.END);
        UiKit.caption(labels, meta(mod, info));

        SwitchCompat toggle = new SwitchCompat(this);
        toggle.setChecked(mod.enabled);
        toggle.setContentDescription(mod.name);
        toggle.setEnabled(!busy);
        toggle.setOnCheckedChangeListener((button, checked) -> {
            mod.enabled = checked;
            changed();
        });
        head.addView(toggle);

        addProblemChips(card, mod, 4);
        if (mod.enabled && analyzed && !hasProblem(mod)) {
            UiKit.chip(card, R.drawable.ic_gzh_check, getString(R.string.mods_card_ok),
                R.color.gzh_status_ok, R.color.gzh_surface_container_high);
        }

        if (!mod.description.isEmpty()) {
            TextView description = UiKit.supporting(card, mod.description);
            description.setMaxLines(3);
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
            () -> showPage(mod)));
        tools.addView(UiKit.iconButton(this, R.drawable.ic_gzh_trash, getString(R.string.mods_delete_button),
            () -> confirmDelete(mod)));
    }

    private void addProblemChips(LinearLayout parent, ModManager.Mod mod, int maxLines) {
        for (ModAnalysis.Problem p : problems) {
            if (p.mod == mod) {
                int color = p.level == ModAnalysis.LEVEL_RED ? R.color.gzh_status_error
                    : p.level == ModAnalysis.LEVEL_AMBER ? R.color.gzh_status_warn : R.color.gzh_on_surface_variant;
                UiKit.chip(parent, R.drawable.ic_gzh_info, p.text, color, R.color.gzh_surface_container_high)
                    .setMaxLines(maxLines);
            }
        }
    }

    private boolean hasProblem(ModManager.Mod mod) {
        for (ModAnalysis.Problem p : problems) {
            if (p.mod == mod && p.level > ModAnalysis.LEVEL_GREY) {
                return true;
            }
        }
        return false;
    }

    private String meta(ModManager.Mod mod, ModAnalysis.Info info) {
        List<String> parts = new ArrayList<>();
        if (info != null) {
            parts.add(getString(kindLabel(info.kind)));
            if (info.game != ModAnalysis.GAME_UNKNOWN) {
                parts.add(getString(info.game == ModAnalysis.GAME_ZH ? R.string.mods_game_zh
                    : info.game == ModAnalysis.GAME_GENERALS ? R.string.mods_game_generals : R.string.mods_game_both));
            }
        }
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

    private static int kindLabel(int kind) {
        switch (kind) {
            case ModAnalysis.KIND_TOTAL: return R.string.mods_kind_total;
            case ModAnalysis.KIND_GAMEPLAY: return R.string.mods_kind_gameplay;
            case ModAnalysis.KIND_MAPS: return R.string.mods_kind_maps;
            case ModAnalysis.KIND_LANGUAGE: return R.string.mods_kind_language;
            default: return R.string.mods_kind_cosmetic;
        }
    }

    /** The mod's own picture (mod.ini Image=, cover/preview/logo/icon), else its initials. */
    private View picture(ModManager.Mod mod, int sizeDp) {
        int size = UiKit.dp(this, sizeDp);
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
        initials.setTextSize(sizeDp / 3f);
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
        changed();
    }

    // ------------------------------------------------------------- old way

    private void renderLegacy() {
        List<String> legacy = ModManager.legacyArchives(this);
        if (legacy.isEmpty()) {
            return;
        }
        LinearLayout card = UiKit.card(list);
        UiKit.sectionHeader(card, R.drawable.ic_gzh_info, getString(R.string.mods_legacy_title), false);
        UiKit.supporting(card, getString(R.string.mods_legacy_help));
        for (String name : legacy) {
            UiKit.divider(card);
            UiKit.body(card, name);
            UiKit.button(card, UiKit.BTN_TONAL, R.drawable.ic_gzh_folder,
                getString(R.string.mods_legacy_move), () -> moveLegacy(name)).setEnabled(!busy);
        }
    }

    private void moveLegacy(String archive) {
        File moved = ModManager.moveLegacyArchive(this, archive);
        if (moved == null) {
            Toast.makeText(this, getString(R.string.mods_legacy_move_failed, archive), Toast.LENGTH_LONG).show();
            return;
        }
        // It loaded every time before: keep it switched on, below the mods already chosen.
        mods = ModManager.listMods(this);
        for (ModManager.Mod mod : mods) {
            if (mod.dir.getAbsolutePath().equals(moved.getAbsolutePath())) {
                mod.enabled = true;
            }
        }
        Toast.makeText(this, getString(R.string.mods_legacy_moved, archive), Toast.LENGTH_LONG).show();
        changed();
    }

    // --------------------------------------------------------------- page

    private void showPage(ModManager.Mod mod) {
        ModAnalysis.Info info = infos.get(mod.dir.getAbsolutePath());
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        int pad = UiKit.dp(this, 20);
        body.setPadding(pad, UiKit.dp(this, 8), pad, 0);

        body.addView(picture(mod, 120));
        List<File> shots = ModManager.screenshots(mod.dir);
        if (!shots.isEmpty()) {
            HorizontalScrollView scroller = new HorizontalScrollView(this);
            LinearLayout strip = new LinearLayout(this);
            strip.setOrientation(LinearLayout.HORIZONTAL);
            scroller.addView(strip);
            int h = UiKit.dp(this, 120);
            for (File shot : shots) {
                Bitmap bitmap = decode(shot, h);
                if (bitmap == null) {
                    continue;
                }
                ImageView image = new ImageView(this);
                image.setImageBitmap(bitmap);
                image.setAdjustViewBounds(true);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, h);
                lp.setMarginEnd(UiKit.dp(this, 8));
                strip.addView(image, lp);
            }
            LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            sp.topMargin = UiKit.dp(this, 12);
            body.addView(scroller, sp);
        }
        UiKit.caption(body, meta(mod, info));
        if (!mod.description.isEmpty()) {
            TextView description = UiKit.supporting(body, mod.description);
            description.setTextIsSelectable(true);
        }
        if (info != null) {
            UiKit.supporting(body, getString(R.string.mods_page_files, info.iniFiles, info.mapFiles,
                info.artFiles, info.windowFiles, info.textFiles));
        }
        addProblemChips(body, mod, 6);
        TextView report = UiKit.supporting(body, "");
        UiKit.button(body, UiKit.BTN_TONAL, R.drawable.ic_gzh_check, getString(R.string.mods_page_check),
            () -> {
                report.setText(R.string.mods_health_scanning);
                worker.execute(() -> {
                    ModHealthCheck.Result r = ModHealthCheck.scan(mod.dir);
                    runOnUiThread(() -> report.setText(healthText(r)));
                });
            });
        UiKit.button(body, UiKit.BTN_TONAL, R.drawable.ic_gzh_share, getString(R.string.mods_export),
            () -> exportMods(java.util.Collections.singletonList(mod), mod.dir.getName()));

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.addView(body);
        new AlertDialog.Builder(this)
            .setTitle(mod.name)
            .setView(scroll)
            .setPositiveButton(android.R.string.ok, null)
            .setNeutralButton(R.string.mods_delete_button, (d, w) -> confirmDelete(mod))
            .show();
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

    // ------------------------------------------------------------- sharing

    private void exportSet() {
        List<ModManager.Mod> on = new ArrayList<>();
        for (ModManager.Mod mod : mods) {
            if (mod.enabled) {
                on.add(mod);
            }
        }
        if (!on.isEmpty()) {
            exportMods(on, getString(R.string.mods_export_set_file));
        }
    }

    /** A zip of the mods (a set carries mods_set.txt with their order), shared through Android. */
    private void exportMods(List<ModManager.Mod> chosen, String baseName) {
        if (busy) {
            return;
        }
        busy = true;
        render();
        Toast.makeText(this, R.string.mods_export_working, Toast.LENGTH_LONG).show();
        worker.execute(() -> {
            File zip = null;
            String failure = null;
            try {
                zip = ModImportService.exportZip(this, chosen, baseName);
            } catch (java.io.IOException e) {
                failure = e.getMessage();
            }
            final File done = zip;
            final String error = failure;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) {
                    return;
                }
                busy = false;
                render();
                if (done == null) {
                    Toast.makeText(this, getString(R.string.mods_export_failed, String.valueOf(error)),
                        Toast.LENGTH_LONG).show();
                    return;
                }
                Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", done);
                Intent share = new Intent(Intent.ACTION_SEND);
                share.setType("application/zip");
                share.putExtra(Intent.EXTRA_STREAM, uri);
                share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                startActivity(Intent.createChooser(share, getString(R.string.mods_export)));
            });
        });
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
        if (!result.set.isEmpty()) {
            // A shared set: its mods, switched on, in the sender's order, above the others.
            List<ModManager.Mod> top = new ArrayList<>();
            for (File dir : result.set) {
                for (ModManager.Mod mod : mods) {
                    if (mod.dir.getAbsolutePath().equals(dir.getAbsolutePath())) {
                        mod.enabled = true;
                        top.add(mod);
                    }
                }
            }
            mods.removeAll(top);
            mods.addAll(0, top);
            changed();
            Toast.makeText(this, getString(R.string.mods_import_set_done, top.size()), Toast.LENGTH_LONG).show();
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
                changed();
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
