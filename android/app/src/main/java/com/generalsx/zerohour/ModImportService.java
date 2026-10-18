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

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * GeneralsX @feature Android port 04/10/2026 Transactional mod importer.
 *
 * User-selected content is untrusted. Everything is copied into
 * Mods/.installing/<uuid> first, every destination is canonical-path checked,
 * BIG archives are header-validated, ZIP traversal is rejected, and the final
 * install becomes visible only through a same-filesystem rename after all
 * validation succeeds. A failed import therefore cannot half-install a mod.
 *
 * GeneralsX @feature Android port 18/10/2026 From the GeneralsX fork by realkot (used with
 * permission), into mods/ (PLAN-027): every mod becomes a folder, a single .big included; a
 * complete game picked by mistake is recognised and refused; and what a player can act on
 * (RAR, no game folder, no .big, a whole game) comes back as an ImportProblem with a launcher
 * string, in the player's language.
 */
final class ModImportService {
    private static final long MAX_TOTAL_BYTES = 8L * 1024L * 1024L * 1024L;
    private static final int MAX_ENTRIES = 100000;
    private static final int MAX_DEPTH = 64;
    private static final int BUFFER_SIZE = 64 * 1024;

    // GeneralsX @feature Android port 04/10/2026 Cooperative transactional cancellation.
    static final class CancellationSignal {
        private volatile boolean cancelled;

        void cancel() { cancelled = true; }
        boolean isCancelled() { return cancelled; }

        void throwIfCancelled() throws ImportCancelledException {
            if (cancelled || Thread.currentThread().isInterrupted()) {
                throw new ImportCancelledException();
            }
        }
    }

    /** A refusal the player can act on: its text is a launcher string. */
    static final class ImportProblem extends IOException {
        final int messageRes;

        ImportProblem(int messageRes) {
            super("import refused");
            this.messageRes = messageRes;
        }
    }

    static final class ImportCancelledException extends IOException {
        ImportCancelledException() { super("Import cancelled"); }
    }

    static final class Result {
        final File installed;
        final int fileCount;
        final int bigCount;
        final long bytes;
        /** A shared mod set (mods_set.txt): its mods in the sender's order; empty for one mod. */
        final List<File> set;

        Result(File installed, int fileCount, int bigCount, long bytes) {
            this(installed, fileCount, bigCount, bytes, new ArrayList<>());
        }

        Result(File installed, int fileCount, int bigCount, long bytes, List<File> set) {
            this.installed = installed;
            this.fileCount = fileCount;
            this.bigCount = bigCount;
            this.bytes = bytes;
            this.set = set;
        }
    }

    static final String SET_FILE = "mods_set.txt";

    private static final class Stats {
        int fileCount;
        int bigCount;
        long bytes;

        void addBytes(long count) throws IOException {
            bytes += count;
            if (bytes > MAX_TOTAL_BYTES) {
                throw new IOException("Mod exceeds the 8 GiB import safety limit");
            }
        }

        void addFile() throws IOException {
            fileCount++;
            if (fileCount > MAX_ENTRIES) {
                throw new IOException("Mod contains too many files");
            }
        }
    }

    private ModImportService() {}

    static Result importDocument(Context ctx, Uri uri) throws IOException {
        return importDocument(ctx, uri, new CancellationSignal());
    }

    static Result importDocument(Context ctx, Uri uri, CancellationSignal cancellation)
            throws IOException {
        cancellation.throwIfCancelled();
        File modsRoot = requireModsRoot(ctx);
        cleanupStaleStaging(modsRoot);

        String displayName = queryDisplayName(ctx.getContentResolver(), uri);
        if (displayName == null || displayName.trim().isEmpty()) {
            throw new IOException("Selected file has no usable name");
        }

        String lower = displayName.toLowerCase(Locale.US);
        if (lower.endsWith(".zip")) {
            return importZip(ctx, uri, displayName, modsRoot, cancellation);
        }
        if (lower.endsWith(".big")) {
            return importBig(ctx, uri, displayName, modsRoot, cancellation);
        }
        if (lower.endsWith(".rar") || lower.endsWith(".7z") || lower.endsWith(".exe")) {
            throw new ImportProblem(R.string.mods_import_rar);
        }
        throw new ImportProblem(R.string.mods_import_unsupported);
    }

