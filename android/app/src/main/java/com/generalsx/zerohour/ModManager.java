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
import android.content.SharedPreferences;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * GeneralsX @feature Android port 18/10/2026 Managed mods (PLAN-027 step 1).
 *
 * One mod = one folder under {@code <game folder>/mods/}, holding its .big files and an optional
 * mod.ini and picture. Each game (Zero Hour, Generals) has its own folder and its own list. The
 * list is an order the player chooses (top wins) with an on/off switch per mod, kept in the
 * launcher's preferences by folder name. Right before a game starts, {@link #writeActiveList}
 * gives the engine the enabled folders, highest priority first (GXManagedMods.h); the engine
 * mounts them above the game's archives and above legacy '!' archives in the game folder.
 *
 * The folder-safety checks (direct child, no symbolic links) and the transactional importer
 * (ModImportService) come from the GeneralsX fork by realkot, used with permission.
 */
final class ModManager {
    static final String MODS_DIR = "mods";
    static final String ACTIVE_LIST_FILE = "mods_active.txt";
    private static final String PREF_ORDER = "mods_order";
    private static final String PREF_ORDER_GENERALS = "mods_order_generals";
    private static final String[] PICTURE_NAMES = { "cover", "preview", "logo", "icon" };
    private static final String[] PICTURE_EXTENSIONS = { ".png", ".jpg", ".jpeg", ".webp" };

    /** One mod folder as the Mods screen shows it. */
    static final class Mod {
        final File dir;
        final String name;
        final String version;
        final String author;
        final String description;
        final File picture;
        final Map<String, String> ini;
        boolean enabled;

        Mod(File dir, Map<String, String> info, String language, boolean enabled) {
            this.dir = dir;
            this.ini = info;
            this.name = localized(info, "name", language, dir.getName());
            this.version = localized(info, "version", language, "");
            this.author = localized(info, "author", language, "");
            String text = localized(info, "description", language, "");
            this.description = text.isEmpty() ? readme(dir, language) : text;
            this.picture = findPicture(dir, info.get("image"));
            this.enabled = enabled;
        }
    }

    private ModManager() {}

    static File gameRoot(Context ctx) {
        String path = SetupActivity.getSavedGamePath(ctx);
        if (path == null || path.trim().isEmpty()) {
            return null;
        }
        return new File(path);
    }

    static File modsRoot(Context ctx) {
        File game = gameRoot(ctx);
        return game == null ? null : new File(game, MODS_DIR);
    }

    private static String orderKey(Context ctx) {
        return SetupActivity.isGeneralsSelected(ctx) ? PREF_ORDER_GENERALS : PREF_ORDER;
    }

    /** The other game than the one picked on Home (SetupActivity.GAME_*). */
    static String otherGame(Context ctx) {
        return SetupActivity.isGeneralsSelected(ctx) ? SetupActivity.GAME_ZERO_HOUR : SetupActivity.GAME_GENERALS;
    }

    /** The mods folder of a given game, or null while that game has no folder set. */
    static File modsRootFor(Context ctx, String game) {
        String path = SetupActivity.getSavedGamePath(ctx, game);
        return path == null || path.trim().isEmpty() ? null : new File(path, MODS_DIR);
    }

    /**
     * Moves a mod folder to the other game's mods folder (a mod imported while the wrong game was
     * picked). It keeps its switch: on, it goes to the top of that game's list. A rename when both
     * folders are on the same storage, else a copy then a delete. Returns the new folder, or null.
     */
    static File moveToGame(Context ctx, Mod mod, String game) {
        File root = modsRootFor(ctx, game);
        if (root == null || (!root.isDirectory() && !root.mkdirs())) {
            return null;
        }
        File target = new File(root, mod.dir.getName());
        for (int i = 2; target.exists() && i < 100; i++) {
            target = new File(root, mod.dir.getName() + " (" + i + ")");
        }
        if (target.exists()) {
            return null;
        }
        if (!mod.dir.renameTo(target)) {
            try {
                copyTree(mod.dir.toPath(), target.toPath());
            } catch (IOException e) {
                android.util.Log.w("ModManager", "could not copy " + mod.dir + " to " + target, e);
                deleteTree(target);
                return null;
            }
            deleteTree(mod.dir);
        }
        // This game's list loses it on its next read (the folder is gone); the other gains it.
        String key = SetupActivity.GAME_GENERALS.equals(game) ? PREF_ORDER_GENERALS : PREF_ORDER;
        SharedPreferences prefs = ctx.getSharedPreferences(SetupActivity.PREFS_NAME, Context.MODE_PRIVATE);
        String stored = prefs.getString(key, "");
        String entry = (mod.enabled ? "1|" : "0|") + target.getName();
        String next = stored.isEmpty() ? entry : (mod.enabled ? entry + "\n" + stored : stored + "\n" + entry);
        prefs.edit().putString(key, next).apply();
        return target;
    }

