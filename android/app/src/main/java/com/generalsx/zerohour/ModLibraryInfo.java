package com.generalsx.zerohour;

import java.io.File;
import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;

// GeneralsX @feature Android port 04/10/2026 Read-only library size accounting.
// No payload reads, symlink following, or changes to the managed mod format.
final class ModLibraryInfo {
    private ModLibraryInfo() {}

    static boolean isManagedEntry(File root, File entry) {
        try {
            return !Files.isSymbolicLink(entry.toPath())
                && root.getCanonicalFile().equals(entry.getCanonicalFile().getParentFile());
        } catch (IOException | SecurityException e) {
            return false;
        }
    }

    static long measure(File entry) {
        final long[] bytes = { 0 };
        final int[] visited = { 0 };
        final boolean[] complete = { true };
        try {
            if (Files.isSymbolicLink(entry.toPath())) return -1;
            Files.walkFileTree(entry.toPath(), new SimpleFileVisitor<Path>() {
                private FileVisitResult check() {
                    if (Thread.currentThread().isInterrupted() || ++visited[0] > 100000) {
                        complete[0] = false;
                        return FileVisitResult.TERMINATE;
                    }
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes attrs) {
                    return check();
                }

                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) {
                    if (attrs.isRegularFile()) bytes[0] = Math.addExact(bytes[0], attrs.size());
                    return check();
                }

                @Override public FileVisitResult visitFileFailed(Path file, IOException error) {
                    complete[0] = false;
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult postVisitDirectory(Path dir, IOException error) {
                    if (error != null) complete[0] = false;
                    return FileVisitResult.CONTINUE;
                }
            });
            return complete[0] ? bytes[0] : -1;
        } catch (IOException | SecurityException | ArithmeticException e) {
            return -1;
        }
    }
}