    static Result importTree(Context ctx, Uri treeUri) throws IOException {
        return importTree(ctx, treeUri, new CancellationSignal());
    }

    static Result importTree(Context ctx, Uri treeUri, CancellationSignal cancellation)
            throws IOException {
        cancellation.throwIfCancelled();
        File modsRoot = requireModsRoot(ctx);
        cleanupStaleStaging(modsRoot);

        String documentId = DocumentsContract.getTreeDocumentId(treeUri);
        Uri rootDoc = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
        String name = queryDocumentName(ctx.getContentResolver(), rootDoc);
        name = sanitizeInstallName(name == null ? "Imported Mod" : name, false);

        File stagingRoot = stagingRoot(modsRoot);
        File stage = new File(stagingRoot, UUID.randomUUID().toString());
        if (!stage.mkdirs()) {
            throw new IOException("Could not create staging directory");
        }

        if (treeLooksLikeGame(ctx.getContentResolver(), treeUri, documentId, 0)) {
            deleteRecursively(stage);
            throw new ImportProblem(R.string.mods_import_full_game);
        }

        Stats stats = new Stats();
        try {
            copyTree(ctx.getContentResolver(), treeUri, documentId, stage, stats, 0, cancellation);
            stats.bigCount = verifyBigArchives(stage, cancellation);
            if (stats.bigCount == 0) {
                throw new ImportProblem(R.string.mods_import_no_big);
            }
            File installRoot = singleFolderInside(stage);

            File target = uniqueTarget(modsRoot, name, false);
            cancellation.throwIfCancelled();
            finishInstall(installRoot, target);
            deleteRecursively(stage);
            return new Result(target, stats.fileCount, stats.bigCount, stats.bytes);
        } catch (IOException | RuntimeException e) {
            deleteRecursively(stage);
            if (cancellation.isCancelled()) throw new ImportCancelledException();
            throw e;
        }
    }

    private static Result importBig(Context ctx, Uri uri, String displayName, File modsRoot,
                                    CancellationSignal cancellation)
            throws IOException {
        // One mod = one folder (PLAN-027): the archive goes into a folder named after it.
        String fileName = sanitizeInstallName(displayName, true);
        String name = sanitizeInstallName(fileName.substring(0, fileName.length() - 4), false);
        File stagingRoot = stagingRoot(modsRoot);
        File stage = new File(stagingRoot, UUID.randomUUID().toString());
        if (!stage.mkdirs()) {
            throw new IOException("Could not create staging directory");
        }
        File archive = new File(stage, fileName);

        Stats stats = new Stats();
        try (InputStream raw = ctx.getContentResolver().openInputStream(uri)) {
            if (raw == null) {
                throw new IOException("Could not open selected BIG archive");
            }
            stats.addFile();
            copyStream(raw, archive, stats, cancellation);
            if (!isValidBig(archive, cancellation)) {
                throw new IOException("Selected file is not a valid BIGF/BIG4 archive");
            }
            stats.bigCount = 1;

            File target = uniqueTarget(modsRoot, name, false);
            cancellation.throwIfCancelled();
            finishInstall(stage, target);
            return new Result(target, stats.fileCount, stats.bigCount, stats.bytes);
        } catch (IOException | RuntimeException e) {
            deleteRecursively(stage);
            if (cancellation.isCancelled()) throw new ImportCancelledException();
            throw e;
        }
    }

