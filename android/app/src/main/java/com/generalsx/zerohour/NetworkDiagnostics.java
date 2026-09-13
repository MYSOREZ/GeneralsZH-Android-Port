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
// "Sign-in doesn't work" is the one bug report we cannot act on. The failure
// is three processes wide -- launcher, browser, game -- across a network we
// cannot see, against a server we do not run, and the user has no adb. Every
// previous round of this has cost days and ended in a guess.
//
// So this asks the questions itself and prints the answers, in the order the
// sign-in actually depends on them:
//
//   device -> network -> DNS -> TLS -> the API's own answers -> local session
//
// The layering matters more than any single probe: an answer at layer N makes
// the layers above it irrelevant. If DNS does not resolve there is no point
// reading anything about HTTP status codes, and the report should make that
// obvious to someone who is not a programmer.
//
// Two of these probes exist because of specific, already-observed failures:
//
//   - Clock skew. Both TLS certificate validation and the API's bearer tokens
//     are time-sensitive. A device whose clock is days off fails sign-in with
//     a TLS error or an instantly-expired session, and the user sees neither
//     -- they see "network error". The Date header of any HTTPS response we
//     get is the server's own clock, so comparing it to ours is free.
//
//   - An unclaimed login code answers 403, not 200. That is the normal,
//     expected answer for every poll before the user finishes signing in, and
//     mistaking it for a transport failure is what broke sign-in (see
//     GeneralsOnlineSession.postJsonOnce). The sweep sends a throwaway code
//     precisely so the report states what the server says today, rather than
//     what we assumed it says when this code was written.

package com.generalsx.zerohour;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;

import org.json.JSONObject;

import java.io.File;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;
import java.util.UUID;

final class NetworkDiagnostics {

    /** Reports progress so the UI can show what is being probed right now. */
    interface Progress {
        void onStep(String label);
    }

    private static final String PREF_INSTALL_ID = "install_id";

    private static final int CONNECT_TIMEOUT_MS = 10000;
    private static final int READ_TIMEOUT_MS = 10000;

    private NetworkDiagnostics() {
    }

    // ---------------------------------------------------------------- identity

    /**
     * A stable per-installation id.
     *
     * The API's auth calls carry three hardware-identity fields
     * (machine_guid / mac_addr / vol_serial) that the Windows client fills
     * from the registry MachineGuid, the first adapter's MAC and the C:
     * volume serial. Android has no equivalent of any of the three, and has
     * deliberately not had one since 6.0: the real MAC is unreadable, and
     * there is no volume serial at all.
     *
     * A random UUID generated once and kept is the honest substitute. It
     * gives the server what those fields are actually for -- one stable
     * value per installation -- without reporting a hardware identifier we
     * did not read, and without the Windows client's "YY"/"ZZ" provenance
     * suffixes, which would claim a source this value does not have.
     *
     * It is per-installation, not per-device: clearing app data or
     * reinstalling produces a new one. That is the correct trade for a value
     * we generate rather than read.
     */
    static String installId(Context ctx) {
        SharedPreferences prefs =
            ctx.getSharedPreferences(GeneralsOnlineSession.PREFS_NAME, Context.MODE_PRIVATE);
        String id = prefs.getString(PREF_INSTALL_ID, null);
        if (id == null || id.isEmpty()) {
            id = UUID.randomUUID().toString();
            prefs.edit().putString(PREF_INSTALL_ID, id).apply();
        }
        return id;
    }

    /**
     * A stable stand-in for the MAC field, derived from the install id.
     *
     * Marked locally-administered (the 0x02 bit) and never taken from a real
     * adapter, so it is structurally what it is: an identifier for this
     * installation formatted the way the field expects, not a hardware
     * address read off the device.
     */
    static String syntheticMac(Context ctx) {
        byte[] digest = sha256(installId(ctx) + "|mac");
        if (digest == null) {
            return "";
        }
        int first = (digest[0] & 0xFE) | 0x02;
        return String.format(Locale.US, "%02X:%02X:%02X:%02X:%02X:%02X",
            first, digest[1], digest[2], digest[3], digest[4], digest[5]);
    }

    /** Same idea for the volume-serial field: 8 hex digits, derived, stable. */
    static String syntheticVolumeSerial(Context ctx) {
        byte[] digest = sha256(installId(ctx) + "|vol");
        if (digest == null) {
            return "";
        }
        return String.format(Locale.US, "%02X%02X%02X%02X",
            digest[0], digest[1], digest[2], digest[3]);
    }

