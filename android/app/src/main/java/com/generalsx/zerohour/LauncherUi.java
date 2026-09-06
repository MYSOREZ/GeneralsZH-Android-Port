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
// The launcher's shared view vocabulary. Every launcher screen is still
// built in code rather than from XML layouts -- that has never been the
// problem here, and changing it would have meant rewriting five screens'
// worth of working logic to gain nothing -- but until now each screen
// carried its own private copy of startCard()/addButton()/dp(), so
// "consistent spacing" and "consistent typography" were a matter of every
// call site remembering the same numbers. They did not: the same card was
// dp(12) here and dp(14) there, help text was 0.7f alpha in one screen and
// 0.8f in the next, and nothing enforced a minimum touch height anywhere.
//
// This class is the single place those decisions live now. A screen asks
// for a section, a card, an expander or a button and gets the same metrics
// and the same Material 3 type role every time, and a change to the scale
// below reaches all of them at once.
//
// Two rules the helpers below enforce that hand-rolled views kept missing:
//
//  1. Everything a finger can hit is at least TOUCH_TARGET (48dp) tall,
//     including the expander headers and the navigation rows, which look
//     like text but behave like buttons.
//  2. Text color is set explicitly after setTextAppearance(). Some Material
//     text appearances carry a color of their own, and a screen that asks
//     for "body text in the variant role" must get that role and not
//     whatever the type style happened to inherit.

package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;

final class LauncherUi {

    private LauncherUi() {}

    // Spacing scale, in dp. Every margin/padding in the launcher is one of
    // these; nothing picks its own number.
    static final int SPACE_1 = 4;
    static final int SPACE_2 = 8;
    static final int SPACE_3 = 12;
    static final int SPACE_4 = 16;
    static final int SPACE_5 = 24;
    static final int SPACE_6 = 32;

    // Minimum height for anything tappable.
    static final int TOUCH_TARGET = 48;

    // Corner radius for cards, matching M3's "large" shape.
    static final int CARD_RADIUS = 16;

    // A settings column wider than this stops being readable and starts
    // being a wall -- on a tablet or an unfolded foldable the content is
    // centred at this width instead of stretching edge to edge.
    static final int CONTENT_MAX_WIDTH = 640;

    // Button variants, in descending order of emphasis. One primary
    // (FILLED) action per screen; TONAL for the secondary action next to
    // it; OUTLINED for a normal action inside a card; TEXT for anything
    // destructive or rarely wanted, so it does not compete with the rest.
    static final int BUTTON_FILLED = 0;
    static final int BUTTON_TONAL = 1;
    static final int BUTTON_OUTLINED = 2;
    static final int BUTTON_TEXT = 3;

    static int dp(Context ctx, int value) {
        float density = ctx.getResources().getDisplayMetrics().density;
        return (int) (value * density + 0.5f);
    }

    static int color(Context ctx, int colorRes) {
        return ContextCompat.getColor(ctx, colorRes);
    }

    /**
     * Standard scrolling screen: a ScrollView filling the window, holding a
     * single vertical column that is centred and capped at
     * CONTENT_MAX_WIDTH. Sets the Activity's content view and applies the
     * system-bar/cutout insets. Returns the column to add content to.
     */
    static LinearLayout scrollingScaffold(Activity activity) {
        ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(true);

        LinearLayout column = new LinearLayout(activity);
        column.setOrientation(LinearLayout.VERTICAL);
        int side = dp(activity, SPACE_4);
        column.setPadding(side, dp(activity, SPACE_3), side, dp(activity, SPACE_6));
        scroll.addView(column, new ScrollView.LayoutParams(
            ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));

        activity.setContentView(scroll);
        InsetUtil.applySafeInsets(scroll);
        applyMaxContentWidth(scroll, column);
        return column;
    }

