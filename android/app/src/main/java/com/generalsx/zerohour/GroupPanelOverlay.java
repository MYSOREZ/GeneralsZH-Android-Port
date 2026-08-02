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

// GeneralsX @feature Android port 02/08/2026
//
// Touch overlay for unit control groups (1-9, 0), drawn over the SDL
// surface -- the same idea as ExaGear/Winlator's touch-control overlays,
// not an engine-side GameWindow. The engine's own UI (.wnd layouts) comes
// entirely from the user's game data archives, which this project never
// ships; adding a brand-new engine-native window would mean solving how to
// load one outside that archive. A plain Android View sidesteps the
// question: it draws on top of the SDL surface and talks to the engine
// through two small JNI calls (SDL3GameEngine.cpp) instead.
//
// Tap a numbered button: recall/select that group -- SelectionXlat.cpp
// already handles everything group-related (assignment, recall,
// double-tap-same-number-to-recenter-camera); this only needs to feed it
// the same MSG_META_SELECT_TEAM<n> a bare 1-9/0 keypress would have.
// Long-press a numbered button: assign the current selection to that group
// (MSG_META_CREATE_TEAM<n>, what Ctrl+1-9/0 does on a keyboard).
//
// Collapsed by default behind a small handle tab (bottom-right corner);
// tapping the handle expands/collapses the row of 10 buttons. Occupied
// groups (queried from the native side) are tinted differently from empty
// ones, refreshed on a timer only while the panel is expanded.

package com.generalsx.zerohour;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;

public class GroupPanelOverlay {

    private static final int GROUP_COUNT = 10;
    private static final int REFRESH_INTERVAL_MS = 500;
    private static final int SHELL_POLL_INTERVAL_MS = 500;

    // GeneralsX @bugfix Android port 02/08/2026 The original dark-gray,
    // 55%-alpha handle was reported invisible on a real device -- it was
    // genuinely just blending into typical green/brown terrain, not a
    // logic bug (the shell-active gating already worked correctly, same
    // flag proven correct for camera pan/zoom elsewhere). Bright, near-
    // opaque amber (matching the HUD's own gold/amber accent color, per
    // the $-cost/star-rank icons) plus a light border reads clearly
    // against any terrain color.
    private static final int COLOR_HANDLE = Color.argb(235, 220, 160, 40);
    private static final int COLOR_HANDLE_BORDER = Color.argb(255, 255, 235, 180);
    private static final int COLOR_PANEL_BG = Color.argb(210, 20, 20, 20);
    private static final int COLOR_BUTTON_EMPTY = Color.argb(200, 90, 90, 90);
    private static final int COLOR_BUTTON_OCCUPIED = Color.argb(230, 60, 160, 60);

    private final View handle;
    private final LinearLayout panel;
    private final Button[] groupButtons = new Button[GROUP_COUNT];
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean expanded = false;

    private final Runnable refreshRunnable = new Runnable() {
        @Override
        public void run() {
            refreshOccupancy();
            if (expanded) {
                handler.postDelayed(this, REFRESH_INTERVAL_MS);
            }
        }
    };

    // GeneralsX @feature Android port 02/08/2026 Control groups only mean
    // anything mid-match -- there's no local player/squads in the main
    // menu/lobby/shell screens, and showing the handle there is just visual
    // clutter. Runs for the whole lifetime of the overlay (unlike
    // refreshRunnable, which only runs while expanded), since this is what
    // decides whether the handle itself is even visible.
    private final Runnable shellPollRunnable = new Runnable() {
        @Override
        public void run() {
            boolean shellActive = GeneralsZHActivity.nativeIsShellActive();
            handle.setVisibility(shellActive ? View.GONE : View.VISIBLE);
            if (shellActive && expanded) {
                setExpanded(false);
            }
            handler.postDelayed(this, SHELL_POLL_INTERVAL_MS);
        }
    };

