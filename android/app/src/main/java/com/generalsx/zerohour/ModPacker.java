/*
**  Command & Conquer Generals Zero Hour(tm)
**  Copyright 2025 Electronic Arts Inc.
**
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
*/

package com.generalsx.zerohour;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * GeneralsX @feature Android port 18/10/2026 Loose files in a mod folder (PLAN-027): a mod laid
 * out as in the game -- Data/INI/..., Art/..., Window/..., Maps/... -- rather than as .big files.
 * The engine mounts a mod's .big files only, so the launcher packs those loose files into one
 * archive in the mod folder, !!loose_files.big, rebuilt whenever a loose file is newer. Its name
 * sorts before the mod's other archives, so -- as loose files over archives in the game itself --
 * they win over the mod's own .big files. The originals stay where they are.
 */
final class ModPacker {
    static final String PACKED_NAME = "!!loose_files.big";

    private ModPacker() {}

    /**
     * GeneralsX @bugfix Android port 25/10/2026 Some mods ship their archives as .gib (ShockWave and
     * others): the same BIG archives, renamed so that other installs ignore them, and renamed back to
     * .big by the mod's own launcher at start. The engine mounts .big only, so those mods started as
     * plain Zero Hour (4PDA reports). The launcher does what the mod's launcher would: renames them,
     * once, here and on import. A .gib whose .big already exists is left alone. Returns how many.
     */
    static int renameGibArchives(File dir) {
        int renamed = 0;
        File[] children = dir.listFiles();
        if (children == null) {
            return 0;
        }
        for (File c : children) {
            if (java.nio.file.Files.isSymbolicLink(c.toPath())) {
                continue;
            }
            if (c.isDirectory()) {
                renamed += renameGibArchives(c);
            } else if (c.getName().toLowerCase(Locale.US).endsWith(".gib")) {
                String name = c.getName();
                File big = new File(dir, name.substring(0, name.length() - 4) + ".big");
                if (!big.exists() && c.renameTo(big)) {
                    renamed++;
                }
            }
        }
        return renamed;
    }

    /** Packs the loose game files of a mod folder when needed. Returns how many it packed. */
    static int ensurePacked(File modDir) throws IOException {
        renameGibArchives(modDir);
        List<File> files = new ArrayList<>();
        List<String> names = new ArrayList<>();
        File[] top = modDir.listFiles();
        if (top == null) {
            return 0;
        }
        long newest = 0;
        for (File child : top) {
            if (child.isDirectory() && !child.getName().equalsIgnoreCase("screenshots")
                    && !child.getName().startsWith(".")) {
                newest = Math.max(newest, collect(child, child.getName(), files, names, 0));
            }
        }
        File packed = new File(modDir, PACKED_NAME);
        if (files.isEmpty()) {
            packed.delete();
            return 0;
        }
        if (packed.isFile() && packed.lastModified() >= newest) {
            return files.size();
        }
        write(packed, files, names);
        return files.size();
    }

    private static long collect(File dir, String path, List<File> files, List<String> names, int depth) {
        long newest = dir.lastModified();
        File[] children = dir.listFiles();
        if (children == null || depth > 16) {
            return newest;
        }
        for (File c : children) {
            if (java.nio.file.Files.isSymbolicLink(c.toPath())) {
                continue;
            }
            String childPath = path + "\\" + c.getName();
            if (c.isDirectory()) {
                newest = Math.max(newest, collect(c, childPath, files, names, depth + 1));
            } else if (!c.getName().toLowerCase(Locale.US).endsWith(".big")
                    && !c.getName().toLowerCase(Locale.US).endsWith(".gib")) {
                files.add(c);
                names.add(childPath);
                newest = Math.max(newest, c.lastModified());
            }
        }
        return newest;
    }

    /**
     * BIGF: "BIGF", archive size (little-endian), file count and header size (big-endian), then
     * per file its offset and size (big-endian) and zero-terminated path; the data after that.
     */
    private static void write(File out, List<File> files, List<String> names) throws IOException {
        int header = 16;
        List<byte[]> encoded = new ArrayList<>();
        for (String name : names) {
            byte[] b = name.getBytes(StandardCharsets.ISO_8859_1);
            encoded.add(b);
            header += 8 + b.length + 1;
        }
        long total = header;
        for (File f : files) {
            total += f.length();
        }
        if (total > 0x7fffffffL) {
            throw new IOException("Loose files too large to pack: " + total);
        }
        File temp = new File(out.getParentFile(), out.getName() + ".tmp");
        try (OutputStream o = new BufferedOutputStream(new FileOutputStream(temp), 64 * 1024)) {
            o.write(new byte[] { 'B', 'I', 'G', 'F' });
            writeLE(o, (int) total);
            writeBE(o, files.size());
            writeBE(o, header);
            long offset = header;
            for (int i = 0; i < files.size(); i++) {
                writeBE(o, (int) offset);
                writeBE(o, (int) files.get(i).length());
                o.write(encoded.get(i));
                o.write(0);
                offset += files.get(i).length();
            }
            byte[] buf = new byte[64 * 1024];
            for (File f : files) {
                try (InputStream in = new FileInputStream(f)) {
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        o.write(buf, 0, n);
                    }
                }
            }
        } catch (IOException e) {
            temp.delete();
            throw e;
        }
        if (!temp.renameTo(out)) {
            out.delete();
            if (!temp.renameTo(out)) {
                temp.delete();
                throw new IOException("Could not finish " + out);
            }
        }
    }

    private static void writeBE(OutputStream o, int v) throws IOException {
        o.write(v >>> 24);
        o.write(v >>> 16);
        o.write(v >>> 8);
        o.write(v);
    }

    private static void writeLE(OutputStream o, int v) throws IOException {
        o.write(v);
        o.write(v >>> 8);
        o.write(v >>> 16);
        o.write(v >>> 24);
    }
}
