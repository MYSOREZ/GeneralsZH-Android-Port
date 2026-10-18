package com.generalsx.zerohour;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.google.android.material.button.MaterialButton;

// GeneralsX @feature Android port 04/10/2026 Shared native launcher presentation only.
// No game, storage, import, update, account, or renderer state is owned here.
// GeneralsX @feature Android port 18/10/2026 From realkot's GeneralsX fork (with the author's
// permission); only the parts the mods screen uses -- this launcher keeps its own navigation.
final class LauncherUi {
    // GeneralsX @feature 10/10/2026 Resolve semantic tokens from the Activity's selected theme.
    static int background(Activity a) { return UiKit.color(a, R.color.gzh_background); }
    static int surface(Activity a) { return UiKit.color(a, R.color.gzh_surface_container); }
    static int outline(Activity a) { return UiKit.color(a, R.color.gzh_outline); }
    static int textColor(Activity a) { return UiKit.color(a, R.color.gzh_on_surface); }
    static int muted(Activity a) { return UiKit.color(a, R.color.gzh_on_surface_variant); }
    static int primary(Activity a) { return UiKit.color(a, R.color.gzh_primary); }
    static int warning(Activity a) { return UiKit.color(a, R.color.gzh_status_warn); }
    static int success(Activity a) { return UiKit.color(a, R.color.gzh_status_ok); }
    private LauncherUi() {}

    static int dp(Activity a, int value) { return UiKit.dp(a, value); }

    static LinearLayout column(Activity a) {
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        return v;
    }

    static GradientDrawable shape(Activity a, int fill, int border, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(a, radius));
        d.setStroke(dp(a, 1), border);
        return d;
    }

    static LinearLayout panel(Activity a, LinearLayout parent) {
        LinearLayout body = column(a);
        body.setPadding(dp(a, 12), dp(a, 12), dp(a, 12), dp(a, 12));
        body.setBackground(shape(a, surface(a), outline(a), 16));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(a, 12);
        parent.addView(body, lp);
        return body;
    }

    static TextView text(Activity a, LinearLayout parent, CharSequence label,
                         int size, int color, boolean bold) {
        TextView v = new TextView(a);
        v.setText(label);
        v.setTextSize(size);
        v.setTextColor(color);
        if (bold) v.setTypeface(Typeface.DEFAULT_BOLD);
        v.setMaxLines(2);
        v.setEllipsize(TextUtils.TruncateAt.END);
        parent.addView(v, new LinearLayout.LayoutParams(-1, -2));
        return v;
    }

    static FrameLayout artwork(Activity a, LinearLayout parent, int height) {
        FrameLayout frame = new FrameLayout(a);
        frame.setBackground(shape(a, surface(a), outline(a), 18));
        frame.setClipToOutline(true);
        parent.addView(frame, new LinearLayout.LayoutParams(-1, dp(a, height)));
        ImageView image = new ImageView(a);
        image.setScaleType(ImageView.ScaleType.CENTER_CROP);
        image.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        frame.addView(image, new FrameLayout.LayoutParams(-1, -1));
        return frame;
    }

    static void purple(Activity a, MaterialButton button) {
        // Legacy method name retained for old callers; the color is now warm gold.
        button.setCornerRadius(dp(a, 15));
        button.setTextColor(UiKit.color(a, R.color.gzh_on_primary));
        button.setIconTint(ColorStateList.valueOf(UiKit.color(a, R.color.gzh_on_primary)));
        button.setBackgroundTintList(ColorStateList.valueOf(primary(a)));
        button.setStrokeColor(ColorStateList.valueOf(primary(a)));
    }
}
