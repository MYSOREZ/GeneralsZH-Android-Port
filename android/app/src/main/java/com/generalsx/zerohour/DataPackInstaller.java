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

import org.json.JSONArray;
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
import java.io.FileInputStream;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class DataPackInstaller {

    private DataPackInstaller() {}

    private static final String MANIFEST_URL = "https://cdn.playgenerals.online/manifest.json";

    /**
     * GeneralsX @feature Android port 27/09/2026 The package's own manifest address can be
     * changed from the signed update settings (datapack_manifest_url), so a move of the
     * GeneralsOnline CDN does not need a new APK. Only an https address is taken.
     */
    static String manifestUrl(Context ctx) {
        String url = UpdateManager.remoteConfig(ctx, "datapack_manifest_url", MANIFEST_URL);
        // GeneralsX @bugfix Android port 03/10/2026 A copy on the updates branch is published for
        // launchers up to 1.3.0, which compare the CDN's sha256 untrimmed (it starts with a space)
        // and so fail every install with "checksum mismatch". This launcher trims it, and the copy
        // is only as current as the last settings publish, so it reads the CDN itself.
        if (url.startsWith(UpdateManager.BASE_URL)) {
            return MANIFEST_URL;
        }
        return url.startsWith("https://") ? url : MANIFEST_URL;
    }

    /** The version the GeneralsOnline CDN offers now, or null if it cannot be reached. */
    static String latestVersion(Context ctx) {
        try {
            String version = new JSONObject(fetchText(manifestUrl(ctx))).optString("version", "");
            return version.isEmpty() ? null : version;
        } catch (Exception e) {
            return null;
        }
    }

    /** The only two directories in the package that mean anything here. */
    private static final String[] WANTED_PREFIXES = {
        "GeneralsOnlineGameData/",
        "Maps/",
    };

    private static final String PREFS_NAME = "generals_online";
    private static final String PREF_INSTALLED_VERSION = "datapack_version";

    /**
     * Presence turns the patch off without removing it, and the engine is the
     * only reader (ArchiveFileSystem::loadMods). It sits in the user-data
     * folder beside the patch rather than in the game folder, so the switch
     * works whether or not a game folder has been picked yet.
     */
    private static final String DISABLE_MARKER = "gx_no_community_patch.txt";

    /**
     * What the last install actually wrote, so uninstalling removes that and
     * nothing else. Deleting Maps/ wholesale would take the player's own maps
     * with it -- they share the directory.
     */
    private static final String INSTALLED_LIST = "datapack-installed.json";

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

    static boolean isEnabled() {
        return !new File(userDataDir(), DISABLE_MARKER).isFile();
    }

    /** Returns true if the state now matches what was asked for. */
    static boolean setEnabled(Context ctx, boolean enabled) {
        File marker = new File(userDataDir(), DISABLE_MARKER);
        if (enabled) {
            boolean ok = !marker.exists() || marker.delete();
            NetworkTrace.write(ctx, "[datapack] community patch enabled=" + ok);
            return ok;
        }
        try {
            File parent = marker.getParentFile();
            if (parent != null && !parent.isDirectory()) {
                parent.mkdirs();
            }
            try (FileWriter out = new FileWriter(marker)) {
                out.write("The community data patch is disabled while this file exists.\n");
            }
            NetworkTrace.write(ctx, "[datapack] community patch disabled");
            return true;
        } catch (IOException e) {
            NetworkTrace.write(ctx, "[datapack] could not disable: " + e);
            return false;
        }
    }

    // GeneralsX @feature Android port 28/09/2026 "Use the patch with mods too". The engine leaves
    // the patch out when a data mod is installed (ArchiveFileSystem::loadMods: the patch's INI
    // directories would mix with the mod's files); this marker, beside the disable one, is the
    // player's override for a mod built on top of the GeneralsOnline patch.
    private static final String WITH_MODS_MARKER = "gx_community_patch_with_mods.txt";

    static boolean isEnabledWithMods() {
        return new File(userDataDir(), WITH_MODS_MARKER).isFile();
    }

    /** Returns true if the state now matches what was asked for. */
    static boolean setEnabledWithMods(Context ctx, boolean enabled) {
        File marker = new File(userDataDir(), WITH_MODS_MARKER);
        if (!enabled) {
            boolean ok = !marker.exists() || marker.delete();
            NetworkTrace.write(ctx, "[datapack] patch with mods disabled=" + ok);
            return ok;
        }
        try {
            File parent = marker.getParentFile();
            if (parent != null && !parent.isDirectory()) {
                parent.mkdirs();
            }
            try (FileWriter out = new FileWriter(marker)) {
                out.write("The community data patch is mounted even with a data mod while this file exists.\n");
            }
            NetworkTrace.write(ctx, "[datapack] patch with mods enabled");
            return true;
        } catch (IOException e) {
            NetworkTrace.write(ctx, "[datapack] could not enable with mods: " + e);
            return false;
        }
    }

    /**
     * The first archive in the game folder that the engine treats as a data mod, or null: the
     * same rule as ArchiveFileSystem::loadMods -- a name starting with '!' (how mods sort ahead
     * of the retail archives) and at least one file under Data\INI\. UI and texture add-ons with
     * the same prefix carry no INI and do not count. Reads only the archives' file tables.
     */
    static String findDataMod(String gamePath) {
        if (gamePath == null) {
            return null;
        }
        File[] files = new File(gamePath).listFiles();
        if (files == null) {
            return null;
        }
        java.util.Arrays.sort(files);
        for (File f : files) {
            String name = f.getName();
            if (!f.isFile() || !name.startsWith("!") || !name.toLowerCase(Locale.US).endsWith(".big")) {
                continue;
            }
            if (bigHasIniFiles(f)) {
                return name;
            }
        }
        return null;
    }

    // BIG file table: "BIGF"/"BIG4", archive size (LE), file count (BE), header size (BE), then per
    // file: offset (BE), size (BE), zero-terminated path.
    private static boolean bigHasIniFiles(File big) {
        try (java.io.DataInputStream in = new java.io.DataInputStream(
                new BufferedInputStream(new java.io.FileInputStream(big), 64 * 1024))) {
            byte[] magic = new byte[4];
            in.readFully(magic);
            if (magic[0] != 'B' || magic[1] != 'I' || magic[2] != 'G') {
                return false;
            }
            in.readInt(); // archive size, little-endian, unused
            final int count = in.readInt();
            in.readInt(); // header size
            if (count < 0 || count > 200000) {
                return false;
            }
            StringBuilder path = new StringBuilder();
            for (int i = 0; i < count; i++) {
                in.readInt();
                in.readInt();
                path.setLength(0);
                int c;
                while ((c = in.read()) > 0) {
                    path.append((char) c);
                }
                if (c < 0) {
                    return false;
                }
                String lower = path.toString().toLowerCase(Locale.US).replace('/', '\\');
                if (lower.startsWith("data\\ini\\")) {
                    return true;
                }
            }
        } catch (IOException e) {
            return false;
        }
        return false;
    }

    /**
     * Removes exactly the files the last install wrote, then any directories
     * left empty by that. Returns how many files went, or -1 if there was no
     * record to work from.
     */
    static int uninstall(Context ctx) {
        List<String> installed = readInstalledList(ctx);
        if (installed == null) {
            return -1;
        }

        File root = userDataDir();
        int removed = 0;
        for (String relative : installed) {
            File victim = new File(root, relative);
            if (victim.isFile() && victim.delete()) {
                removed++;
            }
        }

        // Second pass: the directories those files were the only contents of.
        // Deepest first, so a directory whose children just went can still go.
        List<String> dirs = new ArrayList<>();
        for (String relative : installed) {
            int cut = relative.lastIndexOf('/');
            while (cut > 0) {
                String dir = relative.substring(0, cut);
                if (!dirs.contains(dir)) {
                    dirs.add(dir);
                }
                cut = dir.lastIndexOf('/');
            }
        }
        java.util.Collections.sort(dirs);
        java.util.Collections.reverse(dirs);
        for (String dir : dirs) {
            File candidate = new File(root, dir);
            String[] left = candidate.list();
            if (left != null && left.length == 0) {
                candidate.delete();
            }
        }

        new File(ctx.getFilesDir(), INSTALLED_LIST).delete();
        ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().remove(PREF_INSTALLED_VERSION).apply();

        NetworkTrace.write(ctx, "[datapack] uninstalled " + removed + " file(s)");
        return removed;
    }

    private static List<String> readInstalledList(Context ctx) {
        File list = new File(ctx.getFilesDir(), INSTALLED_LIST);
        if (!list.isFile()) {
            return null;
        }
        try {
            java.io.ByteArrayOutputStream raw = new java.io.ByteArrayOutputStream();
            try (InputStream in = new FileInputStream(list)) {
                copy(in, raw);
            }
            JSONArray array = new JSONArray(raw.toString("UTF-8"));
            List<String> paths = new ArrayList<>(array.length());
            for (int i = 0; i < array.length(); i++) {
                paths.add(array.getString(i));
            }
            return paths;
        } catch (Exception e) {
            NetworkTrace.write(ctx, "[datapack] could not read the installed list: " + e);
            return null;
        }
    }

    private static void writeInstalledList(Context ctx, List<String> paths) {
        try (FileWriter out = new FileWriter(new File(ctx.getFilesDir(), INSTALLED_LIST))) {
            out.write(new JSONArray(paths).toString());
        } catch (Exception e) {
            // Losing this costs the player a clean uninstall, not the install
            // itself -- so it is worth a log line and nothing more.
            NetworkTrace.write(ctx, "[datapack] could not record the installed list: " + e);
        }
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
    /** Held for a whole install: two at once would extract over each other. */
    static final Object INSTALL_LOCK = new Object();

    static Result install(Context ctx, Progress progress) {
        synchronized (INSTALL_LOCK) {
            return installLocked(ctx, progress);
        }
    }

    private static Result installLocked(Context ctx, Progress progress) {
        File tempZip = null;
        try {
            progress.onChecking();
            JSONObject manifest = new JSONObject(fetchText(manifestUrl(ctx)));
            String version = manifest.optString("version", "");
            String downloadUrl = manifest.optString("download_url", "");
            long expectedSize = manifest.optLong("size", -1);
            // GeneralsX @bugfix Android port 02/10/2026 Trimmed: the 100126_QFE3 manifest publishes " 0857..." with a
            // leading space, and every update then failed as "checksum mismatch" on a correct download.
            String expectedSha = manifest.optString("sha256", "").trim();

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
            // [0] checksum state, [1] whether the executable ends its logic checksum with the
            // GeneralsOnline revision tag (1/0, -1 unknown) -- see pcExeCrcState.
            long[] pcExeCrc = new long[] { -1, -1 };
            List<String> written = extract(tempZip, target, pcExeCrc);
            writeInstalledList(ctx, written);
            writePcExeCrcSeed(ctx, pcExeCrc[0], pcExeCrc[1], version);
            NetworkTrace.write(ctx, "[datapack] installed " + written.size()
                + " file(s) into " + target.getAbsolutePath());

            ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .edit().putString(PREF_INSTALLED_VERSION, version).apply();
            UpdateManager.noteDatapackLatest(ctx, version);

            return Result.success(version, written.size());
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

    /**
     * GeneralsX @feature Android port 27/09/2026 The PC client's EXE checksum, from the PC
     * executable in this very package. PC lobbies refuse a client whose checksum differs from
     * the host's, and "Play with PC players" claims the PC number -- which every PC release
     * changes. The package is the PC release, so the executable is right here: this runs the
     * first half of GlobalData::generateExeCRC() (rotate left by one, add the byte, over the
     * executable, then the 1.4 version number) and leaves the state in files/update, where the
     * engine adds the two .scb scripts from its own file system and claims the result
     * (GlobalData.cpp). scripts/update/pc-exe-crc.py is the same computation on a PC.
     */
    static final String PC_EXE_NAME = "GeneralsOnlineZH_60.exe";
    static final String PC_EXE_SEED_FILE = "pc_exe_crc_seed.txt";
    // generateExeCRC() stops after 1001 blocks of 64 KiB; the same limit keeps the two equal.
    private static final long PC_EXE_CRC_LIMIT = 1001L * 65536L;
    private static final int PC_VERSION_NUMBER = (1 << 16) | 4;

    private static long crcFeed(long crc, int b) {
        long rotated = ((crc << 1) | (crc >>> 31)) & 0xFFFFFFFFL;
        return (rotated + (b & 0xFF)) & 0xFFFFFFFFL;
    }

    // GeneralsX @bugfix Android port 03/10/2026 The GeneralsOnline releases of 22/09-28/09 end
    // every logic checksum with this marker and 0x474F0001 (found by disassembly, GameLogic.cpp);
    // the public source never had it, and 100126 (built from it) does not write it. A phone that
    // still appended it mismatched every 100126 PC at the first checkpoint. Whether this
    // executable has the string decides it, so the next release needs nothing from us either.
    private static final byte[] PC_CRC_REVISION_MARKER =
        "MARKER:OfficialLogicCRCRevision".getBytes(java.nio.charset.StandardCharsets.US_ASCII);

    /** out[0] = checksum state, out[1] = 1/0 whether the revision marker is in the file. */
    private static void pcExeCrcState(InputStream exe, long[] out) throws IOException {
        long crc = 0;
        long done = 0;
        int matched = 0;
        boolean hasMarker = false;
        byte[] buffer = new byte[64 * 1024];
        int read;
        while ((read = exe.read(buffer)) > 0) {
            if (done < PC_EXE_CRC_LIMIT) {
                int use = (int) Math.min(read, PC_EXE_CRC_LIMIT - done);
                for (int i = 0; i < use; i++) {
                    crc = crcFeed(crc, buffer[i]);
                }
                done += use;
            }
            for (int i = 0; i < read && !hasMarker; i++) {
                // The marker has no repeated prefix, so restarting a broken match is exact.
                if (buffer[i] == PC_CRC_REVISION_MARKER[matched]) {
                    if (++matched == PC_CRC_REVISION_MARKER.length) {
                        hasMarker = true;
                    }
                } else {
                    matched = buffer[i] == PC_CRC_REVISION_MARKER[0] ? 1 : 0;
                }
            }
        }
        for (int i = 0; i < 4; i++) {
            crc = crcFeed(crc, PC_VERSION_NUMBER >>> (8 * i));
        }
        out[0] = crc;
        out[1] = hasMarker ? 1 : 0;
    }

    private static void writePcExeCrcSeed(Context ctx, long seed, long crcRevision, String version) throws IOException {
        File dir = new File(ctx.getFilesDir(), "update");
        File file = new File(dir, PC_EXE_SEED_FILE);
        if (seed < 0) {
            // A package without the PC executable: the settings' number is the better guess.
            file.delete();
            return;
        }
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("could not create " + dir.getAbsolutePath());
        }
        try (FileWriter out = new FileWriter(file)) {
            out.write(seed + "\n" + version + "\n" + "crc_revision=" + crcRevision + "\n");
        }
        NetworkTrace.write(ctx, "[datapack] PC exe checksum state " + seed + " from " + PC_EXE_NAME
            + ", logic CRC revision tag " + (crcRevision == 1 ? "present" : "absent"));
    }

    /** Whether the installed package's PC checksum has been computed (see PC_EXE_NAME). */
    static boolean hasPcExeCrcSeed(Context ctx) {
        File file = new File(new File(ctx.getFilesDir(), "update"), PC_EXE_SEED_FILE);
        if (!file.isFile()) {
            return false;
        }
        // A seed written before 03/10/2026 lacks the revision-tag line: install the package once
        // more so it is known (see PC_CRC_REVISION_MARKER).
        try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                if (line.startsWith("crc_revision=")) {
                    return true;
                }
            }
        } catch (IOException e) {
            return false;
        }
        return false;
    }

    /** Extracts the wanted prefixes into targetRoot, listing what it wrote. */
    private static List<String> extract(File zipFile, File targetRoot, long[] pcExeCrc)
            throws IOException {
        String rootPath = targetRoot.getCanonicalPath() + File.separator;
        List<String> written = new ArrayList<>();

        try (ZipInputStream zip = new ZipInputStream(
                new BufferedInputStream(new java.io.FileInputStream(zipFile)))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName().replace('\\', '/');
                if (name.equalsIgnoreCase(PC_EXE_NAME)) {
                    pcExeCrcState(zip, pcExeCrc);
                    continue;
                }
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
                written.add(name);
            }
        }

        if (written.isEmpty()) {
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
