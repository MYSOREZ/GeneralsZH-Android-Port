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

// GeneralsX @feature Android port 13/09/2026
//
// The screen around NetworkDiagnostics. Deliberately three controls and a
// monospace report: run it, read it, send it. Anyone who opens this screen is
// already having a bad time, and the useful thing to do with the result is
// paste it into a bug report, so Share is a first-class action rather than
// something in a menu.

package com.generalsx.zerohour;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;

import java.io.File;

public class NetworkDiagnosticsActivity extends Activity {

    private final Handler handler = new Handler(Looper.getMainLooper());

    private TextView reportText;
    private MaterialButton runButton;
    private MaterialButton shareButton;
    private String report = "";
    private boolean running = false;

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.netdiag_title);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(UiKit.color(this, R.color.gzh_background));

        UiKit.appBar(root, getString(R.string.setup_window_title),
            getString(R.string.netdiag_title), 0, null, null);

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.VERTICAL);
        int gutter = UiKit.dim(this, R.dimen.gzh_gutter);
        actions.setPadding(gutter, 0, gutter, 0);
        root.addView(actions, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        runButton = UiKit.button(actions, UiKit.BTN_PRIMARY, R.drawable.ic_gzh_wrench,
            getString(R.string.netdiag_button_run), this::runSweep);

        LinearLayout buttonRow = UiKit.buttonRow(actions);
        shareButton = UiKit.button(buttonRow, UiKit.BTN_TONAL, R.drawable.ic_gzh_share,
            getString(R.string.netdiag_button_share), this::shareReport);
        UiKit.share(shareButton, true);
        UiKit.share(UiKit.button(buttonRow, UiKit.BTN_TONAL, R.drawable.ic_gzh_copy,
            getString(R.string.netdiag_button_copy), this::copyReport), false);
        shareButton.setEnabled(false);

        MaterialCardView card = new MaterialCardView(this);
        card.setRadius(UiKit.dim(this, R.dimen.gzh_radius_card));
        card.setCardElevation(0f);
        card.setCardBackgroundColor(UiKit.color(this, R.color.gzh_surface_container));
        card.setStrokeWidth(0);
        card.setUseCompatPadding(false);
        card.setPreventCornerOverlap(false);

        ScrollView scroll = new ScrollView(this);
        reportText = new TextView(this);
        reportText.setTextIsSelectable(true);
        int pad = UiKit.dim(this, R.dimen.gzh_item_gap);
        reportText.setPadding(pad, pad, pad, pad);
        reportText.setTypeface(android.graphics.Typeface.MONOSPACE);
        reportText.setTextSize(11);
        reportText.setTextColor(UiKit.color(this, R.color.gzh_on_surface_variant));
        reportText.setText(R.string.netdiag_intro);
        scroll.addView(reportText);
        card.addView(scroll, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        FrameLayout host = new FrameLayout(this);
        host.setPadding(gutter, UiKit.dim(this, R.dimen.gzh_card_gap), gutter,
            UiKit.dim(this, R.dimen.gzh_card_gap));
        host.addView(card, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        root.addView(host, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
        InsetUtil.applySafeInsets(root);
    }

    // Every probe here blocks on the network, so the whole sweep runs on one
    // background thread and reports each step back as it starts -- a sweep
    // with an unreachable host takes tens of seconds, and a screen that says
    // nothing for that long reads as a hang.
    private void runSweep() {
        if (running) {
            return;
        }
        running = true;
        runButton.setEnabled(false);
        shareButton.setEnabled(false);
        reportText.setText(R.string.netdiag_running);

        new Thread(() -> {
            String result = NetworkDiagnostics.run(this,
                label -> handler.post(() ->
                    reportText.setText(getString(R.string.netdiag_running_step, label))));
            handler.post(() -> {
                running = false;
                runButton.setEnabled(true);
                shareButton.setEnabled(true);
                report = result;
                reportText.setText(result);
            });
        }, "NetworkDiagnostics").start();
    }

    private void copyReport() {
        if (report.isEmpty()) {
            return;
        }
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText(getString(R.string.netdiag_title), report));
        Toast.makeText(this, R.string.netdiag_toast_copied, Toast.LENGTH_SHORT).show();
    }

    // Shares the full trace file rather than just the on-screen report: the
    // file also holds the request/response lines from real sign-in attempts
    // (and from the game, when its marker is on), which is the part that
    // explains a failure the sweep itself cannot reproduce.
    private void shareReport() {
        File log = NetworkTrace.logFile(this);
        if (!log.isFile()) {
            copyReport();
            return;
        }
        try {
            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", log);
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("text/plain");
            share.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.netdiag_share_subject));
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, getString(R.string.netdiag_button_share)));
        } catch (Exception e) {
            Toast.makeText(this,
                getString(R.string.netdiag_toast_share_failed, e.getMessage()),
                Toast.LENGTH_LONG).show();
        }
    }
}