    private static Result importZip(Context ctx, Uri uri, String displayName, File modsRoot,
                                    CancellationSignal cancellation)
            throws IOException {
        String baseName = displayName.substring(0, displayName.length() - 4);
        String name = sanitizeInstallName(baseName, false);
        File stagingRoot = stagingRoot(modsRoot);
        File stage = new File(stagingRoot, UUID.randomUUID().toString());
        if (!stage.mkdirs()) {
            throw new IOException("Could not create staging directory");
        }

        Stats stats = new Stats();
        try (InputStream raw = ctx.getContentResolver().openInputStream(uri);
             ZipInputStream zip = raw == null ? null
                 : new ZipInputStream(new BufferedInputStream(raw, BUFFER_SIZE))) {
            if (zip == null) {
                throw new IOException("Could not open selected ZIP archive");
            }

            Set<String> seenPaths = new HashSet<>();
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                cancellation.throwIfCancelled();
                String entryName = normalizeZipPath(entry.getName());
                if (entryName.isEmpty()) {
                    zip.closeEntry();
                    continue;
                }
                File dest = resolveSafeRelative(stage, entryName);
                String canonicalKey = dest.getCanonicalPath();
                if (!seenPaths.add(canonicalKey)) {
                    throw new IOException("ZIP contains a duplicate path: " + entryName);
                }
                if (entry.isDirectory()) {
                    if (!dest.isDirectory() && !dest.mkdirs()) {
                        throw new IOException("Could not create directory: " + entryName);
                    }
                } else {
                    stats.addFile();
                    File parent = dest.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                        throw new IOException("Could not create directory for: " + entryName);
                    }
                    copyStream(zip, dest, stats, cancellation);
                }
                zip.closeEntry();
            }

            if (looksLikeGame(stage, 0)) {
                throw new ImportProblem(R.string.mods_import_full_game);
            }
            stats.bigCount = verifyBigArchives(stage, cancellation);
            if (stats.bigCount == 0) {
                throw new ImportProblem(R.string.mods_import_no_big);
            }
            File setList = new File(stage, SET_FILE);
            if (setList.isFile()) {
                return installSet(stage, setList, modsRoot, stats, cancellation);
            }
            File installRoot = singleFolderInside(stage);