    /**
     * Keeps `column` no wider than CONTENT_MAX_WIDTH by growing its side
     * padding, re-evaluated on every layout pass so a rotation (these
     * Activities handle orientation changes themselves rather than being
     * recreated) is picked up without any extra plumbing.
     */
    static void applyMaxContentWidth(View container, View column) {
        final Context ctx = container.getContext();
        container.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int available = (r - l) - v.getPaddingLeft() - v.getPaddingRight();
            int minimum = dp(ctx, SPACE_4);
            int side = Math.max(minimum, (available - dp(ctx, CONTENT_MAX_WIDTH)) / 2 + minimum);
            if (column.getPaddingLeft() != side || column.getPaddingRight() != side) {
                column.setPadding(side, column.getPaddingTop(), side, column.getPaddingBottom());
            }
        });
    }

    /** Screen title + one-line description, at the top of a column. */
    static void screenHeader(LinearLayout column, CharSequence title, CharSequence subtitle) {
        Context ctx = column.getContext();
        TextView titleView = new TextView(ctx);
        titleView.setTextAppearance(R.style.Gzh_Text_Headline);
        titleView.setTextColor(color(ctx, R.color.gzh_on_surface));
        titleView.setText(title);
        column.addView(titleView, marginsBelow(ctx, SPACE_1));

        if (subtitle != null) {
            TextView subtitleView = new TextView(ctx);
            subtitleView.setTextAppearance(R.style.Gzh_Text_Body);
            subtitleView.setTextColor(color(ctx, R.color.gzh_on_surface_variant));
            subtitleView.setText(subtitle);
            column.addView(subtitleView, marginsBelow(ctx, SPACE_4));
        }
    }

    /**
     * Group heading between cards. Small, uppercase-weight label in the
     * primary role -- the thing that turns a flat stack of equal cards into
     * a screen with an order to it.
     */
    static void sectionLabel(LinearLayout column, CharSequence text) {
        Context ctx = column.getContext();
        TextView label = new TextView(ctx);
        label.setTextAppearance(R.style.Gzh_Text_LabelSmall);
        label.setTextColor(color(ctx, R.color.gzh_primary));
        label.setAllCaps(true);
        label.setLetterSpacing(0.08f);
        label.setText(text);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(ctx, SPACE_1), dp(ctx, SPACE_4), dp(ctx, SPACE_1), dp(ctx, SPACE_2));
        column.addView(label, lp);
    }

    /**
     * An empty card appended to `column`. Callers fill the returned view.
     * `strokeColorRes` of 0 means no outline -- used by the readiness banner,
     * whose tinted fill already separates it from the window background.
     */
    static MaterialCardView cardShell(LinearLayout column, int backgroundColorRes,
                                      int strokeColorRes) {
        Context ctx = column.getContext();
        MaterialCardView card = new MaterialCardView(ctx);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 0, 0, dp(ctx, SPACE_3));
        card.setLayoutParams(lp);
        card.setRadius(dp(ctx, CARD_RADIUS));
        // M3 cards sit on tone, not on a drop shadow; a hairline outline
        // keeps them legible in the light scheme where the card and the
        // window background are close in luminance.
        card.setCardElevation(0f);
        if (strokeColorRes != 0) {
            card.setStrokeWidth(dp(ctx, 1));
            card.setStrokeColor(color(ctx, strokeColorRes));
        } else {
            card.setStrokeWidth(0);
        }
        card.setCardBackgroundColor(color(ctx, backgroundColorRes));
        card.setContentPadding(0, 0, 0, 0);
        column.addView(card);
        return card;
    }

    private static MaterialCardView cardShell(LinearLayout column, int backgroundColorRes) {
        return cardShell(column, backgroundColorRes, R.color.gzh_outline_variant);
    }

    /**
     * A plain (always-open) card with an optional title. Returns the
     * vertical content holder, already padded.
     */
    static LinearLayout card(LinearLayout column, CharSequence title) {
        return card(column, title, R.color.gzh_surface);
    }

    static LinearLayout card(LinearLayout column, CharSequence title, int backgroundColorRes) {
        Context ctx = column.getContext();
        MaterialCardView card = cardShell(column, backgroundColorRes);
        LinearLayout content = new LinearLayout(ctx);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(ctx, SPACE_4);
        content.setPadding(pad, pad, pad, pad);
        card.addView(content);

        if (title != null) {
            TextView titleView = new TextView(ctx);
            titleView.setTextAppearance(R.style.Gzh_Text_Title);
            titleView.setTextColor(color(ctx, R.color.gzh_on_surface));
            titleView.setText(title);
            content.addView(titleView, marginsBelow(ctx, SPACE_2));
        }
        return content;
    }

    /**
     * A card whose body is hidden until tapped. This is what keeps a screen
     * with a dozen settings on it scannable: collapsed, each one is a single
     * row showing its name and its current value, and only the setting being
     * changed takes up any room.
     */
    static final class Expander {
        final MaterialCardView card;
        /** Add the section's controls here. */
        final LinearLayout content;
        private final TextView summaryView;
        private final ImageView chevron;
        private boolean expanded;

        private Expander(MaterialCardView card, LinearLayout content, TextView summaryView,
                         ImageView chevron) {
            this.card = card;
            this.content = content;
            this.summaryView = summaryView;
            this.chevron = chevron;
        }

        void setSummary(CharSequence summary) {
            summaryView.setText(summary);
            summaryView.setVisibility(summary == null || summary.length() == 0 ? View.GONE : View.VISIBLE);
        }

        boolean isExpanded() {
            return expanded;
        }

        void setExpanded(boolean expand) {
            setExpanded(expand, true);
        }

        void setExpanded(boolean expand, boolean animate) {
            expanded = expand;
            content.setVisibility(expand ? View.VISIBLE : View.GONE);
            Context ctx = chevron.getContext();
            chevron.setContentDescription(ctx.getString(
                expand ? R.string.common_collapse : R.string.common_expand));
            if (animate) {
                chevron.animate().rotation(expand ? 180f : 0f).setDuration(150).start();
            } else {
                chevron.setRotation(expand ? 180f : 0f);
            }
        }
    }

    static Expander expander(LinearLayout column, CharSequence title, CharSequence summary) {
        Context ctx = column.getContext();
        MaterialCardView card = cardShell(column, R.color.gzh_surface);

        LinearLayout body = new LinearLayout(ctx);
        body.setOrientation(LinearLayout.VERTICAL);
        card.addView(body);

        LinearLayout header = new LinearLayout(ctx);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setMinimumHeight(dp(ctx, TOUCH_TARGET + SPACE_2));
        int pad = dp(ctx, SPACE_4);
        header.setPadding(pad, dp(ctx, SPACE_3), pad, dp(ctx, SPACE_3));
        header.setBackgroundResource(selectableItemBackground(ctx));
        header.setClickable(true);
        header.setFocusable(true);
        body.addView(header, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout labels = new LinearLayout(ctx);
        labels.setOrientation(LinearLayout.VERTICAL);
        header.addView(labels, new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView titleView = new TextView(ctx);
        titleView.setTextAppearance(R.style.Gzh_Text_Title);
        titleView.setTextColor(color(ctx, R.color.gzh_on_surface));
        titleView.setText(title);
        labels.addView(titleView);

        TextView summaryView = new TextView(ctx);
        summaryView.setTextAppearance(R.style.Gzh_Text_BodySmall);
        summaryView.setTextColor(color(ctx, R.color.gzh_on_surface_variant));
        labels.addView(summaryView);

        ImageView chevron = new ImageView(ctx);
        chevron.setImageResource(R.drawable.ic_gzh_expand_more);
        chevron.setImageTintList(ColorStateList.valueOf(color(ctx, R.color.gzh_on_surface_variant)));
        LinearLayout.LayoutParams chevronLp = new LinearLayout.LayoutParams(
            dp(ctx, SPACE_5), dp(ctx, SPACE_5));
        chevronLp.setMarginStart(dp(ctx, SPACE_3));
        header.addView(chevron, chevronLp);

        LinearLayout content = new LinearLayout(ctx);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(pad, 0, pad, pad);
        body.addView(content);

        Expander expander = new Expander(card, content, summaryView, chevron);
        expander.setSummary(summary);
        expander.setExpanded(false, false);
        header.setOnClickListener(v -> expander.setExpanded(!expander.isExpanded()));
        return expander;
    }

    /**
     * A tappable row that goes somewhere else -- the visual opposite of a
     * card full of controls, and the reason the advanced settings can leave
     * the main screen without becoming hard to find.
     */
    static void navigationRow(LinearLayout column, CharSequence title, CharSequence subtitle,
                              Runnable action) {
        Context ctx = column.getContext();
        MaterialCardView card = cardShell(column, R.color.gzh_surface);

        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dp(ctx, TOUCH_TARGET + SPACE_2));
        int pad = dp(ctx, SPACE_4);
        row.setPadding(pad, dp(ctx, SPACE_3), pad, dp(ctx, SPACE_3));
        row.setBackgroundResource(selectableItemBackground(ctx));
        row.setClickable(true);
        row.setFocusable(true);
        row.setOnClickListener(v -> action.run());
        card.addView(row);

        LinearLayout labels = new LinearLayout(ctx);
        labels.setOrientation(LinearLayout.VERTICAL);
        row.addView(labels, new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView titleView = new TextView(ctx);
        titleView.setTextAppearance(R.style.Gzh_Text_Title);
        titleView.setTextColor(color(ctx, R.color.gzh_on_surface));
        titleView.setText(title);
        labels.addView(titleView);

        if (subtitle != null) {
            TextView subtitleView = new TextView(ctx);
            subtitleView.setTextAppearance(R.style.Gzh_Text_BodySmall);
            subtitleView.setTextColor(color(ctx, R.color.gzh_on_surface_variant));
            subtitleView.setText(subtitle);
            labels.addView(subtitleView);
        }

        ImageView chevron = new ImageView(ctx);
        chevron.setImageResource(R.drawable.ic_gzh_chevron_right);
        chevron.setImageTintList(ColorStateList.valueOf(color(ctx, R.color.gzh_on_surface_variant)));
        LinearLayout.LayoutParams chevronLp = new LinearLayout.LayoutParams(
            dp(ctx, SPACE_5), dp(ctx, SPACE_5));
        chevronLp.setMarginStart(dp(ctx, SPACE_3));
        row.addView(chevron, chevronLp);
    }

    static MaterialButton button(LinearLayout parent, int variant, CharSequence label,
                                 Runnable action) {
        Context ctx = parent.getContext();
        MaterialButton button = new MaterialButton(new ContextThemeWrapper(ctx, overlayFor(variant)));
        button.setText(label);
        button.setMinHeight(dp(ctx, TOUCH_TARGET));
        button.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(ctx, SPACE_1), 0, dp(ctx, SPACE_1));
        parent.addView(button, lp);
        return button;
    }

    private static int overlayFor(int variant) {
        switch (variant) {
            case BUTTON_TONAL:    return R.style.ThemeOverlay_Gzh_Button_Tonal;
            case BUTTON_OUTLINED: return R.style.ThemeOverlay_Gzh_Button_Outlined;
            case BUTTON_TEXT:     return R.style.ThemeOverlay_Gzh_Button_Text;
            default:              return R.style.ThemeOverlay_Gzh_Button_Filled;
        }
    }

    /** Body copy in the default on-surface role. */
    static TextView body(LinearLayout parent, CharSequence text) {
        return text(parent, text, R.style.Gzh_Text_Body, R.color.gzh_on_surface, SPACE_2);
    }

    /**
     * Supporting copy -- the "what this setting is for" paragraphs. Smaller
     * and in the variant role rather than the old alpha-dimmed body text,
     * which produced a different effective contrast on every background it
     * landed on.
     */
    static TextView help(LinearLayout parent, CharSequence text) {
        return text(parent, text, R.style.Gzh_Text_BodySmall, R.color.gzh_on_surface_variant, SPACE_2);
    }

    static TextView text(LinearLayout parent, CharSequence text, int appearance, int colorRes,
                         int marginBelow) {
        Context ctx = parent.getContext();
        TextView view = new TextView(ctx);
        view.setTextAppearance(appearance);
        view.setTextColor(color(ctx, colorRes));
        view.setText(text);
        parent.addView(view, marginsBelow(ctx, marginBelow));
        return view;
    }

    /** A filled, rounded pill used for the readiness banner. */
    static GradientDrawable pill(Context ctx, int fillColorRes, int radiusDp) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.RECTANGLE);
        shape.setCornerRadius(dp(ctx, radiusDp));
        shape.setColor(color(ctx, fillColorRes));
        return shape;
    }

    /** A small solid circle, for the status dot. */
    static GradientDrawable dot(Context ctx, int fillColorRes) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.OVAL);
        shape.setColor(color(ctx, fillColorRes));
        return shape;
    }

    static LinearLayout.LayoutParams marginsBelow(Context ctx, int below) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 0, 0, dp(ctx, below));
        return lp;
    }

    private static int selectableItemBackground(Context ctx) {
        TypedValue value = new TypedValue();
        ctx.getTheme().resolveAttribute(android.R.attr.selectableItemBackground, value, true);
        return value.resourceId;
    }
}
