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

// GeneralsX @feature Android port 04/10/2026 Rooms: the game files a match depends on.
//
// Players of a room compare these before launch (docs/port/ROOMS_PROTOCOL.md, "Files"), so a
// different INI or archive is named before the match instead of ending it in a desync: the
// .big archives of the game folder (and, for Zero Hour, of the base Generals folder it loads
// first) and loose files under Data/INI, which override the archives. SHA-256 of a few
// gigabytes takes a while on a phone, so a digest is kept until the file's size or time changes.

package com.generalsx.zerohour;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

final class RoomFiles {
    interface Progress {
        void file(String name);
    }

    private static final String PREFS = "room_files";

    private RoomFiles() {
    }

    /** path (relative, "base/" for the base Generals folder) -> lowercase hex SHA-256. */
    static Map<String, String> compute(Context ctx, String game, Progress progress) throws IOException {
        Map<String, String> out = new TreeMap<>();
        String gamePath = SetupActivity.getSavedGamePath(ctx, game);
        if (gamePath == null) {
            throw new IOException("no game folder");
        }
        File root = new File(gamePath);
        SharedPreferences cache = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        SharedPreferences.Editor edit = cache.edit();
        for (File f : matchFiles(root)) {
            add(out, "", root, f, cache, edit, progress);
        }
        if (SetupActivity.GAME_ZERO_HOUR.equals(game)) {
            String base = ctx.getSharedPreferences(SetupActivity.PREFS_NAME, Context.MODE_PRIVATE)
                .getString(SetupActivity.PREF_BASE_GENERALS_PATH, null);
            if (base != null) {
                File baseRoot = new File(base);
                for (File f : listBig(baseRoot)) {
                    add(out, "base/", baseRoot, f, cache, edit, progress);
                }
            }
        }
        edit.apply();
        return out;
    }

    private static List<File> matchFiles(File root) {
        List<File> files = listBig(root);
        File ini = new File(new File(root, "Data"), "INI");
        collect(ini, files);
        return files;
    }

    private static List<File> listBig(File dir) {
        List<File> files = new ArrayList<>();
        File[] list = dir.listFiles();
        if (list != null) {
            for (File f : list) {
                if (f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".big")) {
                    files.add(f);
                }
            }
        }
        return files;
    }

    private static void collect(File dir, List<File> into) {
        File[] list = dir.listFiles();
        if (list == null) {
            return;
        }
        for (File f : list) {
            if (f.isDirectory()) {
                collect(f, into);
            } else if (f.isFile()) {
                into.add(f);
            }
        }
    }

    private static void add(Map<String, String> out, String prefix, File root, File f,
                            SharedPreferences cache, SharedPreferences.Editor edit, Progress progress)
            throws IOException {
        String rel = prefix + root.toURI().relativize(f.toURI()).getPath();
        String key = f.getAbsolutePath();
        String stamp = f.length() + ":" + f.lastModified() + ":";
        String cached = cache.getString(key, "");
        String digest;
        if (cached.startsWith(stamp)) {
            digest = cached.substring(stamp.length());
        } else {
            if (progress != null) {
                progress.file(rel);
            }
            digest = sha256(f);
            edit.putString(key, stamp + digest);
        }
        out.put(rel.toLowerCase(Locale.ROOT), digest);
    }

    private static String sha256(File f) throws IOException {
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
        byte[] buf = new byte[1 << 20];
        try (InputStream in = new FileInputStream(f)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                if (Thread.interrupted()) {
                    throw new java.io.InterruptedIOException();
                }
                md.update(buf, 0, n);
            }
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest()) {
            sb.append(String.format(Locale.ROOT, "%02x", b));
        }
        return sb.toString();
    }
}
