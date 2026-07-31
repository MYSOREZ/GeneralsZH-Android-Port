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
import java.io.FileReader;
import java.io.IOException;
import java.io.Reader;
import java.nio.CharBuffer;

public class LogViewerActivity extends Activity {

    // Cap how much we read/display: crash-looping sessions can produce
    // megabytes, and this is a plain TextView, not a virtualized log viewer.
    private static final int MAX_CHARS_PER_FILE = 200_000;

    // GeneralsX @bugfix Android port 30/07/2026 Of that budget, keep this much
    // from the START of the session and spend the rest on the tail.
    //
    // Tail-only truncation kept losing exactly the part that explains a
    // failure. The startup banner (build number, device, GPU/driver, Vulkan
    // version), the INI load sequence and DXVK init all happen in the first
    // few thousand lines; a per-frame-chatty session then pushes them out
    // long before the user gets around to exporting. Issue #2's 2026-07-26
    // report was the clearest case: 2,681 lines exported, every one of them
    // per-frame render tracing, nothing at all about what went wrong.
    //
    // 60k/140k rather than an even split: the head only has to cover
    // startup, while the tail is where a hang or a repeating error shows up.
    private static final int HEAD_CHARS_PER_FILE = 60_000;

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
        sb.append(crashLog.exists() ? readHeadAndTail(crashLog) : getString(R.string.logviewer_crash_log_absent));
        sb.append("\n\n");

        // GeneralsX @bugfix Android port 30/07/2026 crash.log now rotates to
        // crash-prev.log on every launch (AndroidCrashHandler.cpp) instead of
        // growing forever across reinstalls -- show that previous record too,
        // the same way generals-stderr-prev.log already is below.
        File crashPrevLog = new File(getFilesDir(), "crash-prev.log");
        sb.append(getString(R.string.logviewer_section_crash_prev_log));
        sb.append(crashPrevLog.exists() ? readHeadAndTail(crashPrevLog) : getString(R.string.logviewer_crash_prev_log_absent));
        sb.append("\n\n");

        File extDir = getExternalFilesDir(null);
        File stderrLog = extDir != null ? new File(extDir, "generals-stderr.log") : null;
        sb.append(getString(R.string.logviewer_section_stderr_log));
        sb.append(stderrLog != null && stderrLog.exists() ? readHeadAndTail(stderrLog) : getString(R.string.logviewer_stderr_log_absent));
        sb.append("\n\n");

        File prevLog = extDir != null ? new File(extDir, "generals-stderr-prev.log") : null;
        sb.append(getString(R.string.logviewer_section_prev_log));
        sb.append(prevLog != null && prevLog.exists() ? readHeadAndTail(prevLog) : getString(R.string.logviewer_prev_log_absent));

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
        new File(getFilesDir(), "crash-prev.log").delete();
        File extDir = getExternalFilesDir(null);
        if (extDir != null) {
            new File(extDir, "generals-stderr.log").delete();
            new File(extDir, "generals-stderr-prev.log").delete();
        }
        loadLogs();
        Toast.makeText(this, R.string.logviewer_toast_cleared, Toast.LENGTH_SHORT).show();
    }

    // GeneralsX @bugfix Android port 31/07/2026 Used to share the same
    // head+tail-capped combinedLog the on-screen TextView shows -- fine for
    // the viewer (a 70+ MB session isn't reasonable to lay out in a plain
    // TextView), but that meant Share could never hand over more than the
    // 200 KB cap, and a real multi-minute session with active logging
    // observed on-device was tens of MB, entirely eliding the gameplay in
    // the middle. Rather than add a second button, Share now sends the real
    // on-disk files unmodified (multiple attachments if more than one
    // exists) -- FileProvider has no size limit of its own the way the old
    // raw EXTRA_TEXT approach did (see the FileProvider migration comment
    // at the top of this file), so the receiving app (email, Drive,
    // Telegram, etc.) decides what it can handle, not us.
    private void shareLogAsFile() {
        try {
            java.util.ArrayList<Uri> uris = new java.util.ArrayList<>();
            addLogFileUri(uris, new File(getFilesDir(), "crash.log"));
            addLogFileUri(uris, new File(getFilesDir(), "crash-prev.log"));
            File extDir = getExternalFilesDir(null);
            if (extDir != null) {
                addLogFileUri(uris, new File(extDir, "generals-stderr.log"));
                addLogFileUri(uris, new File(extDir, "generals-stderr-prev.log"));
            }

            if (uris.isEmpty()) {
                Toast.makeText(this, R.string.logviewer_toast_share_none, Toast.LENGTH_LONG).show();
                return;
            }

            Intent share;
            if (uris.size() == 1) {
                share = new Intent(Intent.ACTION_SEND);
                share.putExtra(Intent.EXTRA_STREAM, uris.get(0));
            } else {
                share = new Intent(Intent.ACTION_SEND_MULTIPLE);
                share.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
            }
            share.setType("text/plain");
            share.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.logviewer_share_subject));
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, getString(R.string.logviewer_share_chooser_title)));
        } catch (Exception e) {
            Toast.makeText(this, getString(R.string.logviewer_toast_share_failed, e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    private void addLogFileUri(java.util.List<Uri> list, File f) {
        if (f.isFile()) {
            list.add(FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", f));
        }
    }

    /**
     * Read a log, keeping the beginning of the session as well as the end.
     * Under the cap the file is returned whole; over it, the head and the
     * tail are joined by a marker naming how much was dropped in between.
     */
    private String readHeadAndTail(File file) {
        try (Reader r = new FileReader(file)) {
            long size = file.length();
            if (size <= MAX_CHARS_PER_FILE) {
                return readChars(r, (int) size);
            }

            String head = readChars(r, HEAD_CHARS_PER_FILE);

            int tailChars = MAX_CHARS_PER_FILE - HEAD_CHARS_PER_FILE;
            // size is a byte count and we are reading chars; for the ASCII
            // these logs are made of they match, and being off by a little
            // only shifts the cut, so an estimate is good enough here.
            long elided = size - HEAD_CHARS_PER_FILE - tailChars;
            skipFully(r, elided);

            String tail = readChars(r, tailChars);
            return head + getString(R.string.logviewer_elided_notice, elided) + tail;
        } catch (IOException e) {
            return getString(R.string.logviewer_read_failed, file.getAbsolutePath(), e.getMessage());
        }
    }

    private static String readChars(Reader r, int count) throws IOException {
        if (count <= 0) {
            return "";
        }
        CharBuffer buf = CharBuffer.allocate(count);
        // A single read() can come up short; keep going until the buffer is
        // full or the stream ends, or the tail would start at the wrong offset.
        while (buf.hasRemaining() && r.read(buf) != -1) {
            // keep reading
        }
        buf.flip();
        return buf.toString();
    }

    private static void skipFully(Reader r, long count) throws IOException {
        long remaining = count;
        while (remaining > 0) {
            long skipped = r.skip(remaining);
            if (skipped <= 0) {
                break;
            }
            remaining -= skipped;
        }
    }

    private int dp(int value) {
        float density = getResources().getDisplayMetrics().density;
        return (int) (value * density + 0.5f);
    }
}
