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
    // GeneralsX @bugfix Android port 02/08/2026 Reported "stylistically out
    // of place" -- plain gray Android buttons next to the game's own
    // steel/gold-trimmed HUD panels. Dark steel-green panel + gold-bordered
    // buttons matches the command bar's own look (dark green-gray metal,
    // gold/bronze trim and text) instead of a generic system control.
    private static final int COLOR_PANEL_BG = Color.argb(225, 28, 36, 32);
    private static final int COLOR_BUTTON_BORDER = Color.argb(255, 195, 165, 90);
    private static final int COLOR_BUTTON_EMPTY = Color.argb(230, 42, 52, 47);
    private static final int COLOR_BUTTON_OCCUPIED = Color.argb(235, 70, 130, 60);

    private final View handle;
    private final LinearLayout panel;
    private final Button[] groupButtons = new Button[GROUP_COUNT];
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean expanded = false;
    // GeneralsX @bugfix Android port 02/08/2026 A tester's very first try --
    // select a worker, tap "1" -- cleared the selection instead of assigning
    // it, because a plain tap on an EMPTY group recalls (and finds) nothing,
    // deselecting everything in the process (matches PC: bare "1" selects,
    // Ctrl+1 assigns, and pressing bare "1" on an empty group behaves
    // identically there too -- but a first-time toucher has no reason to
    // already know the long-press-to-assign convention). Since a group can
    // only ever be legitimately recalled AFTER it's been assigned at least
    // once, a tap on a still-empty group has no useful "recall" meaning to
    // begin with -- reinterpreting it as assign there costs nothing and
    // fixes exactly this. Once a group has real members, tap goes back to
    // being a normal recall (the frequent, common case during a match);
    // long-press still always assigns/replaces a group's members regardless
    // of occupancy.
    private int lastOccupancyMask = 0;

    private final Runnable refreshRunnable = new Runnable() {
        @Override
        public void run() {
            refreshOccupancy();
            if (expanded) {
                handler.postDelayed(this, REFRESH_INTERVAL_MS);
            }
        }
    };

    // GeneralsX @bugfix Android port 02/08/2026 Control groups only mean
    // anything mid-match. Reported visible during the initial black
    // loading screen and the pre-match general-briefing screen -- checking
    // only TheShell->isShellActive() (native side) wasn't enough, since
    // the briefing screen already has an interactive game mode set even
    // though the simulation hasn't started ticking yet. See
    // nativeIsGameplayActive()'s comment in SDL3GameEngine.cpp for the
    // precise frame-count-based check. Runs for the whole lifetime of the
    // overlay (unlike refreshRunnable, which only runs while expanded),
    // since this is what decides whether the handle itself is even visible.
    private final Runnable shellPollRunnable = new Runnable() {
        @Override
        public void run() {
            boolean gameplayActive = GeneralsZHActivity.nativeIsGameplayActive();
            handle.setVisibility(gameplayActive ? View.VISIBLE : View.GONE);
            if (!gameplayActive && expanded) {
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
        {
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(COLOR_PANEL_BG);
            bg.setStroke((int) (1.5f * density), COLOR_BUTTON_BORDER);
            bg.setCornerRadius(4 * density);
            panel.setBackground(bg);
        }
        panel.setPadding((int) (4 * density), (int) (4 * density), (int) (4 * density), (int) (4 * density));
        panel.setVisibility(View.GONE);
        RelativeLayout.LayoutParams panelParams = new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, (int) (48 * density));
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
            b.setTextSize(15f);
            b.setTypeface(Typeface.DEFAULT_BOLD);
            b.setTextColor(Color.argb(255, 235, 220, 190));
            {
                GradientDrawable bg = new GradientDrawable();
                bg.setColor(COLOR_BUTTON_EMPTY);
                bg.setStroke((int) (1 * density), COLOR_BUTTON_BORDER);
                bg.setCornerRadius(3 * density);
                b.setBackground(bg);
            }
            b.setPadding(0, 0, 0, 0);
            LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(
                    (int) (34 * density), ViewGroup.LayoutParams.MATCH_PARENT);
            bp.setMargins((int) (1.5f * density), 0, (int) (1.5f * density), 0);
            b.setLayoutParams(bp);
            b.setOnClickListener(v -> {
                boolean empty = (lastOccupancyMask & (1 << group)) == 0;
                GeneralsZHActivity.nativeGroupCommand(group, empty);
            });
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
        lastOccupancyMask = GeneralsZHActivity.nativeGetGroupOccupancyMask();
        for (int i = 0; i < GROUP_COUNT; i++) {
            boolean occupied = (lastOccupancyMask & (1 << i)) != 0;
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(occupied ? COLOR_BUTTON_OCCUPIED : COLOR_BUTTON_EMPTY);
            bg.setStroke(1, COLOR_BUTTON_BORDER);
            groupButtons[i].setBackground(bg);
        }
    }
}
