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
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.FileProvider;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;

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

        // GeneralsX @feature Android port launcher-ui-refresh 06/09/2026
        // Same three actions, same log, but on the shared palette and type
        // scale: Share is the filled action (getting the log off the phone is
        // the entire point of this screen), Copy is tonal, and Clear -- the
        // only destructive one here -- is a text button so it stops looking
        // like a peer of the other two.
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(LauncherUi.color(this, R.color.gzh_background));

        LinearLayout buttonRow = new LinearLayout(this);
        buttonRow.setOrientation(LinearLayout.HORIZONTAL);
        int pad = LauncherUi.dp(this, LauncherUi.SPACE_3);
        buttonRow.setPadding(pad, pad, pad, LauncherUi.dp(this, LauncherUi.SPACE_2));

        // GeneralsX @bugfix Android port 13/07/2026 WRAP_CONTENT height let
        // each button size to its own text independently -- fine in English,
        // but a translation that wraps to two lines (e.g. Ukrainian
        // "Очистити логи") made that one button taller than its siblings,
        // breaking the row's bottom edge. MATCH_PARENT makes every button in
        // the row stretch to the tallest sibling's height instead.
        addRowButton(buttonRow, LauncherUi.BUTTON_TEXT, R.string.logviewer_button_clear,
            this::confirmClearLogs);
        addRowButton(buttonRow, LauncherUi.BUTTON_TONAL, R.string.logviewer_button_copy, () -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText(getString(R.string.logviewer_share_subject), combinedLog));
            Toast.makeText(this, R.string.logviewer_toast_copied, Toast.LENGTH_SHORT).show();
        });
        addRowButton(buttonRow, LauncherUi.BUTTON_FILLED, R.string.logviewer_button_share,
            this::shareLogAsFile);

        root.addView(buttonRow);

        MaterialCardView logCard = new MaterialCardView(this);
        logCard.setRadius(LauncherUi.dp(this, LauncherUi.CARD_RADIUS));
        logCard.setCardElevation(0f);
        logCard.setStrokeWidth(LauncherUi.dp(this, 1));
        logCard.setStrokeColor(LauncherUi.color(this, R.color.gzh_outline_variant));
        logCard.setCardBackgroundColor(LauncherUi.color(this, R.color.gzh_surface));
        LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        cardLp.setMargins(pad, 0, pad, pad);
        logCard.setLayoutParams(cardLp);

        ScrollView scroll = new ScrollView(this);
        TextView logText = new TextView(this);
        logText.setId(android.R.id.text1);
        logText.setTextIsSelectable(true);
        logText.setTextColor(LauncherUi.color(this, R.color.gzh_on_surface));
        int inner = LauncherUi.dp(this, LauncherUi.SPACE_3);
        logText.setPadding(inner, inner, inner, inner);
        logText.setTypeface(android.graphics.Typeface.MONOSPACE);
        logText.setTextSize(11);
        scroll.addView(logText);
        logCard.addView(scroll);
        root.addView(logCard);

        setContentView(root);
        InsetUtil.applySafeInsets(root);
        loadLogs();
    }

    // Buttons in a horizontal row need weighted layout params, which
    // LauncherUi.button()'s full-width default does not give them.
    private void addRowButton(LinearLayout row, int variant, int labelRes, Runnable action) {
        MaterialButton button = LauncherUi.button(row, variant, getString(labelRes), action);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.MATCH_PARENT, 1f);
        lp.setMargins(LauncherUi.dp(this, 2), 0, LauncherUi.dp(this, 2), 0);
        button.setLayoutParams(lp);
        // A three-button row in a language with long words has no space for
        // Material's default horizontal padding as well as the text.
        button.setPadding(LauncherUi.dp(this, LauncherUi.SPACE_2), button.getPaddingTop(),
            LauncherUi.dp(this, LauncherUi.SPACE_2), button.getPaddingBottom());
        button.setInsetTop(0);
        button.setInsetBottom(0);
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
    // the middle.
    //
    // GeneralsX @bugfix Android port 31/07/2026 follow-up: a first attempt
    // shared the real files unmodified as separate attachments
    // (ACTION_SEND_MULTIPLE), which does avoid the truncation, but a tester
    // pointed out that's worse to actually receive -- several separate
    // files instead of one, and no compression, so a real session's
    // generals-stderr.log alone came in over the ~30 MB limit some chat
    // apps enforce per upload even though it compresses down to a fraction
    // of that (plain-text logs, especially this engine's repetitive
    // per-field/per-frame tracing, compress extremely well). Zip everything
    // into a single archive instead: one attachment, no receiving-app size
    // surprise, and FileProvider still has no size limit of its own on the
    // sharing side (see the FileProvider migration comment at the top of
    // this file) -- the receiving app decides what it can handle, not us.
    private void shareLogAsFile() {
        try {
            File zipFile = new File(getCacheDir(), "generalszh-logs.zip");
            int fileCount = 0;
            try (java.util.zip.ZipOutputStream zos = new java.util.zip.ZipOutputStream(
                    new java.io.FileOutputStream(zipFile, false))) {
                fileCount += addLogFileToZip(zos, new File(getFilesDir(), "crash.log"));
                fileCount += addLogFileToZip(zos, new File(getFilesDir(), "crash-prev.log"));
                File extDir = getExternalFilesDir(null);
                if (extDir != null) {
                    fileCount += addLogFileToZip(zos, new File(extDir, "generals-stderr.log"));
                    fileCount += addLogFileToZip(zos, new File(extDir, "generals-stderr-prev.log"));
                }
            }

            if (fileCount == 0) {
                zipFile.delete();
                Toast.makeText(this, R.string.logviewer_toast_share_none, Toast.LENGTH_LONG).show();
                return;
            }

            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", zipFile);

            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("application/zip");
            share.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.logviewer_share_subject));
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, getString(R.string.logviewer_share_chooser_title)));
        } catch (IOException e) {
            Toast.makeText(this, getString(R.string.logviewer_toast_share_failed, e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    // Returns 1 if the file existed and was added, 0 if it didn't exist (so
    // the caller can tell "nothing to share" apart from "wrote an empty zip").
    private int addLogFileToZip(java.util.zip.ZipOutputStream zos, File f) throws IOException {
        if (!f.isFile()) {
            return 0;
        }
        zos.putNextEntry(new java.util.zip.ZipEntry(f.getName()));
        try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                zos.write(buf, 0, n);
            }
        }
        zos.closeEntry();
        return 1;
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
}
