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

// GeneralsX @feature Android port 23/09/2026 Replay check screen.
//
// Checking a PC recording against this build used to mean launching the game,
// opening the replay and watching it in real time until the frame that mattered.
// A replay holds commands, not state, so it cannot be cut to start later -- but it
// can be simulated faster than it is drawn. This screen lists the replays and starts
// the game on one of them with the engine options from GXReplayCheck.h:
//
//   Run it through       -gxFastTo -1 -gxAutoQuit: no watching; the engine stops a
//                        few hundred frames after the first checksum mismatch (or at
//                        the end), writes gx_replay_check_result.txt and quits back here
//   Watch from frame N   -gxFastTo N: fast-forward to N, then play normally
//
// Both force the network trace marker on, because the checksum comparison and every
// diagnostic that follows it only print with it.

package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.text.DateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;

public class ReplayCheckActivity extends Activity {

    private static final String RESULT_FILE = "gx_replay_check_result.txt";
    private static final String NET_TRACE_MARKER = "gx_net_trace.txt";

    private static final int MODE_RUN_THROUGH = 0;
    private static final int MODE_WATCH_FROM = 1;

    private int mode = MODE_RUN_THROUGH;
    private TextView resultText;
    private EditText frameInput;
    private LinearLayout frameRow;
    private LinearLayout replayList;
    private Intent pendingLaunch;

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.replaycheck_title);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(UiKit.color(this, R.color.gzh_background));
        UiKit.appBar(root, getString(R.string.setup_window_title),
            getString(R.string.replaycheck_title), R.drawable.ic_gzh_refresh,
            getString(R.string.replaycheck_refresh), this::refresh);

        LinearLayout page = UiKit.scrollingPage(root);

        LinearLayout resultCard = UiKit.card(page);
        UiKit.sectionHeader(resultCard, R.drawable.ic_gzh_check,
            getString(R.string.replaycheck_last_result), false);
        resultText = UiKit.body(resultCard, "");
        resultText.setTextIsSelectable(true);
        UiKit.button(resultCard, UiKit.BTN_TONAL, R.drawable.ic_gzh_doc,
            getString(R.string.setup_button_view_logs),
            () -> startActivity(new Intent(this, LogViewerActivity.class)));

        LinearLayout modeCard = UiKit.card(page);
        UiKit.sectionHeader(modeCard, R.drawable.ic_gzh_sliders,
            getString(R.string.replaycheck_mode), false);
        UiKit.segmented(modeCard, new CharSequence[] {
                getString(R.string.replaycheck_mode_run),
                getString(R.string.replaycheck_mode_watch)
            }, mode, index -> {
                mode = index;
                updateModeUi();
            });
        UiKit.supporting(modeCard, getString(R.string.replaycheck_mode_help));
        frameRow = new LinearLayout(this);
        frameRow.setOrientation(LinearLayout.VERTICAL);
        modeCard.addView(frameRow);
        UiKit.caption(frameRow, getString(R.string.replaycheck_frame_label));
        frameInput = new EditText(this);
        frameInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        frameInput.setHint("3400");
        frameRow.addView(frameInput);

        LinearLayout listCard = UiKit.card(page);
        UiKit.sectionHeader(listCard, R.drawable.ic_gzh_play,
            getString(R.string.replaycheck_pick), false);
        replayList = new LinearLayout(this);
        replayList.setOrientation(LinearLayout.VERTICAL);
        listCard.addView(replayList);

        setContentView(root);
        InsetUtil.applySafeInsets(root);
        updateModeUi();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (pendingLaunch == null) {
            setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        }
        refresh();
    }

    private void updateModeUi() {
        frameRow.setVisibility(mode == MODE_WATCH_FROM ? android.view.View.VISIBLE : android.view.View.GONE);
    }

    private void refresh() {
        resultText.setText(describeLastResult());
        replayList.removeAllViews();

        File dir = new File(DataPackInstaller.userDataDir(), "Replays");
        File[] files = dir.listFiles((d, name) -> name.toLowerCase().endsWith(".rep"));
        if (files == null || files.length == 0) {
            UiKit.supporting(replayList, getString(R.string.replaycheck_none, dir.getAbsolutePath()));
            return;
        }
        // Newest first: the replay just copied over from the PC is the one wanted.
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        DateFormat fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT);
        for (int i = 0; i < files.length; i++) {
            final File f = files[i];
            if (i > 0) {
                UiKit.divider(replayList);
            }
            String note = fmt.format(new Date(f.lastModified())) + " · " + (f.length() / 1024) + " KB";
            UiKit.listRow(replayList, R.drawable.ic_gzh_play, f.getName(), note, () -> launch(f));
        }
    }

    private String describeLastResult() {
        File f = new File(DataPackInstaller.userDataDir(), RESULT_FILE);
        if (!f.isFile()) {
            return getString(R.string.replaycheck_no_result);
        }
        Map<String, String> kv = new HashMap<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) {
                    kv.put(line.substring(0, eq), line.substring(eq + 1));
                }
            }
        } catch (IOException e) {
            return getString(R.string.replaycheck_no_result);
        }
        String replay = kv.getOrDefault("replay", "?");
        String frames = kv.getOrDefault("frames", "?");
        String checkpoints = kv.getOrDefault("checkpoints", "0");
        String matched = kv.getOrDefault("matched", "0");
        String seconds = kv.getOrDefault("seconds", "?");
        String first = kv.getOrDefault("first_mismatch", "0");
        String lastOk = kv.getOrDefault("last_matched", "0");
        if (!"0".equals(first)) {
            return getString(R.string.replaycheck_result_mismatch, replay, first, lastOk,
                matched, checkpoints, frames, seconds);
        }
        return getString(R.string.replaycheck_result_match, replay, matched, checkpoints, frames, seconds);
    }

    private void launch(File replay) {
        String gamePath = SetupActivity.getSavedGamePath(this);
        if (gamePath == null) {
            Toast.makeText(this, R.string.replaycheck_no_game_folder, Toast.LENGTH_LONG).show();
            return;
        }

        int fastTo = -1;
        boolean autoQuit = true;
        if (mode == MODE_WATCH_FROM) {
            autoQuit = false;
            try {
                fastTo = Integer.parseInt(frameInput.getText().toString().trim());
            } catch (NumberFormatException e) {
                fastTo = 0;
            }
            if (fastTo <= 0) {
                Toast.makeText(this, R.string.replaycheck_need_frame, Toast.LENGTH_LONG).show();
                return;
            }
        }

        // The comparison and all of its diagnostics print only with the network trace on.
        File marker = new File(gamePath, NET_TRACE_MARKER);
        if (!marker.isFile()) {
            try {
                marker.createNewFile();
                Toast.makeText(this, R.string.replaycheck_trace_enabled, Toast.LENGTH_SHORT).show();
            } catch (IOException ignored) {
                Toast.makeText(this, R.string.replaycheck_trace_failed, Toast.LENGTH_LONG).show();
            }
        }
        // A stale result must not be mistaken for this run's.
        new File(DataPackInstaller.userDataDir(), RESULT_FILE).delete();

        Intent intent = new Intent(this, GeneralsZHActivity.class);
        intent.putExtra(GeneralsZHActivity.EXTRA_REPLAY, replay.getName());
        intent.putExtra(GeneralsZHActivity.EXTRA_FAST_TO, fastTo);
        intent.putExtra(GeneralsZHActivity.EXTRA_AUTO_QUIT, autoQuit);

        // Same as SetupActivity.onLaunchGame(): rotate first, start once the OS has
        // confirmed landscape, so the game's window-size probe never sees portrait.
        if (getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE) {
            startActivity(intent);
            return;
        }
        pendingLaunch = intent;
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (pendingLaunch != null && newConfig.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            Intent intent = pendingLaunch;
            pendingLaunch = null;
            startActivity(intent);
        }
    }
}
