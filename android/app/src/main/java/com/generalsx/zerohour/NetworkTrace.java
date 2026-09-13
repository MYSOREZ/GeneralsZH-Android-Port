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
// One append-only text log shared by the launcher's GeneralsOnline calls and
// by the game's own HTTP layer (the engine appends to the same file via the
// gx_net_trace.txt marker -- see HTTPRequest.cpp). Sign-in spans both
// processes: the launcher does the browser/code exchange, the game reuses the
// resulting token, and until now a failure in either half produced one
// on-screen sentence and nothing to read afterwards.
//
// Everything a support request needs is therefore in one file in one order:
// launcher requests, game requests, and the diagnostic sweep, interleaved by
// timestamp.
//
// Redaction is not optional here. The log is written to be shared -- with us,
// on a forum, in a Discord thread -- and the bodies it carries contain session
// and refresh tokens, which ARE the account until they expire. redact() below
// strips them on the way in, so a log is safe to post even if the person
// posting it never looks at it.

package com.generalsx.zerohour;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class NetworkTrace {

    private static final String TAG = "NetworkTrace";

    /** Same directory the stderr logs live in, so LogViewer's share picks it up. */
    static final String LOG_NAME = "generals-network.log";

    /**
     * Marker file that switches the ENGINE side of this log on. The launcher
     * side is always on: it is a handful of lines per sign-in and the failure
     * it diagnoses is, by construction, one the user cannot reproduce for us
     * on demand. The engine side is per-request during matchmaking, so it
     * stays opt-in and lives with the other gx_*.txt markers in the game
     * folder (SetupActivity's Diagnostics card toggles it).
     */
    static final String ENGINE_MARKER = "gx_net_trace.txt";

    /** Keep the log bounded; it is shared by hand, not rotated by a service. */
    private static final long MAX_BYTES = 512 * 1024;

    private static final Object LOCK = new Object();

    private static final SimpleDateFormat STAMP =
        new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US);

    // A bearer token in a header we built, and the two token fields the auth
    // API returns. Both shapes appear in this log (request headers, response
    // bodies), and both are credentials.
    private static final Pattern BEARER =
        Pattern.compile("(Bearer\\s+)([A-Za-z0-9._\\-+/=]{8,})");
    private static final Pattern TOKEN_FIELD =
        Pattern.compile("(\"(?:session_token|refresh_token|access_token)\"\\s*:\\s*\")([^\"]{8,})(\")");

    private NetworkTrace() {
    }

    static File logFile(Context ctx) {
        File dir = ctx.getExternalFilesDir(null);
        if (dir == null) {
            dir = ctx.getFilesDir();
        }
        return new File(dir, LOG_NAME);
    }

    /**
     * Replaces credential material with a length-tagged placeholder. The
     * length is kept because "did we send a token at all, and was it the
     * shape we expected" is exactly the question a login trace has to
     * answer -- "(token, 128 chars)" answers it without handing the token
     * to whoever reads the log.
     */
    static String redact(String text) {
        if (text == null || text.isEmpty()) {
            return "";
        }
        StringBuffer out = new StringBuffer(text.length());
        Matcher m = BEARER.matcher(text);
        while (m.find()) {
            m.appendReplacement(out,
                Matcher.quoteReplacement(m.group(1) + "<token, " + m.group(2).length() + " chars>"));
        }
        m.appendTail(out);

        String once = out.toString();
        out = new StringBuffer(once.length());
        m = TOKEN_FIELD.matcher(once);
        while (m.find()) {
            m.appendReplacement(out,
                Matcher.quoteReplacement(m.group(1) + "<token, " + m.group(2).length() + " chars>" + m.group(3)));
        }
        m.appendTail(out);
        return out.toString();
    }

    static void write(Context ctx, String line) {
        if (ctx == null) {
            return;
        }
        synchronized (LOCK) {
            try {
                File f = logFile(ctx);
                if (f.length() > MAX_BYTES) {
                    // Single generation, no .1/.2 chain: this log is read by
                    // being sent to us, and the interesting event is always
                    // the most recent attempt.
                    File prev = new File(f.getParentFile(), LOG_NAME + ".prev");
                    prev.delete();
                    f.renameTo(prev);
                }
                try (PrintWriter w = new PrintWriter(new FileWriter(f, true))) {
                    w.print(STAMP.format(new Date()));
                    w.print("  ");
                    w.println(redact(line));
                }
            } catch (IOException e) {
                Log.w(TAG, "could not append to network log: " + e.getMessage());
            }
        }
    }

    /** A blank line plus a titled banner, so separate attempts stay readable. */
    static void section(Context ctx, String title) {
        write(ctx, "");
        write(ctx, "===== " + title + " =====");
    }

    static void clear(Context ctx) {
        synchronized (LOCK) {
            logFile(ctx).delete();
            File dir = ctx.getExternalFilesDir(null);
            if (dir != null) {
                new File(dir, LOG_NAME + ".prev").delete();
            }
        }
    }

    /**
     * Body snippets go in the log verbatim apart from redaction, but a
     * response body can be a whole HTML error page; cap it so one failure
     * cannot push the rest of the attempt out of the file.
     */
    static String snippet(String body, int max) {
        if (body == null) {
            return "";
        }
        String flat = body.replace('\n', ' ').replace('\r', ' ').trim();
        if (flat.length() <= max) {
            return flat;
        }
        return flat.substring(0, max) + "... (" + flat.length() + " bytes total)";
    }
}
