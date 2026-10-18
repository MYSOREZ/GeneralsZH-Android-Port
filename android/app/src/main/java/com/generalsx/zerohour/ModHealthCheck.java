package com.generalsx.zerohour;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * GeneralsX @feature 10/10/2026 Read-only installed-mod structural inspection.
 * This never writes or mounts a mod, and does not claim that W3D/effects can render.
 * Bound disk traversal to avoid hanging the UI on gigantic or hostile directories.
 */
final class ModHealthCheck {
    static final int MAX_VISITS = 50000;
    static final int MAX_BIG_FILES = 1024;
    private static final int MAX_BIG_ENTRIES = 200000;
    private static final int MAX_NAME_BYTES = 4096;
    private static final int MAX_ISSUES = 8;

    private ModHealthCheck() {}

    static final class Result {
        int archives, validArchives, invalidArchives, archivedEntries;
        int looseFiles, nestedArchives, symlinks, unreadable;
        long bytes;
        boolean dataFolder, windowFolder, truncated, cancelled;
        final List<String> invalidNames = new ArrayList<>();
        List<String> issues() { return Collections.unmodifiableList(invalidNames); }
    }

    static Result scan(File mod) {
        Result result = new Result();
        if (mod == null || !Files.exists(mod.toPath(), LinkOption.NOFOLLOW_LINKS)
                || Files.isSymbolicLink(mod.toPath())) {
            result.unreadable++;
            return result;
        }
        final Path root = mod.toPath().toAbsolutePath().normalize();
        result.dataFolder = Files.isDirectory(root.resolve("Data"), LinkOption.NOFOLLOW_LINKS);
        result.windowFolder = Files.isDirectory(root.resolve("Window"), LinkOption.NOFOLLOW_LINKS);
        final int[] visits = {0};
        try {
            Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
                private FileVisitResult visit() {
                    if (Thread.currentThread().isInterrupted()) {
                        result.cancelled = true;
                        result.truncated = true;
                        return FileVisitResult.TERMINATE;
                    }
                    if (++visits[0] > MAX_VISITS) {
                        result.truncated = true;
                        return FileVisitResult.TERMINATE;
                    }
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes a) {
                    return visit();
                }

                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) {
                    FileVisitResult decision = visit();
                    if (decision == FileVisitResult.TERMINATE) return decision;
                    if (attrs.isSymbolicLink()) {
                        result.symlinks++;
                        return FileVisitResult.CONTINUE;
                    }
                    if (!attrs.isRegularFile()) return FileVisitResult.CONTINUE;
                    long size = attrs.size();
                    if (size > Long.MAX_VALUE - result.bytes) result.truncated = true;
                    else result.bytes += size;
                    if (!file.getFileName().toString().toLowerCase(Locale.ROOT).endsWith(".big")) {
                        result.looseFiles++;
                        return FileVisitResult.CONTINUE;
                    }
                    if (++result.archives > MAX_BIG_FILES) {
                        result.truncated = true;
                        return FileVisitResult.TERMINATE;
                    }
                    if (!file.getParent().equals(root)) result.nestedArchives++;
                    int count = checkBigTable(file.toFile());
                    if (count < 0) {
                        result.invalidArchives++;
                        if (result.invalidNames.size() < MAX_ISSUES) {
                            result.invalidNames.add(root.relativize(file).toString());
                        }
                    } else {
                        result.validArchives++;
                        if (count > Integer.MAX_VALUE - result.archivedEntries) result.truncated = true;
                        else result.archivedEntries += count;
                    }
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult visitFileFailed(Path file, IOException error) {
                    result.unreadable++;
                    return visit();
                }
            });
        } catch (IOException | SecurityException e) {
            result.unreadable++;
        }
        return result;
    }

    // Match the installed importer's BIGF/BIG4 header/table rules without ever
    // reading archive payloads. File-size field is little-endian; indexed fields BE.
    static int checkBigTable(File big) {
        if (big == null || !big.isFile() || big.length() < 16) return -1;
        try (RandomAccessFile in = new RandomAccessFile(big, "r")) {
            int magic = in.readInt();
            if (magic != 0x42494746 && magic != 0x42494734) return -1;
            in.readInt(); // little-endian declared archive size; importer does not trust it
            int count = in.readInt();
            int headerSize = in.readInt();
            long length = in.length();
            if (count < 0 || count > MAX_BIG_ENTRIES || headerSize < 16 || headerSize > length) return -1;
            long pos = 16;
            for (int i = 0; i < count; ++i) {
                if ((i & 255) == 0 && Thread.currentThread().isInterrupted()) return -1;
                if (pos + 9 > headerSize) return -1;
                long start = Integer.toUnsignedLong(in.readInt());
                long size = Integer.toUnsignedLong(in.readInt());
                pos += 8;
                int nameBytes = 0;
                int value;
                do {
                    if (++nameBytes > MAX_NAME_BYTES || ++pos > headerSize) return -1;
                    value = in.read();
                    if (value < 0) return -1;
                } while (value != 0);
                if (start > length || size > length || start + size > length) return -1;
            }
            return count;
        } catch (IOException | SecurityException e) {
            return -1;
        }
    }
}
