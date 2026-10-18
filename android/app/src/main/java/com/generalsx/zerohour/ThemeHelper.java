package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.view.Window;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsControllerCompat;

/**
 * GeneralsX @feature 10/10/2026 Launcher-only Light / Dark / Follow System.
 * Never modifies GeneralsZHActivity, SDL, engine, graphics settings or mod state.
 * Default DARK preserves the previously shipped launcher appearance.
 */
final class ThemeHelper {
    static final int SYSTEM = 0;
    static final int LIGHT = 1;
    static final int DARK = 2;
    private static final String PREF_MODE = "launcher_theme_mode";
    private ThemeHelper() {}

    static int getMode(Context context) {
        int stored = context.getSharedPreferences(LocaleHelper.PREFS_NAME, Context.MODE_PRIVATE)
            .getInt(PREF_MODE, DARK);
        return stored >= SYSTEM && stored <= DARK ? stored : DARK;
    }

    static void setMode(Context context, int mode) {
        if (mode < SYSTEM || mode > DARK) {
            throw new IllegalArgumentException("Unsupported launcher theme");
        }
        context.getSharedPreferences(LocaleHelper.PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putInt(PREF_MODE, mode).apply();
    }

    static Context wrap(Context context) {
        int mode = getMode(context);
        if (mode == SYSTEM) return context;
        Configuration config = new Configuration(context.getResources().getConfiguration());
        config.uiMode = (config.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
            | (mode == LIGHT ? Configuration.UI_MODE_NIGHT_NO : Configuration.UI_MODE_NIGHT_YES);
        return context.createConfigurationContext(config);
    }

    static boolean isDark(Context context) {
        int mode = getMode(context);
        if (mode == DARK) return true;
        if (mode == LIGHT) return false;
        return (context.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
            != Configuration.UI_MODE_NIGHT_NO;
    }

    static void apply(Activity activity) {
        activity.setTheme(isDark(activity)
            ? R.style.Theme_GeneralsZHSettings : R.style.Theme_GeneralsZHSettingsLight);
    }

    static void applyBars(Activity activity) {
        Window window = activity.getWindow();
        WindowInsetsControllerCompat controller =
            WindowCompat.getInsetsController(window, window.getDecorView());
        boolean light = !isDark(activity);
        controller.setAppearanceLightStatusBars(light);
        controller.setAppearanceLightNavigationBars(light);
    }
}
