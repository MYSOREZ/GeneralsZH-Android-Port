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

import android.content.Context;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * GeneralsX @feature Android port 18/10/2026 What the mods on the Mods screen are and how they
 * get along (PLAN-027): read from the file tables of their .big archives, never their contents.
 *
 * <ul>
 *   <li>Kind: total conversion, gameplay, cosmetic, maps or language -- from mod.ini's Kind, else
 *   from which folders the archives fill.</li>
 *   <li>Game: Zero Hour, Generals or both -- from mod.ini's Game, else by comparing the mod's INI
 *   paths with the two games' own INI archives (whichever of the two game folders are set).</li>
 *   <li>Problems among the switched-on mods, in their order, each RED (will not start together),
 *   AMBER (changes gameplay, check it) or GREY (cosmetic overlap): two total conversions; a mod
 *   for the other game; Requires / ConflictsWith in mod.ini; a pair the signed settings know to
 *   break (mod_conflicts); INI files changed by two mods; art/window files changed by two.</li>
 * </ul>
 * File tables are cached per archive (path, size, time), so re-rendering the screen is cheap.
 */
final class ModAnalysis {
    static final int KIND_TOTAL = 0;
    static final int KIND_GAMEPLAY = 1;
    static final int KIND_COSMETIC = 2;
    static final int KIND_MAPS = 3;
    static final int KIND_LANGUAGE = 4;

    static final int GAME_UNKNOWN = 0;
    static final int GAME_ZH = 1;
    static final int GAME_GENERALS = 2;
    static final int GAME_BOTH = 3;

    static final int LEVEL_GREY = 0;
    static final int LEVEL_AMBER = 1;
    static final int LEVEL_RED = 2;

    // A mod that changes this many INI files rebuilds the game rather than adjusting it.
    private static final int TOTAL_CONVERSION_INI_FILES = 150;

    /** What one mod folder holds. */
    static final class Info {
        final Set<String> files = new HashSet<>();
        int iniFiles, mapFiles, artFiles, windowFiles, textFiles;
        int kind;
        int game;

        boolean hasIni() {
            return iniFiles > 0;
        }
    }

    /** One problem, shown on the card of {@code mod} (the upper one of a pair). */
    static final class Problem {
        final ModManager.Mod mod;
        final int level;
        final String text;

        Problem(ModManager.Mod mod, int level, String text) {
            this.mod = mod;
            this.level = level;
            this.text = text;
        }
    }

    private static final Map<String, List<String>> sTables = new HashMap<>();
    private static final Map<String, Set<String>> sGameIni = new HashMap<>();

    private ModAnalysis() {}

    static Info analyze(Context ctx, ModManager.Mod mod, Map<String, String> ini) {
        Info info = new Info();
        for (File big : bigFiles(mod.dir)) {
            info.files.addAll(table(big));
        }
        for (String f : info.files) {
            if (f.startsWith("data\\ini\\") && f.endsWith(".ini")) {
                info.iniFiles++;
            } else if (f.startsWith("maps\\")) {
                info.mapFiles++;
            } else if (f.startsWith("window\\")) {
                info.windowFiles++;
            } else if (f.endsWith(".csf") || f.endsWith(".str")) {
                info.textFiles++;
            } else {
                info.artFiles++;
            }
        }
        info.kind = kindOf(ini.get("kind"), info);
        info.game = gameOf(ctx, ini.get("game"), info);
        return info;
    }

    private static int kindOf(String declared, Info info) {
        if (declared != null) {
            switch (declared.trim().toLowerCase(Locale.US)) {
                case "totalconversion": return KIND_TOTAL;
                case "gameplay": return KIND_GAMEPLAY;
                case "cosmetic": return KIND_COSMETIC;
                case "maps": return KIND_MAPS;
                case "language": return KIND_LANGUAGE;
                default: break;
            }
        }
        if (info.iniFiles >= TOTAL_CONVERSION_INI_FILES) {
            return KIND_TOTAL;
        }
        if (info.iniFiles > 0) {
            return KIND_GAMEPLAY;
        }
        if (info.mapFiles > 0 && info.artFiles == 0 && info.windowFiles == 0) {
            return KIND_MAPS;
        }
        if (info.textFiles > 0 && info.artFiles == 0 && info.windowFiles == 0 && info.mapFiles == 0) {
            return KIND_LANGUAGE;
        }
        return KIND_COSMETIC;
    }

