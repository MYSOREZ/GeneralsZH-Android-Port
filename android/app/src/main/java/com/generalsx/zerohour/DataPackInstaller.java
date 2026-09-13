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
// Fetches the GeneralsOnline game data this device needs to play against PC
// players, so that playing online needs no PC at all.
//
// Two things live outside the retail game and are not optional online. The
// community data patch is an INI archive the PC client mounts on every launch
// (ArchiveFileSystem::loadMods), and it moves the INI checksum every lobby is
// compared against: retail data alone computes 4272612339, retail plus the
// patch computes 2180732466, which is what every PC-hosted lobby reports. The
// community maps are the maps those lobbies are actually played on. Without
// either, a join is refused before it starts.
//
// Neither ships with the game and neither is served by any API the client
// talks to -- on Windows they arrive inside the installer. They are also,
// however, published as a plain ZIP next to a small JSON manifest, and that is
// what this reads: manifest for the version, size and SHA-256, then the ZIP,
// then the two directories out of it that mean anything on Android. The
// Windows binaries in the same archive are ignored.
//
// Only those two prefixes are extracted, and every entry is checked against
// its destination before anything is written -- a ZIP is an untrusted list of
// paths, and "GeneralsOnlineGameData/../../.." would otherwise be a write
// wherever it pointed.

