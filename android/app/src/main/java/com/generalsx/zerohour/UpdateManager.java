package com.generalsx.zerohour;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.util.Base64;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.X509EncodedKeySpec;
import java.util.Iterator;
import java.util.zip.GZIPInputStream;

/**
 * GeneralsX @feature Android port 27/09/2026 Updates without reinstalling the APK.
 *
 * The repository's {@code updates} branch holds one commit: {@code manifest.json}, its signature
 * {@code manifest.json.sig}, and, when there is one, a newer engine ({@code engine/<seq>/
 * libmain.so.gz}, {@code libmain60.so.gz}). The launcher fetches the manifest, and uses it only if
 *
 * <ul>
 *   <li>the signature verifies against {@link #PUBLIC_KEY_B64} (ECDSA P-256 over the exact bytes;
 *       the private key never enters the repository -- see docs/HOWTO/PUBLISH_UPDATE.md), and</li>
 *   <li>its {@code serial} is not lower than the last one accepted, so an old signed manifest
 *       cannot be replayed to roll players back.</li>
 * </ul>
 *
 * Nothing is secret here: the manifest is public, and so are the service addresses in it. What
 * the signature guards against is substitution -- a changed file, whether on the way or in the
 * repository, is refused.
 *
 * What a verified manifest can carry:
 * <ul>
 *   <li>{@code config}: string values written to {@code files/update/remote_config.ini}, read by
 *       the engine at startup (GXRemoteConfig.h) -- today the STUN and TURN server lists.</li>
 *   <li>{@code support}: url, sha256 and size of {@code support.json}, the Help page's
 *       "Support the project" card ({@link SupportLinks}).</li>
 *   <li>{@code engine}: a newer engine build. Its files are downloaded, checked against the
 *       SHA-256 in the (signed) manifest, and loaded instead of the APK's own by
 *       {@link GeneralsZHActivity} -- but only when its {@code seq} is higher than the APK's
 *       bundled engine (assets/engine_build.txt, the commit count it was built at) and the
 *       libraries it was linked against ({@code requires_libs}) are exactly the ones this APK
 *       installed. An engine that twice fails to reach the main menu is dropped
 *       ({@link #noteEngineBoot}).</li>
 * </ul>
 */
final class UpdateManager {
    private static final String TAG = "GXUpdate";

    static final String BASE_URL =
        "https://raw.githubusercontent.com/MYSOREZ/GeneralsZH-Android-Port/updates/";

    /** SubjectPublicKeyInfo (DER, base64) of the update signing key. */
    static final String PUBLIC_KEY_B64 =
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEFjy+4K0lTRmnwQe+nQqXreCMtJehCl1wiYNgq5Rr/MHWkDukps0eUbmuyxSenyFL4T5zo+WBIFeLDO5PXFoG/A==";

    static final String[] ENGINE_LIBS = { "libmain.so", "libmain60.so" };

    private static final String PREFS = "gx_update";
    private static final String KEY_SERIAL = "serial";
    private static final String KEY_AUTO = "auto_check";
    private static final String KEY_LAST_CHECK = "last_check";
    private static final String KEY_DEPS_OK_FOR = "deps_ok_for";
    private static final String KEY_DATAPACK_LATEST = "datapack_latest";
    private static final String KEY_SETTINGS_DATE = "settings_date";

    private UpdateManager() {
    }

    // ---------------------------------------------------------------------------------------
    // Paths

    static File updateDir(Context ctx) {
        return new File(ctx.getFilesDir(), "update");
    }

    private static File engineRoot(Context ctx) {
        return new File(updateDir(ctx), "engine");
    }

    private static File activeEngineMarker(Context ctx) {
        return new File(updateDir(ctx), "engine_active.txt");
    }

    /** Written before an updated engine is loaded, deleted by the engine at its main menu. */
    private static File bootPendingMarker(Context ctx) {
        return new File(updateDir(ctx), "boot_pending");
    }

    private static File badEngineMarker(Context ctx) {
        return new File(updateDir(ctx), "engine_bad.txt");
    }

    // ---------------------------------------------------------------------------------------
    // Settings

    static boolean isAutoCheckEnabled(Context ctx) {
        return prefs(ctx).getBoolean(KEY_AUTO, true);
    }