    private static int gameOf(Context ctx, String declared, Info info) {
        if (declared != null) {
            String d = declared.trim().toLowerCase(Locale.US).replace(" ", "");
            if (d.equals("zerohour") || d.equals("zh")) return GAME_ZH;
            if (d.equals("generals")) return GAME_GENERALS;
            if (d.equals("both")) return GAME_BOTH;
        }
        if (info.iniFiles == 0) {
            return GAME_UNKNOWN;
        }
        // The game whose own INI archive shares more of the mod's INI paths. Both games share most
        // file names, so only a clear lead counts.
        Set<String> zh = gameIni(SetupActivity.getSavedGamePath(ctx, SetupActivity.GAME_ZERO_HOUR), "inizh.big");
        Set<String> gen = gameIni(SetupActivity.getSavedGamePath(ctx, SetupActivity.GAME_GENERALS), "ini.big");
        if (zh == null || gen == null) {
            return GAME_UNKNOWN;
        }
        int inZh = 0, inGen = 0;
        for (String f : info.files) {
            if (f.startsWith("data\\ini\\")) {
                if (zh.contains(f)) inZh++;
                if (gen.contains(f)) inGen++;
            }
        }
        if (inZh > inGen + 5 && inZh > inGen * 1.2) return GAME_ZH;
        if (inGen > inZh + 5 && inGen > inZh * 1.2) return GAME_GENERALS;
        return GAME_UNKNOWN;
    }

    /** The paths in a game's own INI archive (top of the folder or Data\INI), or null. */
    private static Set<String> gameIni(String gamePath, String archive) {
        if (gamePath == null) {
            return null;
        }
        synchronized (sGameIni) {
            Set<String> cached = sGameIni.get(gamePath + "|" + archive);
            if (cached != null) {
                return cached;
            }
            File game = new File(gamePath);
            File[] candidates = { new File(game, archive), new File(new File(new File(game, "Data"), "INI"), archive) };
            for (File f : candidates) {
                File found = findIgnoreCase(f);
                if (found != null) {
                    Set<String> set = new HashSet<>(table(found));
                    sGameIni.put(gamePath + "|" + archive, set);
                    return set;
                }
            }
            return null;
        }
    }

    private static File findIgnoreCase(File f) {
        if (f.isFile()) {
            return f;
        }
        File parent = f.getParentFile();
        File[] siblings = parent == null ? null : parent.listFiles();
        if (siblings != null) {
            for (File s : siblings) {
                if (s.isFile() && s.getName().equalsIgnoreCase(f.getName())) {
                    return s;
                }
            }
        }
        return null;
    }

    static List<File> bigFiles(File dir) {
        List<File> out = new ArrayList<>();
        collectBig(dir, out, 0);
        return out;
    }

    private static void collectBig(File dir, List<File> out, int depth) {
        File[] children = dir.listFiles();
        if (children == null || depth > 8) {
            return;
        }
        for (File c : children) {
            if (java.nio.file.Files.isSymbolicLink(c.toPath())) {
                continue;
            }
            if (c.isDirectory()) {
                collectBig(c, out, depth + 1);
            } else if (c.getName().toLowerCase(Locale.US).endsWith(".big")) {
                out.add(c);
            }
        }
    }