package com.generalsx.zerohour;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class DataPackInstaller {

    private DataPackInstaller() {}

    private static final String MANIFEST_URL = "https://cdn.playgenerals.online/manifest.json";

    /** The only two directories in the package that mean anything here. */
    private static final String[] WANTED_PREFIXES = {
        "GeneralsOnlineGameData/",
        "Maps/",
    };

    private static final String PREFS_NAME = "generals_online";
    private static final String PREF_INSTALLED_VERSION = "datapack_version";

    /** Where the engine looks: BuildUserDataPathFromRegistry's Android branch. */
    static File userDataDir() {
        return new File(android.os.Environment.getExternalStorageDirectory(),
            "Generals/Command and Conquer Generals Zero Hour Data");
    }

    static File communityPatchFile() {
        return new File(userDataDir(),
            "GeneralsOnlineGameData/500_900_CommunityPatch_CoreINI.big");
    }

    static String installedVersion(Context ctx) {
        String version = ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_INSTALLED_VERSION, null);
        // A recorded version with the file gone is worse than no record: it
        // would report "installed" for data the player has since deleted.
        return communityPatchFile().isFile() ? version : null;
    }

    /** Phases the caller turns into something on screen. */
    interface Progress {
        void onChecking();
        void onDownloading(long bytes, long total);
        void onInstalling();
    }

    static final class Result {
        final boolean ok;
        final String version;
        final int filesWritten;
        final String error;

        private Result(boolean ok, String version, int filesWritten, String error) {
            this.ok = ok;
            this.version = version;
            this.filesWritten = filesWritten;
            this.error = error;
        }

        static Result success(String version, int filesWritten) {
            return new Result(true, version, filesWritten, null);
        }

        static Result failure(String error) {
            return new Result(false, null, 0, error);
        }
    }

    /**
     * Downloads and installs the current package. Blocking -- call it off the
     * main thread. Never throws: every failure comes back as Result.failure so
     * the caller has one thing to render.
     */
    static Result install(Context ctx, Progress progress) {
        File tempZip = null;
        try {
            progress.onChecking();
            JSONObject manifest = new JSONObject(fetchText(MANIFEST_URL));
            String version = manifest.optString("version", "");
            String downloadUrl = manifest.optString("download_url", "");
            long expectedSize = manifest.optLong("size", -1);
            String expectedSha = manifest.optString("sha256", "");

            if (downloadUrl.isEmpty() || !downloadUrl.startsWith("https://")) {
                return Result.failure("manifest has no usable download URL");
            }

            NetworkTrace.write(ctx, "[datapack] manifest version=" + version
                + " size=" + expectedSize + " url=" + downloadUrl);

            // The cache dir, not the shared storage the data itself goes to:
            // a half-downloaded package is this app's business and should not
            // survive being killed mid-download.
            tempZip = new File(ctx.getCacheDir(), "generalsonline-datapack.zip");
            String actualSha = download(downloadUrl, tempZip, expectedSize, progress);

            if (!expectedSha.isEmpty() && !expectedSha.equalsIgnoreCase(actualSha)) {
                // The manifest publishes a digest; ignoring it would make this
                // a download that installs whatever arrived, which for files
                // the game then treats as authoritative data is not a risk
                // worth taking for the two lines it saves.
                NetworkTrace.write(ctx, "[datapack] checksum mismatch: expected "
                    + expectedSha + " got " + actualSha);
                return Result.failure("checksum mismatch");
            }

            progress.onInstalling();
            File target = userDataDir();
            int written = extract(tempZip, target);
            NetworkTrace.write(ctx, "[datapack] installed " + written
                + " file(s) into " + target.getAbsolutePath());

            ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .edit().putString(PREF_INSTALLED_VERSION, version).apply();

            return Result.success(version, written);
        } catch (Exception e) {
            String message = e.getMessage() != null ? e.getMessage() : e.toString();
            NetworkTrace.write(ctx, "[datapack] failed: " + message);
            return Result.failure(message);
        } finally {
            if (tempZip != null) {
                // Best effort: 30MB of cache is not worth failing the install
                // that already succeeded.
                tempZip.delete();
            }
        }
    }

    private static String fetchText(String url) throws IOException {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(20000);
            conn.setRequestProperty("Accept", "application/json");

            int status = conn.getResponseCode();
            if (status != 200) {
                throw new IOException("HTTP " + status + " from manifest");
            }

            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            copy(conn.getInputStream(), out);
            return out.toString("UTF-8");
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    /** Streams the package to disk and returns its SHA-256, lowercase hex. */
    private static String download(String url, File dest, long expectedSize, Progress progress)
            throws IOException, java.security.NoSuchAlgorithmException {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(30000);

            int status = conn.getResponseCode();
            if (status != 200) {
                throw new IOException("HTTP " + status + " downloading the package");
            }

            long total = conn.getContentLengthLong();
            if (total <= 0) {
                total = expectedSize;
            }

            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream in = new BufferedInputStream(conn.getInputStream());
                 OutputStream out = new FileOutputStream(dest)) {
                byte[] buffer = new byte[64 * 1024];
                long done = 0;
                long lastReported = -1;
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                    digest.update(buffer, 0, read);
                    done += read;
                    // One update per percent, not per buffer: 30MB in 64KB
                    // chunks is ~500 posts to the main thread otherwise.
                    long percent = total > 0 ? (done * 100 / total) : -1;
                    if (percent != lastReported) {
                        lastReported = percent;
                        progress.onDownloading(done, total);
                    }
                }
            }

            return toHex(digest.digest());
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    /** Extracts the wanted prefixes into targetRoot. Returns the file count. */
    private static int extract(File zipFile, File targetRoot) throws IOException {
        String rootPath = targetRoot.getCanonicalPath() + File.separator;
        int written = 0;

        try (ZipInputStream zip = new ZipInputStream(
                new BufferedInputStream(new java.io.FileInputStream(zipFile)))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName().replace('\\', '/');
                if (!isWanted(name)) {
                    continue;
                }

                File out = new File(targetRoot, name);
                // Zip slip: the entry name decides the path, and the entry
                // name comes from a file off the network.
                if (!out.getCanonicalPath().startsWith(rootPath)) {
                    throw new IOException("package entry escapes its directory: " + name);
                }

                if (entry.isDirectory()) {
                    out.mkdirs();
                    continue;
                }

                File parent = out.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                    throw new IOException("could not create " + parent.getAbsolutePath());
                }

                try (OutputStream dest = new FileOutputStream(out)) {
                    copy(zip, dest);
                }
                written++;
            }
        }

        if (written == 0) {
            throw new IOException("package contained none of the expected data");
        }
        return written;
    }

    private static boolean isWanted(String name) {
        for (String prefix : WANTED_PREFIXES) {
            if (name.regionMatches(true, 0, prefix, 0, prefix.length())) {
                return true;
            }
        }
        return false;
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        int read;
        while ((read = in.read(buffer)) > 0) {
            out.write(buffer, 0, read);
        }
    }

    private static String toHex(byte[] bytes) {
        StringBuilder hex = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            hex.append(String.format(Locale.US, "%02x", b));
        }
        return hex.toString();
    }
}