    static void setAutoCheckEnabled(Context ctx, boolean enabled) {
        prefs(ctx).edit().putBoolean(KEY_AUTO, enabled).apply();
    }

    static long lastCheckMillis(Context ctx) {
        return prefs(ctx).getLong(KEY_LAST_CHECK, 0L);
    }

    /** When the settings in use were published, or null while only the built-in ones exist. */
    static java.util.Date settingsPublished(Context ctx) {
        String date = prefs(ctx).getString(KEY_SETTINGS_DATE, null);
        if (date == null || acceptedSerial(ctx) <= 0) {
            return null;
        }
        try {
            return new java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US).parse(date);
        } catch (java.text.ParseException e) {
            return null;
        }
    }

    static int acceptedSerial(Context ctx) {
        return prefs(ctx).getInt(KEY_SERIAL, 0);
    }

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    // ---------------------------------------------------------------------------------------
    // Engine selection (called by GeneralsZHActivity before SDL loads the libraries)

    /** The commit count the APK's own engine was built at; 0 if the APK does not say. */
    static int bundledEngineSeq(Context ctx) {
        try (InputStream in = ctx.getAssets().open("engine_build.txt")) {
            String text = new String(readAll(in), StandardCharsets.US_ASCII).trim();
            return Integer.parseInt(text);
        } catch (IOException | NumberFormatException e) {
            return 0;
        }
    }

    /** The downloaded engine that is ready to run, or 0 if the APK's own engine should run. */
    static int activeEngineSeq(Context ctx) {
        int seq = readInt(activeEngineMarker(ctx));
        if (seq <= 0 || seq <= bundledEngineSeq(ctx) || seq == readInt(badEngineMarker(ctx))) {
            return 0;
        }
        for (String lib : ENGINE_LIBS) {
            if (!new File(new File(engineRoot(ctx), Integer.toString(seq)), lib).isFile()) {
                return 0;
            }
        }
        if (!dependenciesStillMatch(ctx, seq)) {
            return 0;
        }
        return seq;
    }

    /** Absolute path of an updated engine library, or null to load the APK's own. */
    static String activeEngineLibrary(Context ctx, String libName) {
        int seq = activeEngineSeq(ctx);
        if (seq == 0) {
            return null;
        }
        return new File(new File(engineRoot(ctx), Integer.toString(seq)), libName).getAbsolutePath();
    }

    /**
     * Record that an updated engine is about to start. If the marker is still there from the
     * previous start of the same engine -- it never reached the main menu -- count it; on the
     * second such failure the engine is marked bad and the APK's own runs from then on.
     * @return true if the updated engine may be used for this start.
     */
    static boolean noteEngineBoot(Context ctx, int seq) {
        File pending = bootPendingMarker(ctx);
        int failures = 0;
        if (pending.isFile()) {
            String[] parts = readText(pending).trim().split(" ");
            if (parts.length == 2 && Integer.toString(seq).equals(parts[0])) {
                try {
                    failures = Integer.parseInt(parts[1]) + 1;
                } catch (NumberFormatException ignored) {
                    failures = 1;
                }
            }
        }
        if (failures >= 2) {
            Log.w(TAG, "engine " + seq + " failed to reach the main menu twice; using the APK's engine");
            writeText(badEngineMarker(ctx), Integer.toString(seq));
            pending.delete();
            return false;
        }
        writeText(pending, seq + " " + failures);
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Checking

    static final class Result {
        boolean ok;
        String error;
        int serial;
        boolean configUpdated;
        boolean supportUpdated;
        int engineSeq;              // engine offered by the manifest, 0 if none
        boolean engineDownloaded;   // newly downloaded and ready for the next start
        boolean engineIncompatible; // offered but built against other libraries -- needs a new APK
        boolean offline;            // no network: nothing changed, the last good update stays in use
        String datapackAvailable;   // newer community data on the GeneralsOnline CDN, not yet installed
        String datapackInstalled;   // community data updated by this check
    }

    /** A value from the verified settings (files/update/remote_config.ini), or fallback. */
    static String remoteConfig(Context ctx, String key, String fallback) {
        File file = new File(updateDir(ctx), "remote_config.ini");
        if (file.isFile()) {
            for (String line : readText(file).split("\n")) {
                int eq = line.indexOf('=');
                if (!line.startsWith("#") && eq > 0 && line.substring(0, eq).equals(key)) {
                    String value = line.substring(eq + 1).trim();
                    return value.isEmpty() ? fallback : value;
                }
            }
        }
        return fallback;
    }

    /**
     * Blocking; call off the UI thread. Always applies the signed settings (network servers, the
     * PC checksum: a few lines, and the game needs them current whichever screen checked) and
     * notices a newer community data patch; the patch itself is installed by the multiplayer
     * screen (checkDatapackOnly).
     * @param withEngine also download a newer engine -- the Updates card on the home screen, which
     *        is the engine's place; the multiplayer screen passes false.
     */
    static Result check(Context ctx, boolean withEngine) {
        Result r = new Result();
        try {
            byte[] manifestBytes = download(BASE_URL + "manifest.json", 256 * 1024);
            byte[] signatureText = download(BASE_URL + "manifest.json.sig", 16 * 1024);
            if (!verify(manifestBytes, signatureText)) {
                r.error = "signature";
                return r;
            }
            JSONObject manifest = new JSONObject(new String(manifestBytes, StandardCharsets.UTF_8));
            r.serial = manifest.optInt("serial", 0);
            if (r.serial < acceptedSerial(ctx)) {
                r.error = "older manifest (" + r.serial + " < " + acceptedSerial(ctx) + ")";
                return r;
            }

            File dir = updateDir(ctx);
            if (!dir.isDirectory() && !dir.mkdirs()) {
                r.error = "cannot create " + dir;
                return r;
            }

            JSONObject config = manifest.optJSONObject("config");
            if (config != null) {
                r.configUpdated = writeRemoteConfig(ctx, config);
            }

            r.supportUpdated = applySupport(ctx, manifest.optJSONObject("support"));

            JSONObject engine = manifest.optJSONObject("engine");
            if (engine != null && withEngine) {
                applyEngine(ctx, engine, r);
            }

            noticeNewerDatapack(ctx, r);

            writeBytes(new File(dir, "manifest.json"), manifestBytes);
            SharedPreferences.Editor edit = prefs(ctx).edit();
            if (r.serial != acceptedSerial(ctx) || !prefs(ctx).contains(KEY_SETTINGS_DATE)) {
                // Players see the settings by date, not by serial. A manifest from before the
                // "published" field is dated by the day it arrived.
                String published = manifest.optString("published", "");
                edit.putString(KEY_SETTINGS_DATE, published.matches("\\d{4}-\\d{2}-\\d{2}")
                    ? published
                    : new java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US)
                        .format(new java.util.Date()));
            }
            edit
                .putInt(KEY_SERIAL, r.serial)
                .putLong(KEY_LAST_CHECK, System.currentTimeMillis())
                .apply();
            r.ok = r.error == null;
        } catch (java.net.UnknownHostException | java.net.ConnectException
                 | java.net.SocketTimeoutException | java.net.NoRouteToHostException e) {
            // Offline is a normal state, not an error: the launcher and the game work without a
            // network, and whatever was verified last time (settings, engine) stays in use.
            r.offline = true;
            r.error = "offline";
        } catch (Exception e) {
            Log.w(TAG, "update check failed", e);
            String msg = e.getMessage();
            r.error = (msg == null || msg.isEmpty()) ? e.getClass().getSimpleName() : msg;
        }
        return r;
    }

    /**
     * GeneralsX @feature Android port 27/09/2026 The community data patch comes straight from
     * the GeneralsOnline CDN, verified by the SHA-256 in its own manifest (DataPackInstaller).
     * Only a player who installed it is kept current -- nothing is pushed on anyone else.
     *
     * The multiplayer screen's data card is where the patch is checked and installed
     * (checkDatapackOnly); the Updates card only notices a newer version (noticeNewerDatapack)
     * and points there.
     */
    private static void checkDatapack(Context ctx, Result r, boolean install,
                                      DataPackInstaller.Progress progress) {
        if (DataPackInstaller.installedVersion(ctx) == null) {
            return;
        }
        String latest = DataPackInstaller.latestVersion(ctx);
        if (latest == null) {
            return;
        }
        noteDatapackLatest(ctx, latest);
        if (!datapackUpdateWanted(ctx)) {
            return;
        }
        if (!install) {
            r.datapackAvailable = latest;
            return;
        }
        // The Updates card and the data card can both get here at once; the second one waits
        // for the first and then finds nothing left to do instead of downloading it again.
        synchronized (DataPackInstaller.INSTALL_LOCK) {
            if (!datapackUpdateWanted(ctx)) {
                return;
            }
            DataPackInstaller.Result result = DataPackInstaller.install(ctx, progress);
            if (result.ok) {
                r.datapackInstalled = result.version;
            } else {
                r.datapackAvailable = latest;
            }
        }
    }

    private static void noticeNewerDatapack(Context ctx, Result r) {
        if (DataPackInstaller.installedVersion(ctx) == null) {
            return;
        }
        noteDatapackLatest(ctx, DataPackInstaller.latestVersion(ctx));
        if (datapackNewerAvailable(ctx)) {
            r.datapackAvailable = datapackLatestSeen(ctx);
        }
    }

    /** The CDN has a patch version other than the installed one (as of the last check). */
    static boolean datapackNewerAvailable(Context ctx) {
        String installed = DataPackInstaller.installedVersion(ctx);
        String latest = datapackLatestSeen(ctx);
        return installed != null && latest != null && !latest.equals(installed);
    }

    /** The data patch part of check() alone. Blocking; call off the UI thread. */
    static Result checkDatapackOnly(Context ctx, boolean install, DataPackInstaller.Progress progress) {
        Result r = new Result();
        checkDatapack(ctx, r, install, progress);
        r.ok = true;
        return r;
    }

    static void noteDatapackLatest(Context ctx, String version) {
        if (version != null && !version.isEmpty()) {
            prefs(ctx).edit().putString(KEY_DATAPACK_LATEST, version).apply();
        }
    }

    /** The newest patch version the CDN reported at the last check, or null. */
    static String datapackLatestSeen(Context ctx) {
        return prefs(ctx).getString(KEY_DATAPACK_LATEST, null);
    }

    /**
     * An installed patch is out of date when the CDN has another version, or when it was
     * installed before the launcher computed the PC checksum from it -- then it is fetched once
     * more, so cross-play claims the number of the PC release this device actually has.
     */
    static boolean datapackUpdateWanted(Context ctx) {
        String installed = DataPackInstaller.installedVersion(ctx);
        if (installed == null) {
            return false;
        }
        String latest = datapackLatestSeen(ctx);
        return (latest != null && !latest.equals(installed)) || !DataPackInstaller.hasPcExeCrcSeed(ctx);
    }

    static boolean isUnmeteredNetwork(Context ctx) {
        android.net.ConnectivityManager cm =
            (android.net.ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        return cm != null && !cm.isActiveNetworkMetered();
    }

    private static boolean writeRemoteConfig(Context ctx, JSONObject config) throws IOException {
        StringBuilder ini = new StringBuilder();
        ini.append("# Written by the launcher from a signed update manifest. Do not edit.\n");
        for (Iterator<String> it = config.keys(); it.hasNext(); ) {
            String key = it.next();
            String value = config.optString(key, "");
            if (!key.matches("[a-z0-9_]+") || value.indexOf('\n') >= 0) {
                continue;
            }
            ini.append(key).append('=').append(value).append('\n');
        }
        File file = new File(updateDir(ctx), "remote_config.ini");
        String before = file.isFile() ? readText(file) : "";
        String after = ini.toString();
        if (before.equals(after)) {
            return false;
        }
        writeBytes(file, after.getBytes(StandardCharsets.UTF_8));
        return true;
    }

    /**
     * GeneralsX @feature Android port 03/10/2026 The Help page's support card (SupportLinks). The
     * manifest names the file's SHA-256, so the file is as trusted as the signed manifest; a
     * manifest without the entry withdraws the card. A file that fails to download or to match is
     * not an error for the rest of the update: the previous verified copy stays.
     * @return whether the copy on disk changed.
     */
    private static boolean applySupport(Context ctx, JSONObject support) {
        File file = new File(updateDir(ctx), SupportLinks.FILE_NAME);
        if (support == null) {
            return file.exists() && file.delete();
        }
        String sha = support.optString("sha256", "");
        String url = support.optString("url", "");
        try {
            if (file.isFile() && sha256(file).equalsIgnoreCase(sha)) {
                return false;
            }
            if (!url.startsWith(BASE_URL)) {
                Log.w(TAG, "support file outside the updates branch: " + url);
                return false;
            }
            byte[] data = download(url, 256 * 1024);
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            if (!hex(digest.digest(data)).equalsIgnoreCase(sha)) {
                Log.w(TAG, "support file does not match the signed manifest");
                return false;
            }
            writeBytes(file, data);
            return true;
        } catch (Exception e) {
            Log.w(TAG, "support file not updated", e);
            return false;
        }
    }

    private static void applyEngine(Context ctx, JSONObject engine, Result r) throws Exception {
        int seq = engine.optInt("seq", 0);
        r.engineSeq = seq;
        if (seq <= bundledEngineSeq(ctx) || seq == readInt(badEngineMarker(ctx))) {
            return;
        }
        JSONObject requires = engine.optJSONObject("requires_libs");
        if (requires == null || !librariesMatch(ctx, requires)) {
            r.engineIncompatible = true;
            return;
        }
        File target = new File(engineRoot(ctx), Integer.toString(seq));
        JSONObject files = engine.getJSONObject("files");
        boolean all = true;
        for (String lib : ENGINE_LIBS) {
            JSONObject entry = files.optJSONObject(lib);
            if (entry == null) {
                all = false;
                break;
            }
            File out = new File(target, lib);
            String sha = entry.getString("sha256");
            if (out.isFile() && sha.equalsIgnoreCase(sha256(out))) {
                continue;
            }
            downloadEngineFile(entry.getString("url"), entry.optLong("size", 0L), sha, out);
        }
        if (!all) {
            r.error = "engine entry is missing a library";
            return;
        }
        boolean wasActive = readInt(activeEngineMarker(ctx)) == seq;
        writeText(activeEngineMarker(ctx), Integer.toString(seq));
        prefs(ctx).edit().putString(KEY_DEPS_OK_FOR, depsStamp(ctx, seq)).apply();
        pruneOtherEngines(ctx, seq);
        r.engineDownloaded = !wasActive;
    }

    // ---------------------------------------------------------------------------------------
    // Library compatibility

    private static boolean librariesMatch(Context ctx, JSONObject requires) throws IOException {
        File libDir = new File(ctx.getApplicationInfo().nativeLibraryDir);
        for (Iterator<String> it = requires.keys(); it.hasNext(); ) {
            String lib = it.next();
            File installed = new File(libDir, lib);
            if (!installed.isFile() || !requires.optString(lib, "").equalsIgnoreCase(sha256(installed))) {
                Log.i(TAG, "engine needs a different " + lib + " than this APK installed");
                return false;
            }
        }
        return true;
    }

    /** Re-checked after the APK itself is updated, since that replaces the libraries. */
    private static boolean dependenciesStillMatch(Context ctx, int seq) {
        String stamp = depsStamp(ctx, seq);
        if (stamp.equals(prefs(ctx).getString(KEY_DEPS_OK_FOR, ""))) {
            return true;
        }
        try {
            File manifestFile = new File(updateDir(ctx), "manifest.json");
            JSONObject engine = new JSONObject(readText(manifestFile)).optJSONObject("engine");
            if (engine == null || engine.optInt("seq", 0) != seq) {
                return false;
            }
            JSONObject requires = engine.optJSONObject("requires_libs");
            if (requires != null && librariesMatch(ctx, requires)) {
                prefs(ctx).edit().putString(KEY_DEPS_OK_FOR, stamp).apply();
                return true;
            }
        } catch (Exception e) {
            Log.w(TAG, "could not re-check the engine's libraries", e);
        }
        return false;
    }

    private static String depsStamp(Context ctx, int seq) {
        long installed = 0L;
        try {
            PackageInfo info = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            installed = info.lastUpdateTime;
        } catch (Exception ignored) {
            // an unknown install time just forces a re-check
        }
        return seq + ":" + installed;
    }

    private static void pruneOtherEngines(Context ctx, int keep) {
        File[] dirs = engineRoot(ctx).listFiles();
        if (dirs == null) {
            return;
        }
        for (File d : dirs) {
            if (d.isDirectory() && !d.getName().equals(Integer.toString(keep))) {
                File[] files = d.listFiles();
                if (files != null) {
                    for (File f : files) {
                        f.delete();
                    }
                }
                d.delete();
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // Crypto and transfer

    static boolean verify(byte[] data, byte[] signatureText) {
        try {
            byte[] keyBytes = Base64.decode(PUBLIC_KEY_B64, Base64.DEFAULT);
            PublicKey key = KeyFactory.getInstance("EC").generatePublic(new X509EncodedKeySpec(keyBytes));
            byte[] sig = Base64.decode(new String(signatureText, StandardCharsets.US_ASCII).trim(), Base64.DEFAULT);
            Signature verifier = Signature.getInstance("SHA256withECDSA");
            verifier.initVerify(key);
            verifier.update(data);
            return verifier.verify(sig);
        } catch (Exception e) {
            Log.w(TAG, "signature check failed", e);
            return false;
        }
    }

    private static byte[] download(String url, int maxBytes) throws IOException {
        HttpURLConnection conn = open(url);
        try (InputStream in = conn.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                if (out.size() > maxBytes) {
                    throw new IOException("response too large: " + url);
                }
            }
            return out.toByteArray();
        } finally {
            conn.disconnect();
        }
    }

    /** Downloads a .gz, inflates it to a temp file, checks size and SHA-256, then moves it in. */
    private static void downloadEngineFile(String url, long size, String sha256, File out) throws Exception {
        File parent = out.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("cannot create " + parent);
        }
        File tmp = new File(out.getPath() + ".part");
        HttpURLConnection conn = open(url);
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        long written = 0;
        try (InputStream raw = conn.getInputStream();
             InputStream in = url.endsWith(".gz") ? new GZIPInputStream(raw, 65536) : raw;
             OutputStream os = new FileOutputStream(tmp)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
                digest.update(buf, 0, n);
                written += n;
                if (size > 0 && written > size) {
                    throw new IOException("engine file larger than the manifest says");
                }
            }
        } finally {
            conn.disconnect();
        }
        String got = hex(digest.digest());
        if ((size > 0 && written != size) || !got.equalsIgnoreCase(sha256)) {
            tmp.delete();
            throw new IOException("engine file does not match the signed manifest: " + out.getName());
        }
        if (out.exists() && !out.delete()) {
            tmp.delete();
            throw new IOException("cannot replace " + out);
        }
        if (!tmp.renameTo(out)) {
            tmp.delete();
            throw new IOException("cannot move " + tmp + " to " + out);
        }
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(15000);
        conn.setReadTimeout(60000);
        conn.setUseCaches(false);
        int status = conn.getResponseCode();
        if (status < 200 || status >= 300) {
            conn.disconnect();
            throw new IOException("HTTP " + status + " for " + url);
        }
        return conn;
    }

    static String sha256(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                digest.update(buf, 0, n);
            }
            return hex(digest.digest());
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }

    private static String hex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(String.format(java.util.Locale.ROOT, "%02x", b & 0xff));
        }
        return sb.toString();
    }

    // ---------------------------------------------------------------------------------------
    // Small file helpers

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private static String readText(File file) {
        try (InputStream in = new FileInputStream(file)) {
            return new String(readAll(in), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private static int readInt(File file) {
        try {
            return file.isFile() ? Integer.parseInt(readText(file).trim()) : 0;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static void writeText(File file, String text) {
        try {
            writeBytes(file, text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "cannot write " + file, e);
        }
    }

    private static void writeBytes(File file, byte[] bytes) throws IOException {
        File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("cannot create " + parent);
        }
        File tmp = new File(file.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(bytes);
        }
        if (!tmp.renameTo(file)) {
            file.delete();
            if (!tmp.renameTo(file)) {
                throw new IOException("cannot write " + file);
            }
        }
    }
}
