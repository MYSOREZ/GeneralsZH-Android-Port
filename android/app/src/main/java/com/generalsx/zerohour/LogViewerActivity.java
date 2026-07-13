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

// GeneralsX @build Android port 07/07/2026
//
// Shows the two log sources without adb:
//  - crash.log (getFilesDir(), internal storage) — written by
//    AndroidCrashHandler.cpp's signal handler, which installs before any
//    engine code runs, so it captures crashes the regular engine log never
//    gets a chance to record.
//  - generals-stderr.log / -prev.log (getExternalFilesDir(), external
//    storage) — the regular engine log, written once main() starts
//    (SDL3Main.cpp).
// A Share button hands the combined text to any app the user has (Files,
// email, a messaging app) via the standard Android share sheet — the
// no-computer path to getting a log out of the phone.
//
// GeneralsX @bugfix Android port 11/07/2026 Share used to be
// Intent.EXTRA_TEXT (inline text) -- only apps that accept plain text show
// up as targets (no "Save to..."), and a big log risks silently failing to
// launch anything at all (TransactionTooLargeException). Now writes the log
// to a real file and shares it via FileProvider instead, which both apps
// that save files and apps that accept text can handle. The old "Refresh"
// button reread files that, in practice, are never still being written
// while this screen is open (the crashed/exited process is dead by the
// time you get here) -- replaced with "Clear Logs" instead, which is what
// people actually wanted a button for.

package com.generalsx.zerohour;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.Reader;
import java.nio.charset.StandardCharsets;
import java.nio.CharBuffer;

public class LogViewerActivity extends Activity {

    // Cap how much we read/display: crash-looping sessions can produce
    // megabytes, and this is a plain TextView, not a virtualized log viewer.
    private static final int MAX_CHARS_PER_FILE = 200_000;

    private String combinedLog = "";

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.logviewer_title);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        LinearLayout buttonRow = new LinearLayout(this);
        buttonRow.setOrientation(LinearLayout.HORIZONTAL);
        buttonRow.setPadding(dp(8), dp(8), dp(8), dp(8));

        // GeneralsX @bugfix Android port 13/07/2026 WRAP_CONTENT height let
        // each button size to its own text independently -- fine in English,
        // but a translation that wraps to two lines (e.g. Ukrainian
        // "Очистити логи") made that one button taller than its siblings,
        // breaking the row's bottom edge. MATCH_PARENT makes every button in
        // the row stretch to the tallest sibling's height instead.
        Button clearButton = new Button(this);
        clearButton.setText(R.string.logviewer_button_clear);
        clearButton.setOnClickListener(v -> confirmClearLogs());
        buttonRow.addView(clearButton, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));

        Button copyButton = new Button(this);
        copyButton.setText(R.string.logviewer_button_copy);
        copyButton.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText(getString(R.string.logviewer_share_subject), combinedLog));
            Toast.makeText(this, R.string.logviewer_toast_copied, Toast.LENGTH_SHORT).show();
        });
        buttonRow.addView(copyButton, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));

        Button shareButton = new Button(this);
        shareButton.setText(R.string.logviewer_button_share);
        shareButton.setOnClickListener(v -> shareLogAsFile());
        buttonRow.addView(shareButton, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));

        root.addView(buttonRow);

        ScrollView scroll = new ScrollView(this);
        TextView logText = new TextView(this);
        logText.setId(android.R.id.text1);
        logText.setTextIsSelectable(true);
        logText.setPadding(dp(12), dp(12), dp(12), dp(12));
        logText.setTypeface(android.graphics.Typeface.MONOSPACE);
        logText.setTextSize(11);
        scroll.addView(logText);
        root.addView(scroll, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
        InsetUtil.applySafeInsets(root);
        loadLogs();
    }

    private TextView logText() {
        return findViewById(android.R.id.text1);
    }

    private void loadLogs() {
        StringBuilder sb = new StringBuilder();

        File crashLog = new File(getFilesDir(), "crash.log");
        sb.append(getString(R.string.logviewer_section_crash_log));
        sb.append(crashLog.exists() ? readTail(crashLog) : getString(R.string.logviewer_crash_log_absent));
        sb.append("\n\n");

        File extDir = getExternalFilesDir(null);
        File stderrLog = extDir != null ? new File(extDir, "generals-stderr.log") : null;
        sb.append(getString(R.string.logviewer_section_stderr_log));
        sb.append(stderrLog != null && stderrLog.exists() ? readTail(stderrLog) : getString(R.string.logviewer_stderr_log_absent));
        sb.append("\n\n");

        File prevLog = extDir != null ? new File(extDir, "generals-stderr-prev.log") : null;
        sb.append(getString(R.string.logviewer_section_prev_log));
        sb.append(prevLog != null && prevLog.exists() ? readTail(prevLog) : getString(R.string.logviewer_prev_log_absent));

        combinedLog = sb.toString();
        logText().setText(combinedLog);
    }

    private void confirmClearLogs() {
        new AlertDialog.Builder(this)
            .setTitle(R.string.logviewer_dialog_clear_title)
            .setMessage(R.string.logviewer_dialog_clear_message)
            .setPositiveButton(R.string.logviewer_dialog_clear_confirm, (dialog, which) -> clearLogs())
            .setNegativeButton(R.string.logviewer_dialog_clear_cancel, null)
            .show();
    }

    private void clearLogs() {
        new File(getFilesDir(), "crash.log").delete();
        File extDir = getExternalFilesDir(null);
        if (extDir != null) {
            new File(extDir, "generals-stderr.log").delete();
            new File(extDir, "generals-stderr-prev.log").delete();
        }
        loadLogs();
        Toast.makeText(this, R.string.logviewer_toast_cleared, Toast.LENGTH_SHORT).show();
    }

    private void shareLogAsFile() {
        try {
            File logFile = new File(getCacheDir(), "generalszh-log.txt");
            try (FileOutputStream out = new FileOutputStream(logFile, false)) {
                out.write(combinedLog.getBytes(StandardCharsets.UTF_8));
            }

            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", logFile);

            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("text/plain");
            share.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.logviewer_share_subject));
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, getString(R.string.logviewer_share_chooser_title)));
        } catch (IOException e) {
            Toast.makeText(this, getString(R.string.logviewer_toast_share_failed, e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    private String readTail(File file) {
        try (Reader r = new FileReader(file)) {
            long size = file.length();
            if (size > MAX_CHARS_PER_FILE) {
                r.skip(size - MAX_CHARS_PER_FILE);
            }
            CharBuffer buf = CharBuffer.allocate(MAX_CHARS_PER_FILE);
            r.read(buf);
            buf.flip();
            return (size > MAX_CHARS_PER_FILE ? getString(R.string.logviewer_truncated_notice) : "") + buf.toString();
        } catch (IOException e) {
            return getString(R.string.logviewer_read_failed, file.getAbsolutePath(), e.getMessage());
        }
    }

    private int dp(int value) {
        float density = getResources().getDisplayMetrics().density;
        return (int) (value * density + 0.5f);
    }
}