    /**
     * A .big's file table, lower-case with backslashes. BIG layout: "BIGF"/"BIG4", archive size
     * (LE), file count (BE), header size (BE), then per file: offset (BE), size (BE), a
     * zero-terminated path.
     */
    static List<String> table(File big) {
        String key = big.getAbsolutePath() + "|" + big.length() + "|" + big.lastModified();
        synchronized (sTables) {
            List<String> cached = sTables.get(key);
            if (cached != null) {
                return cached;
            }
        }
        List<String> paths = new ArrayList<>();
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(big), 64 * 1024))) {
            byte[] magic = new byte[4];
            in.readFully(magic);
            if (magic[0] == 'B' && magic[1] == 'I' && magic[2] == 'G') {
                in.readInt();
                final int count = in.readInt();
                in.readInt();
                StringBuilder path = new StringBuilder();
                for (int i = 0; i < count && count <= 200000; i++) {
                    in.readInt();
                    in.readInt();
                    path.setLength(0);
                    int c;
                    while ((c = in.read()) > 0) {
                        path.append((char) c);
                    }
                    if (c < 0) {
                        break;
                    }
                    paths.add(path.toString().toLowerCase(Locale.US).replace('/', '\\'));
                }
            }
        } catch (IOException e) {
            android.util.Log.w("ModAnalysis", "could not read the file table of " + big, e);
        }
        synchronized (sTables) {
            sTables.put(key, paths);
        }
        return paths;
    }

    /**
     * The problems among the mods switched on, in their order. {@code infos} and {@code inis} are
     * per mod folder path.
     */
    static List<Problem> problems(Context ctx, List<ModManager.Mod> mods, Map<String, Info> infos,
                                  Map<String, Map<String, String>> inis) {
        List<Problem> out = new ArrayList<>();
        List<ModManager.Mod> on = new ArrayList<>();
        for (ModManager.Mod m : mods) {
            if (m.enabled && infos.containsKey(m.dir.getAbsolutePath())) {
                on.add(m);
            }
        }
        boolean generals = SetupActivity.isGeneralsSelected(ctx);
        String known = UpdateManager.remoteConfig(ctx, "mod_conflicts", "").toLowerCase(Locale.US);
        for (int i = 0; i < on.size(); i++) {
            ModManager.Mod a = on.get(i);
            Info ia = infos.get(a.dir.getAbsolutePath());
            Map<String, String> iniA = inis.get(a.dir.getAbsolutePath());
            if ((generals && ia.game == GAME_ZH) || (!generals && ia.game == GAME_GENERALS)) {
                out.add(new Problem(a, LEVEL_RED, ctx.getString(generals
                    ? R.string.mods_problem_for_zh : R.string.mods_problem_for_generals)));
            }
            for (String required : list(iniA.get("requires"))) {
                if (findOn(on, required) == null) {
                    out.add(new Problem(a, LEVEL_RED, ctx.getString(R.string.mods_problem_requires, required)));
                }
            }
            for (int j = i + 1; j < on.size(); j++) {
                ModManager.Mod b = on.get(j);
                Info ib = infos.get(b.dir.getAbsolutePath());
                Map<String, String> iniB = inis.get(b.dir.getAbsolutePath());
                if (ia.kind == KIND_TOTAL && ib.kind == KIND_TOTAL) {
                    out.add(new Problem(a, LEVEL_RED, ctx.getString(R.string.mods_problem_two_total, b.name)));
                    continue;
                }
                if (names(list(iniA.get("conflictswith")), b) || names(list(iniB.get("conflictswith")), a)
                        || knownPair(known, a, b)) {
                    out.add(new Problem(a, LEVEL_RED, ctx.getString(R.string.mods_problem_conflicts, b.name)));
                    continue;
                }
                int iniOverlap = 0, otherOverlap = 0;
                for (String f : ia.files) {
                    if (ib.files.contains(f)) {
                        if (f.startsWith("data\\ini\\")) iniOverlap++; else otherOverlap++;
                    }
                }
                if (iniOverlap > 0) {
                    out.add(new Problem(a, LEVEL_AMBER, ctx.getString(R.string.mods_problem_ini_overlap,
                        b.name, iniOverlap)));
                } else if (otherOverlap > 0) {
                    out.add(new Problem(a, LEVEL_GREY, ctx.getString(R.string.mods_problem_art_overlap,
                        b.name, otherOverlap)));
                }
            }
        }
        return out;
    }

    private static List<String> list(String value) {
        List<String> out = new ArrayList<>();
        if (value != null) {
            for (String part : value.split("[,;]")) {
                if (!part.trim().isEmpty()) {
                    out.add(part.trim());
                }
            }
        }
        return out;
    }

    private static boolean matches(ModManager.Mod mod, String name) {
        return mod.name.equalsIgnoreCase(name) || mod.dir.getName().equalsIgnoreCase(name);
    }

    private static ModManager.Mod findOn(List<ModManager.Mod> on, String name) {
        for (ModManager.Mod m : on) {
            if (matches(m, name)) {
                return m;
            }
        }
        return null;
    }

    private static boolean names(List<String> list, ModManager.Mod mod) {
        for (String n : list) {
            if (matches(mod, n)) {
                return true;
            }
        }
        return false;
    }

    /**
     * mod_conflicts from the signed settings: pairs "a|b" separated by ';', each side a piece of
     * a mod's name (lower case) -- a newly found bad combination is blocked without a new APK.
     */
    private static boolean knownPair(String known, ModManager.Mod a, ModManager.Mod b) {
        if (known.isEmpty()) {
            return false;
        }
        String na = a.name.toLowerCase(Locale.US) + " " + a.dir.getName().toLowerCase(Locale.US);
        String nb = b.name.toLowerCase(Locale.US) + " " + b.dir.getName().toLowerCase(Locale.US);
        for (String pair : known.split(";")) {
            String[] sides = pair.split("\\|");
            if (sides.length != 2 || sides[0].trim().isEmpty() || sides[1].trim().isEmpty()) {
                continue;
            }
            String x = sides[0].trim(), y = sides[1].trim();
            if ((na.contains(x) && nb.contains(y)) || (na.contains(y) && nb.contains(x))) {
                return true;
            }
        }
        return false;
    }
}