    public GroupPanelOverlay(Context context, RelativeLayout root) {
        float density = context.getResources().getDisplayMetrics().density;

        handle = new Button(context);
        ((Button) handle).setAllCaps(false);
        ((Button) handle).setText("≡"); // "≡" -- generic menu/groups glyph
        ((Button) handle).setTextColor(Color.BLACK);
        ((Button) handle).setTextSize(20f);
        ((Button) handle).setTypeface(Typeface.DEFAULT_BOLD);
        handle.setId(View.generateViewId());
        {
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(COLOR_HANDLE);
            bg.setStroke((int) (2 * density), COLOR_HANDLE_BORDER);
            bg.setCornerRadius(6 * density);
            handle.setBackground(bg);
        }
        RelativeLayout.LayoutParams handleParams = new RelativeLayout.LayoutParams(
                (int) (44 * density), (int) (44 * density));
        // GeneralsX @tweak Android port 02/08/2026 Moved from bottom-right to
        // bottom-left (above the minimap) per tester feedback.
        handleParams.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
        handleParams.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        handleParams.leftMargin = (int) (6 * density);
        // GeneralsX @feature Android port 02/08/2026 Rough guess at clearing
        // the engine's own minimap/command bar (rendered by the engine
        // itself, not an Android view, so there's no layout API to ask its
        // real height) -- adjust this margin after seeing it in game if it
        // overlaps.
        handleParams.bottomMargin = (int) (150 * density);
        handle.setLayoutParams(handleParams);
        handle.setOnClickListener(v -> setExpanded(!expanded));
        root.addView(handle);

        panel = new LinearLayout(context);
        panel.setId(View.generateViewId());
        panel.setOrientation(LinearLayout.HORIZONTAL);
        panel.setBackgroundColor(COLOR_PANEL_BG);
        panel.setVisibility(View.GONE);
        RelativeLayout.LayoutParams panelParams = new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, (int) (40 * density));
        panelParams.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
        panelParams.addRule(RelativeLayout.ABOVE, handle.getId());
        panelParams.leftMargin = (int) (6 * density);
        panelParams.bottomMargin = (int) (4 * density);
        root.addView(panel, panelParams);

        for (int i = 0; i < GROUP_COUNT; i++) {
            final int group = i;
            Button b = new Button(context);
            b.setAllCaps(false);
            b.setText(String.valueOf((i + 1) % 10)); // 1,2,...,9,0 -- matches the keyboard row
            b.setTextSize(14f);
            b.setBackgroundColor(COLOR_BUTTON_EMPTY);
            b.setPadding(0, 0, 0, 0);
            LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(
                    (int) (34 * density), ViewGroup.LayoutParams.MATCH_PARENT);
            b.setLayoutParams(bp);
            b.setOnClickListener(v -> GeneralsZHActivity.nativeGroupCommand(group, false));
            b.setOnLongClickListener(v -> {
                GeneralsZHActivity.nativeGroupCommand(group, true);
                return true;
            });
            groupButtons[i] = b;
            panel.addView(b);
        }

        handle.setVisibility(View.GONE); // hidden until the first shell-poll tick confirms we're in-game
        handler.post(shellPollRunnable);
    }

    private void setExpanded(boolean value) {
        expanded = value;
        panel.setVisibility(expanded ? View.VISIBLE : View.GONE);
        if (expanded) {
            refreshOccupancy();
            handler.postDelayed(refreshRunnable, REFRESH_INTERVAL_MS);
        } else {
            handler.removeCallbacks(refreshRunnable);
        }
    }

    private void refreshOccupancy() {
        int mask = GeneralsZHActivity.nativeGetGroupOccupancyMask();
        for (int i = 0; i < GROUP_COUNT; i++) {
            boolean occupied = (mask & (1 << i)) != 0;
            groupButtons[i].setBackgroundColor(occupied ? COLOR_BUTTON_OCCUPIED : COLOR_BUTTON_EMPTY);
        }
    }
}
