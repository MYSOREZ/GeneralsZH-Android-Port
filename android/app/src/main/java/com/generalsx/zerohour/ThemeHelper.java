package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.view.Window;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsControllerCompat;

/**
 * GeneralsX @feature Android port 18/10/2026 The launcher's appearance (Interface -> Appearance):
 * the standard dark look by default, or the "navy" (navy and gold) or "day" (light) palette of the
 * GeneralsX fork by realkot (used with permission), or "system", which follows the phone: day
 * when it is light, standard when it is dark. Launcher only; the game is never affected.
 *
 * A palette is a set of colours named like the standard ones with a gzh_navy_ / gzh_day_ prefix
 * (colors_palettes.xml): UiKit.color() maps a standard colour to the chosen palette, and apply()
 * sets the matching Material theme, so screens keep naming the standard colours.
 */
final class ThemeHelper {
    static final int STANDARD = 0;
    static final int NAVY = 1;
    static final int DAY = 2;
    static final int SYSTEM = 3;
    private static final String PREF_MODE = "launcher_theme_mode";
    private static final android.util.SparseIntArray sNavy = new android.util.SparseIntArray();
    private static final android.util.SparseIntArray sDay = new android.util.SparseIntArray();
    private ThemeHelper() {}

    static int getMode(Context context) {
        int stored = context.getSharedPreferences(LocaleHelper.PREFS_NAME, Context.MODE_PRIVATE)
            .getInt(PREF_MODE, STANDARD);
        return stored >= STANDARD && stored <= SYSTEM ? stored : STANDARD;
    }

    static void setMode(Context context, int mode) {
        if (mode < STANDARD || mode > SYSTEM) {
            throw new IllegalArgumentException("Unsupported launcher theme");
        }
        context.getSharedPreferences(LocaleHelper.PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putInt(PREF_MODE, mode).apply();
    }

    /** The palette in use: STANDARD, NAVY or DAY (SYSTEM resolved against the phone). */
    static int palette(Context context) {
        int mode = getMode(context);
        if (mode != SYSTEM) {
            return mode;
        }
        int night = context.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        return night == Configuration.UI_MODE_NIGHT_NO ? DAY : STANDARD;
    }

    /** The colour resource to use for a standard gzh_* colour under the chosen palette. */
    static int mapColor(Context context, int colorRes) {
        int palette = palette(context);
        if (palette == STANDARD) {
            return colorRes;
        }
        android.util.SparseIntArray cache = palette == NAVY ? sNavy : sDay;
        synchronized (cache) {
            int mapped = cache.get(colorRes, 0);
            if (mapped == 0) {
                android.content.res.Resources res = context.getResources();
                String name = res.getResourceEntryName(colorRes);
                mapped = colorRes;
                if (name.startsWith("gzh_")) {
                    int id = res.getIdentifier((palette == NAVY ? "gzh_navy_" : "gzh_day_")
                        + name.substring(4), "color", context.getPackageName());
                    if (id != 0) {
                        mapped = id;
                    }
                }
                cache.put(colorRes, mapped);
            }
            return mapped;
        }
    }

    static void apply(Activity activity) {
        int palette = palette(activity);
        activity.setTheme(palette == NAVY ? R.style.Theme_GeneralsZHSettingsNavy
            : palette == DAY ? R.style.Theme_GeneralsZHSettingsLight : R.style.Theme_GeneralsZHSettings);
    }

    static void applyBars(Activity activity) {
        Window window = activity.getWindow();
        WindowInsetsControllerCompat controller =
            WindowCompat.getInsetsController(window, window.getDecorView());
        boolean light = palette(activity) == DAY;
        controller.setAppearanceLightStatusBars(light);
        controller.setAppearanceLightNavigationBars(light);
    }
}