            File target = uniqueTarget(modsRoot, name, false);
            cancellation.throwIfCancelled();
            finishInstall(installRoot, target);
            deleteRecursively(stage);
            return new Result(target, stats.fileCount, stats.bigCount, stats.bytes);
        } catch (IOException | RuntimeException e) {
            deleteRecursively(stage);
            if (cancellation.isCancelled()) throw new ImportCancelledException();
            throw e;
        }
    }

    private static void copyTree(ContentResolver resolver, Uri treeUri, String parentDocumentId,
                                 File destDir, Stats stats, int depth,
                                 CancellationSignal cancellation) throws IOException {
        cancellation.throwIfCancelled();
        if (depth > MAX_DEPTH) {
            throw new IOException("Mod folder nesting is too deep");
        }

        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocumentId);
        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE
        };

        try (Cursor cursor = resolver.query(children, projection, null, null, null)) {
            if (cursor == null) {
                throw new IOException("Could not read selected mod folder");
            }
            int idCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID);
            int nameCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            int mimeCol = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE);

            while (cursor.moveToNext()) {
                cancellation.throwIfCancelled();
                String childId = cursor.getString(idCol);
                String childName = validateSegment(cursor.getString(nameCol));
                String mime = cursor.getString(mimeCol);
                File dest = resolveSafeRelative(destDir, childName);

                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (!dest.isDirectory() && !dest.mkdirs()) {
                        throw new IOException("Could not create directory: " + childName);
                    }
                    copyTree(resolver, treeUri, childId, dest, stats, depth + 1, cancellation);
                } else {
                    stats.addFile();
                    Uri childUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childId);
                    try (InputStream in = resolver.openInputStream(childUri)) {
                        if (in == null) {
                            throw new IOException("Could not open: " + childName);
                        }
                        copyStream(in, dest, stats, cancellation);
                    }
                }
            }
        }
    }

    // A complete game rather than a mod: the archives every install has, at the top or one folder
    // down (an archive unpacked into a folder of its own name).
    private static final String[][] GAME_ARCHIVES = {
        { "ini.big", "inizh.big" }, { "w3d.big", "w3dzh.big" }, { "textures.big", "textureszh.big" } };

    private static boolean namesLookLikeGame(java.util.Collection<String> lowerNames) {
        for (String[] alternatives : GAME_ARCHIVES) {
            boolean found = false;
            for (String name : alternatives) {
                found |= lowerNames.contains(name);
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

    private static boolean looksLikeGame(File dir, int depth) {
        File[] children = dir.listFiles();
        if (children == null) {
            return false;
        }
        Set<String> names = new HashSet<>();
        for (File child : children) {
            names.add(child.getName().toLowerCase(Locale.US));
        }
        if (namesLookLikeGame(names)) {
            return true;
        }
        if (depth == 0) {
            for (File child : children) {
                if (child.isDirectory() && looksLikeGame(child, 1)) {
                    return true;
                }
            }
        }
        return false;
    }

    private static boolean treeLooksLikeGame(ContentResolver resolver, Uri treeUri, String documentId,
                                             int depth) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId);
        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE
        };
        Set<String> names = new HashSet<>();
        List<String> folders = new ArrayList<>();
        try (Cursor cursor = resolver.query(children, projection, null, null, null)) {
            if (cursor == null) {
                return false;
            }
            while (cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name != null) {
                    names.add(name.toLowerCase(Locale.US));
                }
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                    folders.add(cursor.getString(0));
                }
            }
        } catch (RuntimeException e) {
            // An unreadable listing is not a game; the copy that follows reports what fails.
            return false;
        }
        if (namesLookLikeGame(names)) {
            return true;
        }
        if (depth == 0) {
            for (String folder : folders) {
                if (treeLooksLikeGame(resolver, treeUri, folder, 1)) {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * The folder that is the mod: the staged one, or the only folder inside it when an archive
     * was packed with its folder (Contra.zip -> Contra/...), so the mod's files sit at its top.
     */
    private static File singleFolderInside(File stage) {
        File[] children = stage.listFiles();
        if (children != null && children.length == 1 && children[0].isDirectory()) {
            return children[0];
        }
        return stage;
    }

    /**
     * A shared mod set: mods_set.txt names its folders, top first. Each becomes a mod of its own;
     * the result lists them in that order.
     */
    private static Result installSet(File stage, File setList, File modsRoot, Stats stats,
                                     CancellationSignal cancellation) throws IOException {
        List<File> installed = new ArrayList<>();
        for (String line : java.nio.file.Files.readAllLines(setList.toPath(), java.nio.charset.StandardCharsets.UTF_8)) {
            String folder = line.trim();
            if (folder.isEmpty() || folder.startsWith("#")) {
                continue;
            }
            File source = resolveSafeRelative(stage, validateSegment(folder));
            if (!source.isDirectory()) {
                continue;
            }
            cancellation.throwIfCancelled();
            File target = uniqueTarget(modsRoot, sanitizeInstallName(folder, false), false);
            finishInstall(source, target);
            installed.add(target);
        }
        deleteRecursively(stage);
        if (installed.isEmpty()) {
            throw new ImportProblem(R.string.mods_import_no_big);
        }
        return new Result(installed.get(0), stats.fileCount, stats.bigCount, stats.bytes, installed);
    }

    /**
     * Zips mods for sharing into the app's cache: each under its folder name, and for more than
     * one a mods_set.txt with their order, which importZip() turns back into the same set.
     */
    static File exportZip(Context ctx, List<ModManager.Mod> mods, String baseName) throws IOException {
        File out = new File(ctx.getCacheDir(), sanitizeInstallName(baseName, false) + ".zip");
        File[] old = ctx.getCacheDir().listFiles((d, n) -> n.endsWith(".zip"));
        if (old != null) {
            for (File f : old) {
                f.delete();
            }
        }
        try (java.util.zip.ZipOutputStream zip = new java.util.zip.ZipOutputStream(
                new BufferedOutputStream(new FileOutputStream(out), BUFFER_SIZE))) {
            if (mods.size() > 1) {
                StringBuilder order = new StringBuilder();
                for (ModManager.Mod mod : mods) {
                    order.append(mod.dir.getName()).append('\n');
                }
                zip.putNextEntry(new ZipEntry(SET_FILE));
                zip.write(order.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                zip.closeEntry();
            }
            byte[] buf = new byte[BUFFER_SIZE];
            for (ModManager.Mod mod : mods) {
                addToZip(zip, mod.dir, mod.dir.getName(), buf, 0);
            }
        } catch (IOException e) {
            out.delete();
            throw e;
        }
        return out;
    }

    private static void addToZip(java.util.zip.ZipOutputStream zip, File file, String path, byte[] buf,
                                 int depth) throws IOException {
        if (depth > MAX_DEPTH || java.nio.file.Files.isSymbolicLink(file.toPath())) {
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File c : children) {
                    addToZip(zip, c, path + "/" + c.getName(), buf, depth + 1);
                }
            }
            return;
        }
        zip.putNextEntry(new ZipEntry(path));
        try (InputStream in = new FileInputStream(file)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                zip.write(buf, 0, n);
            }
        }
        zip.closeEntry();
    }

    private static File requireModsRoot(Context ctx) throws IOException {
        File root = ModManager.modsRoot(ctx);
        if (root == null) {
            throw new ImportProblem(R.string.mods_no_game_message);
        }
        if (!root.isDirectory() && !root.mkdirs()) {
            throw new IOException("Could not create the mods directory: " + root);
        }
        return root.getCanonicalFile();
    }

    private static File stagingRoot(File modsRoot) throws IOException {
        File staging = new File(modsRoot, ".installing").getCanonicalFile();
        if (!isInside(modsRoot, staging)) {
            throw new IOException("Invalid staging path");
        }
        if (!staging.isDirectory() && !staging.mkdirs()) {
            throw new IOException("Could not create mod staging directory");
        }
        return staging;
    }

    private static String queryDisplayName(ContentResolver resolver, Uri uri) {
        try (Cursor cursor = resolver.query(uri,
                new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int col = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (col >= 0) {
                    return cursor.getString(col);
                }
            }
        } catch (RuntimeException ignored) {
        }
        return uri.getLastPathSegment();
    }

    private static String queryDocumentName(ContentResolver resolver, Uri uri) {
        try (Cursor cursor = resolver.query(uri,
                new String[] { DocumentsContract.Document.COLUMN_DISPLAY_NAME },
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int col = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
                if (col >= 0) {
                    return cursor.getString(col);
                }
            }
        } catch (RuntimeException ignored) {
        }
        return null;
    }

    private static void copyStream(InputStream in, File dest, Stats stats,
                                   CancellationSignal cancellation) throws IOException {
        File parent = dest.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Could not create destination directory");
        }

        try (BufferedOutputStream out = new BufferedOutputStream(
                new FileOutputStream(dest), BUFFER_SIZE)) {
            byte[] buffer = new byte[BUFFER_SIZE];
            int n;
            while (true) {
                cancellation.throwIfCancelled();
                n = in.read(buffer);
                if (n == -1) break;
                if (n == 0) {
                    continue;
                }
                stats.addBytes(n);
                out.write(buffer, 0, n);
            }
            out.flush();
        }
    }

    private static int verifyBigArchives(File root, CancellationSignal cancellation)
            throws IOException {
        cancellation.throwIfCancelled();
        if (root.isFile()) {
            if (root.getName().toLowerCase(Locale.US).endsWith(".big")) {
                if (!isValidBig(root, cancellation)) {
                    throw new IOException("Corrupt BIG archive: " + root.getName());
                }
                return 1;
            }
            return 0;
        }

        File[] children = root.listFiles();
        if (children == null) {
            throw new IOException("Could not verify imported directory: " + root.getName());
        }
        int count = 0;
        for (File child : children) {
            count += verifyBigArchives(child, cancellation);
        }
        return count;
    }

    private static boolean isValidBig(File file, CancellationSignal cancellation)
            throws ImportCancelledException {
        if (!file.isFile() || file.length() < 16) {
            return false;
        }

        // Do more than check the four-byte magic: walk the BIG file table
        // without reading payload data, and reject impossible header lengths,
        // runaway names, or entries pointing outside the archive. This catches
        // truncated/corrupt downloads before the native loader ever sees them.
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(
                new FileInputStream(file), BUFFER_SIZE))) {
            byte[] magic = new byte[4];
            in.readFully(magic);
            if (magic[0] != 'B' || magic[1] != 'I' || magic[2] != 'G'
                    || (magic[3] != 'F' && magic[3] != '4')) {
                return false;
            }

            in.readInt(); // archive-size field is little-endian; not needed here
            int count = in.readInt();      // file count is big-endian
            int headerSize = in.readInt(); // header size is big-endian
            if (count < 0 || count > 200000 || headerSize < 16
                    || headerSize > file.length()) {
                return false;
            }

            long tableBytes = 16;
            for (int i = 0; i < count; i++) {
                if ((i & 255) == 0) cancellation.throwIfCancelled();
                if (tableBytes + 9 > headerSize) {
                    return false;
                }
                long offset = Integer.toUnsignedLong(in.readInt());
                long size = Integer.toUnsignedLong(in.readInt());
                tableBytes += 8;

                int nameBytes = 0;
                int c;
                do {
                    c = in.read();
                    if (c < 0) {
                        return false;
                    }
                    tableBytes++;
                    nameBytes++;
                    if (nameBytes > 4096 || tableBytes > headerSize) {
                        return false;
                    }
                } while (c != 0);

                if (offset > file.length() || size > file.length()
                        || offset + size < offset || offset + size > file.length()) {
                    return false;
                }
            }
            return tableBytes <= headerSize;
        } catch (ImportCancelledException e) {
            throw e;
        } catch (IOException e) {
            return false;
        }
    }

    private static String normalizeZipPath(String raw) throws IOException {
        if (raw == null || raw.indexOf(0) >= 0) {
            throw new IOException("ZIP contains an invalid path");
        }
        String path = raw.replace('\\', '/');
        while (path.startsWith("./")) {
            path = path.substring(2);
        }
        if (path.startsWith("/") || path.matches("^[A-Za-z]:.*")) {
            throw new IOException("ZIP contains an absolute path: " + raw);
        }
        return path;
    }

    private static File resolveSafeRelative(File root, String relative) throws IOException {
        if (relative == null || relative.indexOf(0) >= 0) {
            throw new IOException("Invalid mod path");
        }

        String normalized = relative.replace('\\', '/');
        String[] segments = normalized.split("/");
        File current = root;
        int depth = 0;
        for (String segment : segments) {
            if (segment.isEmpty() || ".".equals(segment)) {
                continue;
            }
            if ("..".equals(segment)) {
                throw new IOException("Mod tries to escape its install directory");
            }
            validateSegment(segment);
            if (++depth > MAX_DEPTH) {
                throw new IOException("Mod path nesting is too deep");
            }
            current = new File(current, segment);
        }

        File canonicalRoot = root.getCanonicalFile();
        File canonical = current.getCanonicalFile();
        if (!isInsideOrEqual(canonicalRoot, canonical)) {
            throw new IOException("Mod path escapes its install directory");
        }
        return canonical;
    }

    private static String validateSegment(String segment) throws IOException {
        if (segment == null) {
            throw new IOException("Mod contains an unnamed file");
        }
        String value = segment.trim();
        if (value.isEmpty() || ".".equals(value) || "..".equals(value)
                || value.indexOf('/') >= 0 || value.indexOf('\\') >= 0
                || value.indexOf(0) >= 0) {
            throw new IOException("Mod contains an unsafe file name");
        }
        if (value.length() > 255) {
            throw new IOException("Mod contains a file name longer than 255 characters");
        }
        return value;
    }

    private static String sanitizeInstallName(String input, boolean keepBigExtension)
            throws IOException {
        if (input == null) {
            throw new IOException("Mod name is missing");
        }
        String value = input.trim();
        if (value.isEmpty()) {
            throw new IOException("Mod name is empty");
        }

        StringBuilder out = new StringBuilder();
        for (int i = 0; i < value.length() && out.length() < 80; i++) {
            char c = value.charAt(i);
            if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
                    || c == '"' || c == '<' || c == '>' || c == '|') {
                out.append('_');
            } else {
                out.append(c);
            }
        }
        String result = out.toString().trim();
        while (result.endsWith(".")) {
            result = result.substring(0, result.length() - 1).trim();
        }
        if (result.isEmpty() || ".".equals(result) || "..".equals(result)) {
            throw new IOException("Mod name is not usable");
        }
        // A dot-prefixed direct child is hidden by ModManager.listMods().
        // Keep imported mods visible instead of successfully installing one
        // that then appears to have vanished from the UI.
        if (result.startsWith(".")) {
            result = "_" + result.substring(1);
            if (result.equals("_")) {
                result = "_mod";
            }
        }
        if (keepBigExtension && !result.toLowerCase(Locale.US).endsWith(".big")) {
            result += ".big";
        }
        return result;
    }

    private static File uniqueTarget(File root, String requestedName, boolean file)
            throws IOException {
        File target = new File(root, requestedName).getCanonicalFile();
        if (!isInside(root, target)) {
            throw new IOException("Invalid mod destination");
        }
        if (!target.exists()) {
            return target;
        }

        String stem = requestedName;
        String extension = "";
        if (file && requestedName.toLowerCase(Locale.US).endsWith(".big")) {
            stem = requestedName.substring(0, requestedName.length() - 4);
            extension = ".big";
        }
        for (int i = 2; i <= 99; i++) {
            target = new File(root, stem + " (" + i + ")" + extension).getCanonicalFile();
            if (isInside(root, target) && !target.exists()) {
                return target;
            }
        }
        throw new IOException("Too many copies of this mod are already installed");
    }

    private static void finishInstall(File stage, File target) throws IOException {
        if (target.exists()) {
            throw new IOException("Destination already exists: " + target.getName());
        }
        if (!stage.renameTo(target)) {
            throw new IOException("Could not atomically finish the mod installation");
        }
    }

    private static boolean isInside(File root, File child) throws IOException {
        File canonicalRoot = root.getCanonicalFile();
        File parent = child.getCanonicalFile().getParentFile();
        while (parent != null) {
            if (parent.equals(canonicalRoot)) {
                return true;
            }
            parent = parent.getParentFile();
        }
        return false;
    }

    private static boolean isInsideOrEqual(File root, File child) throws IOException {
        return root.getCanonicalFile().equals(child.getCanonicalFile()) || isInside(root, child);
    }

    private static void cleanupStaleStaging(File modsRoot) {
        File staging = new File(modsRoot, ".installing");
        File[] entries = staging.listFiles();
        if (entries == null) {
            return;
        }
        long cutoff = System.currentTimeMillis() - 24L * 60L * 60L * 1000L;
        for (File entry : entries) {
            if (entry.lastModified() > 0 && entry.lastModified() < cutoff) {
                deleteRecursively(entry);
            }
        }
    }

    private static void deleteRecursively(File file) {
        if (file == null || !file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursively(child);
                }
            }
        }
        file.delete();
    }
}