    private static void copyTree(Path from, Path to) throws IOException {
        Files.walkFileTree(from, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes attrs)
                    throws IOException {
                Files.createDirectories(to.resolve(from.relativize(dir).toString()));
                return FileVisitResult.CONTINUE;
            }

            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs)
                    throws IOException {
                Files.copy(file, to.resolve(from.relativize(file).toString()));
                return FileVisitResult.CONTINUE;
            }
        });
    }

    private static void deleteTree(File dir) {
        File[] children = dir.listFiles();
        if (children != null) {
            for (File c : children) {
                if (c.isDirectory() && !Files.isSymbolicLink(c.toPath())) {
                    deleteTree(c);
                } else {
                    c.delete();
                }
            }
        }
        dir.delete();
    }

    /**
     * The mods of the selected game in the player's order: those already in the list first, then
     * folders found since, at the bottom and switched off.
     */
    static List<Mod> listMods(Context ctx) {
        List<Mod> result = new ArrayList<>();
        File root = modsRoot(ctx);
        if (root == null || !root.isDirectory()) {
            return result;
        }
        File[] children = root.listFiles(f -> f.isDirectory() && !f.getName().startsWith(".")
            && !Files.isSymbolicLink(f.toPath()));
        if (children == null) {
            return result;
        }
        Arrays.sort(children, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        Map<String, File> byName = new HashMap<>();
        for (File child : children) {
            byName.put(child.getName(), child);
        }
        String language = Locale.getDefault().getLanguage();
        for (String entry : readOrder(ctx)) {
            boolean enabled = entry.startsWith("1|");
            String name = entry.substring(2);
            File dir = byName.remove(name);
            if (dir != null) {
                result.add(new Mod(dir, readModIni(dir), language, enabled));
            }
        }
        for (File child : children) {
            if (byName.containsKey(child.getName())) {
                result.add(new Mod(child, readModIni(child), language, false));
            }
        }
        return result;
    }

    /** Keeps the order and switches of the given list for the selected game. */
    static void saveOrder(Context ctx, List<Mod> mods) {
        StringBuilder sb = new StringBuilder();
        for (Mod mod : mods) {
            if (sb.length() > 0) {
                sb.append('\n');
            }
            sb.append(mod.enabled ? "1|" : "0|").append(mod.dir.getName());
        }
        ctx.getSharedPreferences(SetupActivity.PREFS_NAME, Context.MODE_PRIVATE).edit()
            .putString(orderKey(ctx), sb.toString()).apply();
    }

    private static List<String> readOrder(Context ctx) {
        SharedPreferences prefs = ctx.getSharedPreferences(SetupActivity.PREFS_NAME, Context.MODE_PRIVATE);
        String stored = prefs.getString(orderKey(ctx), "");
        List<String> entries = new ArrayList<>();
        for (String line : stored.split("\n")) {
            if (line.length() > 2 && (line.startsWith("1|") || line.startsWith("0|"))) {
                entries.add(line);
            }
        }
        return entries;
    }

    static int enabledCount(Context ctx) {
        int count = 0;
        for (Mod mod : listMods(ctx)) {
            if (mod.enabled) {
                count++;
            }
        }
        return count;
    }

    /**
     * Right before the game starts: the enabled folders of the selected game, highest priority
     * first, for the engine (GXManagedMods.h). No enabled mod: no file.
     */
    static void writeActiveList(Context ctx) {
        File out = new File(ctx.getFilesDir(), ACTIVE_LIST_FILE);
        List<String> paths = new ArrayList<>();
        for (Mod mod : listMods(ctx)) {
            if (mod.enabled) {
                paths.add(mod.dir.getAbsolutePath());
            }
        }
        File pending = new File(ctx.getFilesDir(), BOOT_PENDING_FILE);
        if (paths.isEmpty()) {
            out.delete();
            pending.delete();
            return;
        }
        // Safe start: the marker counts the starts with this same list that have not reached the
        // main menu yet (the engine deletes it there, GXRemoteConfig::markEngineBootComplete).
        String list = String.join("\n", paths);
        int count = failedStarts(ctx, list) + 1;
        try (FileWriter w = new FileWriter(pending, false)) {
            w.write(count + "\n" + list);
        } catch (IOException e) {
            android.util.Log.w("ModManager", "could not write the mods boot marker", e);
        }
        try (FileWriter w = new FileWriter(out, false)) {
            // The set's profile: saves and replays of this set apart from the vanilla game's.
            w.write("#profile " + profileName(paths) + "\n");
            for (String path : paths) {
                w.write(path);
                w.write('\n');
            }
        } catch (IOException e) {
            android.util.Log.w("ModManager", "could not write the active mod list", e);
        }
    }

    /** The folder name of a set's profile: its mods' folder names, in order (GXManagedMods.h). */
    static String profileName(List<String> paths) {
        StringBuilder sb = new StringBuilder();
        for (String path : paths) {
            if (sb.length() > 0) {
                sb.append(" + ");
            }
            sb.append(new File(path).getName());
        }
        String name = sb.toString().replaceAll("[^A-Za-z0-9 _+.\\-]", "_");
        if (name.length() > 60) {
            name = name.substring(0, 52) + "_" + Integer.toHexString(sb.toString().hashCode());
        }
        return name.startsWith(".") ? "_" + name.substring(1) : name;
    }

    static final String BOOT_PENDING_FILE = "mods_boot_pending";
    private static final String PREF_LAST_ENABLED = "mods_last_enabled";

    /**
     * How many starts in a row with the given list (enabled folders, top first, one per line)
     * never reached the main menu; 0 when the last start did, or used another list.
     */
    static int failedStarts(Context ctx, String list) {
        File pending = new File(ctx.getFilesDir(), BOOT_PENDING_FILE);
        if (!pending.isFile()) {
            return 0;
        }
        String text = readText(pending, 64 * 1024);
        int eol = text.indexOf('\n');
        if (eol < 0 || !text.substring(eol + 1).equals(list)) {
            return 0;
        }
        try {
            return Integer.parseInt(text.substring(0, eol).trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** The enabled folders of the selected game as writeActiveList() would write them. */
    static String activeList(Context ctx) {
        List<String> paths = new ArrayList<>();
        for (Mod mod : listMods(ctx)) {
            if (mod.enabled) {
                paths.add(mod.dir.getAbsolutePath());
            }
        }
        return String.join("\n", paths);
    }

    /** Remembers the mod switched on most recently, for "start without the last mod added". */
    static void noteEnabled(Context ctx, Mod mod) {
        ctx.getSharedPreferences(SetupActivity.PREFS_NAME, Context.MODE_PRIVATE).edit()
            .putString(PREF_LAST_ENABLED, mod.dir.getName()).apply();
    }

    static String lastEnabled(Context ctx) {
        return ctx.getSharedPreferences(SetupActivity.PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_LAST_ENABLED, null);
    }

    /** Switches every mod of the selected game off, or only the one named. */
    static void switchOff(Context ctx, String onlyFolder) {
        List<Mod> mods = listMods(ctx);
        for (Mod mod : mods) {
            if (onlyFolder == null || mod.dir.getName().equals(onlyFolder)) {
                mod.enabled = false;
            }
        }
        saveOrder(ctx, mods);
    }

    /** The legacy '!' archives in the game folder: always loaded, outside this list. */
    static List<String> legacyArchives(Context ctx) {
        List<String> names = new ArrayList<>();
        File game = gameRoot(ctx);
        File[] files = game == null ? null : game.listFiles(f -> f.isFile()
            && f.getName().startsWith("!") && f.getName().toLowerCase(Locale.US).endsWith(".big"));
        if (files != null) {
            for (File f : files) {
                names.add(f.getName());
            }
            names.sort(String::compareToIgnoreCase);
        }
        return names;
    }

    /**
     * Moves an old '!' archive from the game folder into a mod folder of its own under mods/
     * (same storage, so a rename). Returns the new mod folder, or null.
     */
    static File moveLegacyArchive(Context ctx, String archive) {
        File game = gameRoot(ctx);
        File root = modsRoot(ctx);
        if (game == null || root == null || archive.contains("/") || archive.contains("\\")) {
            return null;
        }
        File source = new File(game, archive);
        if (!source.isFile() || (!root.isDirectory() && !root.mkdirs())) {
            return null;
        }
        String base = archive;
        while (base.startsWith("!")) {
            base = base.substring(1);
        }
        if (base.toLowerCase(Locale.US).endsWith(".big")) {
            base = base.substring(0, base.length() - 4);
        }
        base = base.trim().isEmpty() ? "mod" : base.trim();
        File dir = new File(root, base);
        for (int i = 2; dir.exists() && i < 100; i++) {
            dir = new File(root, base + " (" + i + ")");
        }
        if (dir.exists() || !dir.mkdirs()) {
            return null;
        }
        if (!source.renameTo(new File(dir, archive))) {
            dir.delete();
            return null;
        }
        return dir;
    }

    /**
     * mod.ini: "Key = Value" lines, keys case-insensitive, ';' or '#' comments; a key may carry a
     * language, "Name.ru = ...".
     */
    static Map<String, String> readModIni(File dir) {
        Map<String, String> info = new HashMap<>();
        File ini = new File(dir, "mod.ini");
        if (!ini.isFile() || ini.length() > 64 * 1024) {
            return info;
        }
        try (BufferedReader r = new BufferedReader(new InputStreamReader(
                new FileInputStream(ini), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                int comment = line.indexOf(';');
                if (comment >= 0) {
                    line = line.substring(0, comment);
                }
                line = line.trim();
                int eq = line.indexOf('=');
                if (line.startsWith("#") || eq <= 0) {
                    continue;
                }
                info.put(line.substring(0, eq).trim().toLowerCase(Locale.US),
                    line.substring(eq + 1).trim().replace("\\n", "\n"));
            }
        } catch (IOException e) {
            android.util.Log.w("ModManager", "could not read " + ini, e);
        }
        return info;
    }

    private static String localized(Map<String, String> info, String key, String language, String fallback) {
        String value = info.get(key + "." + language);
        if (value == null || value.isEmpty()) {
            value = info.get(key + ".en");
        }
        if (value == null || value.isEmpty()) {
            value = info.get(key);
        }
        return value == null || value.isEmpty() ? fallback : value;
    }

    /** readme_<language>.txt, readme_en.txt or readme.txt, the first part of it. */
    static String readme(File dir, String language) {
        String[] names = { "readme_" + language + ".txt", "readme_en.txt", "readme.txt" };
        File[] files = dir.listFiles(File::isFile);
        if (files == null) {
            return "";
        }
        for (String wanted : names) {
            for (File f : files) {
                if (f.getName().equalsIgnoreCase(wanted)) {
                    return readText(f, 4000);
                }
            }
        }
        return "";
    }

    static String readText(File f, int limit) {
        char[] buf = new char[limit];
        try (java.io.Reader r = new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8)) {
            int n = 0, got;
            while (n < limit && (got = r.read(buf, n, limit - n)) > 0) {
                n += got;
            }
            return new String(buf, 0, n).replace("\r", "").trim();
        } catch (IOException e) {
            android.util.Log.w("ModManager", "could not read " + f, e);
            return "";
        }
    }

    /** The pictures in the mod's screenshots/ folder, sorted by name. */
    static List<File> screenshots(File dir) {
        List<File> out = new ArrayList<>();
        File[] files = new File(dir, "screenshots").listFiles(File::isFile);
        if (files != null) {
            for (File f : files) {
                String lower = f.getName().toLowerCase(Locale.US);
                for (String ext : PICTURE_EXTENSIONS) {
                    if (lower.endsWith(ext)) {
                        out.add(f);
                    }
                }
            }
            out.sort((a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        }
        return out;
    }

    private static File findPicture(File dir, String named) {
        if (named != null && !named.isEmpty() && !named.contains("..")) {
            File f = new File(dir, named);
            if (f.isFile()) {
                return f;
            }
        }
        File[] files = dir.listFiles(File::isFile);
        if (files == null) {
            return null;
        }
        for (String base : PICTURE_NAMES) {
            for (File f : files) {
                String lower = f.getName().toLowerCase(Locale.US);
                for (String ext : PICTURE_EXTENSIONS) {
                    if (lower.equals(base + ext)) {
                        return f;
                    }
                }
            }
        }
        return null;
    }

    /**
     * Deletes one managed mod folder. Only a direct child of mods/, never through a symbolic
     * link (Files.walkFileTree does not follow them, so a link inside is removed as a link).
     */
    static boolean deleteMod(Context ctx, File candidate) {
        File root = modsRoot(ctx);
        if (root == null || candidate == null) {
            return false;
        }
        try {
            File canonicalRoot = root.getCanonicalFile();
            File canonical = candidate.getCanonicalFile();
            File parent = canonical.getParentFile();
            if (parent == null || !parent.equals(canonicalRoot) || candidate.getName().startsWith(".")
                    || Files.isSymbolicLink(candidate.toPath()) || !canonical.isDirectory()) {
                return false;
            }
            Files.walkFileTree(canonical.toPath(), new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs)
                        throws IOException {
                    Files.delete(file);
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult postVisitDirectory(Path dir, IOException error)
                        throws IOException {
                    if (error != null) {
                        throw error;
                    }
                    Files.delete(dir);
                    return FileVisitResult.CONTINUE;
                }
            });
            return !canonical.exists();
        } catch (IOException | SecurityException e) {
            android.util.Log.w("ModManager", "could not delete " + candidate, e);
            return false;
        }
    }
}