    private static byte[] sha256(String input) {
        try {
            return MessageDigest.getInstance("SHA-256")
                .digest(input.getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            return null;
        }
    }

    // ------------------------------------------------------------------ sweep

    static String run(Context ctx, Progress progress) {
        StringBuilder r = new StringBuilder();
        NetworkTrace.section(ctx, "network diagnostics");

        header(ctx, r);
        deviceAndNetwork(ctx, r, progress);
        dns(r, progress);
        endpoints(ctx, r, progress);
        session(ctx, r);

        r.append('\n');
        r.append("--- end of report ---\n");

        String report = NetworkTrace.redact(r.toString());
        for (String line : report.split("\n")) {
            NetworkTrace.write(ctx, line);
        }
        return report;
    }

    private static void header(Context ctx, StringBuilder r) {
        SimpleDateFormat iso = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US);
        r.append("GeneralsZH network diagnostics\n");
        r.append("run at      : ").append(iso.format(new Date())).append('\n');
        r.append("app version : ").append(versionName(ctx)).append('\n');
        r.append("android     : ").append(Build.VERSION.RELEASE)
            .append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        r.append("device      : ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        r.append("install id  : ").append(installId(ctx)).append('\n');
        r.append('\n');
    }

    private static String versionName(Context ctx) {
        try {
            return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    // ------------------------------------------------------- device + network

    private static void deviceAndNetwork(Context ctx, StringBuilder r, Progress progress) {
        step(progress, "network");
        r.append("[1] Network\n");

        ConnectivityManager cm =
            (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) {
            r.append("  no ConnectivityManager\n\n");
            return;
        }

        Network active = cm.getActiveNetwork();
        if (active == null) {
            r.append("  RESULT: no active network -- the device is offline.\n");
            r.append("  Nothing below this line can succeed until that changes.\n\n");
            return;
        }

        NetworkCapabilities caps = cm.getNetworkCapabilities(active);
        if (caps == null) {
            r.append("  active network present, capabilities unavailable\n\n");
            return;
        }

        String transport = "other";
        if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
            transport = "Wi-Fi";
        } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
            transport = "mobile data";
        } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
            transport = "ethernet";
        }
        boolean vpn = caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN);
        boolean validated = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);

        r.append("  transport   : ").append(transport).append(vpn ? " + VPN" : "").append('\n');
        r.append("  validated   : ").append(validated).append('\n');
        r.append("  metered     : ").append(!caps.hasCapability(
            NetworkCapabilities.NET_CAPABILITY_NOT_METERED)).append('\n');

        LinkProperties link = cm.getLinkProperties(active);
        if (link != null) {
            r.append("  DNS servers : ");
            if (link.getDnsServers().isEmpty()) {
                r.append("(none reported)");
            } else {
                for (int i = 0; i < link.getDnsServers().size(); i++) {
                    if (i > 0) {
                        r.append(", ");
                    }
                    r.append(link.getDnsServers().get(i).getHostAddress());
                }
            }
            r.append('\n');
            if (Build.VERSION.SDK_INT >= 28 && link.isPrivateDnsActive()) {
                r.append("  private DNS : on")
                    .append(link.getPrivateDnsServerName() != null
                        ? " (" + link.getPrivateDnsServerName() + ")" : "")
                    .append('\n');
            }
            if (link.getHttpProxy() != null) {
                r.append("  proxy       : ").append(link.getHttpProxy()).append('\n');
            }
        }

        if (!validated) {
            r.append("  NOTE: Android has not validated this network. That usually means a\n");
            r.append("        captive portal (hotel/cafe Wi-Fi wanting a login page first).\n");
        }
        if (vpn) {
            r.append("  NOTE: a VPN is active. If sign-in fails only with it on, its exit\n");
            r.append("        country may be blocked; if it fails only with it off, the ISP\n");
            r.append("        may be filtering the API host.\n");
        }
        r.append('\n');
    }

    // -------------------------------------------------------------------- DNS

    private static final String[] HOSTS = {
        "api.playgenerals.online",
        "api-ru.playgenerals.online",
        "www.playgenerals.online",
    };

    private static void dns(StringBuilder r, Progress progress) {
        r.append("[2] DNS\n");
        for (String host : HOSTS) {
            step(progress, "DNS " + host);
            long t0 = System.currentTimeMillis();
            try {
                InetAddress[] addrs = InetAddress.getAllByName(host);
                long ms = System.currentTimeMillis() - t0;
                StringBuilder list = new StringBuilder();
                for (int i = 0; i < addrs.length && i < 4; i++) {
                    if (i > 0) {
                        list.append(", ");
                    }
                    list.append(addrs[i].getHostAddress());
                }
                r.append("  OK   ").append(pad(host)).append(' ')
                    .append(list).append("  (").append(ms).append(" ms)\n");
            } catch (UnknownHostException e) {
                r.append("  FAIL ").append(pad(host)).append(" does not resolve\n");
                r.append("       A blocked or hijacked DNS entry looks exactly like this.\n");
            }
        }
        r.append('\n');
    }

    private static String pad(String s) {
        StringBuilder b = new StringBuilder(s);
        while (b.length() < 28) {
            b.append(' ');
        }
        return b.toString();
    }

    // -------------------------------------------------------------- endpoints

    private static void endpoints(Context ctx, StringBuilder r, Progress progress) {
        r.append("[3] Server endpoints\n");

        // The login WEBSITE is a different host from the API, and one can work
        // while the other is blocked -- that asymmetry is the whole reason the
        // alternate API endpoint exists upstream, so the report has to test
        // them separately rather than concluding anything from one of them.
        step(progress, "login site");
        Probe site = probe(ctx, "GET", "https://www.playgenerals.online/", null, null);
        r.append("  login site        : ").append(site.describe()).append('\n');

        step(progress, "API (primary)");
        Probe primary = probeApi(ctx, GeneralsOnlineSession.API_BASE);
        r.append("  API primary       : ").append(primary.describe()).append('\n');

        step(progress, "API (alternate)");
        Probe alt = probeApi(ctx, GeneralsOnlineSession.API_BASE_ALT);
        r.append("  API alternate     : ").append(alt.describe()).append('\n');

        // Clock check, from whichever probe actually reached a server.
        Probe timed = site.serverDateMs > 0 ? site
            : (primary.serverDateMs > 0 ? primary : alt);
        if (timed.serverDateMs > 0) {
            long skew = (timed.localDateMs - timed.serverDateMs) / 1000L;
            r.append("  clock skew        : ").append(skew).append(" s");
            if (Math.abs(skew) > 300) {
                r.append("   <-- PROBLEM");
            }
            r.append('\n');
            if (Math.abs(skew) > 300) {
                r.append("       This device's clock is more than 5 minutes off. HTTPS\n");
                r.append("       certificates and login tokens are both time-checked, so\n");
                r.append("       sign-in can fail for this reason alone. Turn on automatic\n");
                r.append("       date and time in Android settings and run this again.\n");
            }
            r.append("  device time zone  : ").append(TimeZone.getDefault().getID()).append('\n');
        }

        r.append('\n');
        r.append("[4] Sign-in API behaviour\n");
        step(progress, "CheckLogin");

        // A code that was never issued by the website. The answer to this is
        // also the answer the poll loop gets for every second the user spends
        // on the login page, so it documents the normal pending state.
        String throwaway = "diagnostic" + UUID.randomUUID().toString().replace("-", "");
        throwaway = throwaway.substring(0, Math.min(32, throwaway.length()));

        JSONObject body = new JSONObject();
        try {
            body.put("code", throwaway);
            body.put("client_id", GeneralsOnlineSession.CLIENT_ID);
            body.put("machine_guid", installId(ctx));
            body.put("mac_addr", syntheticMac(ctx));
            body.put("vol_serial", syntheticVolumeSerial(ctx));
            body.put("exe_crc", 0);
            body.put("ini_crc", 0);
        } catch (Exception e) {
            // A JSONObject.put of constants cannot actually throw; if it
            // somehow did, an empty body still produces a usable status code.
        }

        Probe check = probe(ctx, "POST",
            GeneralsOnlineSession.API_BASE + "CheckLogin", body.toString(), null);
        r.append("  CheckLogin (unclaimed code)\n");
        r.append("    status  : ").append(check.status > 0
            ? Integer.toString(check.status) : "no response").append('\n');
        if (!check.body.isEmpty()) {
            r.append("    body    : ").append(NetworkTrace.snippet(check.body, 200)).append('\n');
        }
        if (!check.error.isEmpty()) {
            r.append("    error   : ").append(check.error).append('\n');
        }
        r.append("    meaning : ").append(explainCheckLogin(check)).append('\n');
        r.append('\n');
    }

    private static String explainCheckLogin(Probe p) {
        if (p.status <= 0) {
            return "the API could not be reached at all -- see [2] and [3] above";
        }
        switch (p.status) {
            case 200:
                return "the server answers unclaimed codes with 200; polling reads the "
                    + "\"result\" field";
            case 401:
                return "the request was rejected before reaching the login logic "
                    + "(no credentials accepted)";
            case 403:
                // Two different situations share this status, and only the
                // sign-in log can tell them apart: 403 while waiting is
                // normal, 403 that outlives a successful website sign-in is
                // the server refusing the poll itself -- which is how the
                // client_id mismatch presented (see
                // GeneralsOnlineSession.CLIENT_ID).
                return "expected for a code nobody has claimed. During a real sign-in "
                    + "this is also the normal answer while waiting -- but if it "
                    + "continues after the website says you are signed in, the server "
                    + "is refusing the poll, not waiting for you.";
            case 423:
                return "this account is banned server-side";
            default:
                return "unexpected status for an unclaimed code; the API contract may "
                    + "have changed again";
        }
    }

    // ---------------------------------------------------------------- session

    private static void session(Context ctx, StringBuilder r) {
        r.append("[5] Stored sign-in\n");
        SharedPreferences prefs =
            ctx.getSharedPreferences(GeneralsOnlineSession.PREFS_NAME, Context.MODE_PRIVATE);
        String session = prefs.getString(GeneralsOnlineSession.PREF_SESSION_TOKEN, "");
        String refresh = prefs.getString(GeneralsOnlineSession.PREF_REFRESH_TOKEN, "");
        long userId = prefs.getLong(GeneralsOnlineSession.PREF_USER_ID, -1);
        String name = prefs.getString(GeneralsOnlineSession.PREF_DISPLAY_NAME, "");

        if (session.isEmpty() && refresh.isEmpty()) {
            r.append("  not signed in on this device\n");
        } else {
            r.append("  user id       : ").append(userId).append('\n');
            r.append("  display name  : ").append(name.isEmpty() ? "(none)" : name).append('\n');
            r.append("  session token : ").append(session.isEmpty()
                ? "(none)" : session.length() + " chars").append('\n');
            r.append("  refresh token : ").append(refresh.isEmpty()
                ? "(none)" : refresh.length() + " chars").append('\n');
        }

        File marker = new File(ctx.getFilesDir(), GeneralsOnlineSession.SESSION_MARKER_NAME);
        r.append("  game marker   : ").append(marker.isFile()
            ? "present (" + marker.length() + " bytes) -- the game can read this session"
            : "absent -- the game will start signed out").append('\n');
    }

    // ------------------------------------------------------------------ probe

    private static final class Probe {
        int status = -1;
        String body = "";
        String error = "";
        long ms = -1;
        long serverDateMs = -1;
        long localDateMs = -1;

        String describe() {
            if (status > 0) {
                return "HTTP " + status + " (" + ms + " ms)";
            }
            return "unreachable -- " + (error.isEmpty() ? "no response" : error);
        }
    }

    private static Probe probeApi(Context ctx, String base) {
        // ServiceConfig is the API's own reachability check: the game fetches
        // it before anything else and treats any failure as "use defaults",
        // so whatever it answers is safe to ask for from here too.
        return probe(ctx, "GET", base + "ServiceConfig", null, null);
    }

    private static Probe probe(Context ctx, String method, String urlText,
                               String postBody, String bearer) {
        Probe p = new Probe();
        HttpURLConnection conn = null;
        long t0 = System.currentTimeMillis();
        try {
            URL url = new URL(urlText);
            conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod(method);
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            if (bearer != null) {
                conn.setRequestProperty("Authorization", "Bearer " + bearer);
            }
            if (postBody != null) {
                conn.setRequestProperty("Content-Type", "application/json");
                conn.setDoOutput(true);
                conn.getOutputStream().write(postBody.getBytes(StandardCharsets.UTF_8));
            }

            p.status = conn.getResponseCode();
            p.ms = System.currentTimeMillis() - t0;
            p.localDateMs = System.currentTimeMillis();
            p.serverDateMs = conn.getHeaderFieldDate("Date", -1);

            InputStream in = (p.status >= 200 && p.status < 300)
                ? conn.getInputStream() : conn.getErrorStream();
            if (in != null) {
                p.body = readSome(in, 4096);
            }
        } catch (Exception e) {
            p.ms = System.currentTimeMillis() - t0;
            p.error = e.getClass().getSimpleName()
                + (e.getMessage() != null ? ": " + e.getMessage() : "");
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }

        NetworkTrace.write(ctx, "  probe " + method + ' ' + urlText
            + " -> " + (p.status > 0 ? "HTTP " + p.status : "ERR " + p.error)
            + (p.body.isEmpty() ? "" : "  body=" + NetworkTrace.snippet(p.body, 200)));
        return p;
    }

    private static String readSome(InputStream in, int max) {
        try {
            byte[] buf = new byte[max];
            int total = 0;
            int n;
            while (total < max && (n = in.read(buf, total, max - total)) != -1) {
                total += n;
            }
            return new String(buf, 0, total, StandardCharsets.UTF_8);
        } catch (Exception e) {
            return "";
        }
    }

    private static void step(Progress progress, String label) {
        if (progress != null) {
            progress.onStep(label);
        }
    }
}
