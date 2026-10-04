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

// GeneralsX @feature Android port 04/10/2026 Rooms: which servers exist and how close they are.
//
// The public list is servers.json in MYSOREZ/Generals-Servers, rebuilt hourly from the servers
// themselves (they announce to each other; see that repository's README). A server installed a
// minute ago is not in it yet, so the live servers are also asked for their /peers. Every
// candidate is then asked /health, and the ones that answer are shown by round-trip time. The
// player's own servers (installed from here, or typed) are kept on this phone and always shown.

package com.generalsx.zerohour;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

final class RoomServers {
    static final String DEFAULT_REGISTRY =
        "https://raw.githubusercontent.com/MYSOREZ/Generals-Servers/main/servers.json";
    private static final String PREFS = "rooms";
    private static final String PREF_MINE = "my_servers";
    private static final int MAX_CANDIDATES = 80;

    static final class Server {
        final String url;
        final boolean mine;
        String name = "";
        boolean alive;
        long pingMs;
        int rooms;
        int players;

        Server(String url, boolean mine) {
            this.url = url;
            this.mine = mine;
        }
    }

    private RoomServers() {
    }

    /** wss://host[:port]/ws in one spelling, or null. Plain ws:// is kept for local tests. */
    static String normalize(String raw) {
        if (raw == null) {
            return null;
        }
        String s = raw.trim();
        if (s.isEmpty()) {
            return null;
        }
        if (!s.contains("://")) {
            s = "wss://" + s;
        }
        try {
            URI u = new URI(s);
            String scheme = u.getScheme() == null ? "" : u.getScheme().toLowerCase(Locale.ROOT);
            if ((!scheme.equals("wss") && !scheme.equals("ws")) || u.getHost() == null) {
                return null;
            }
            boolean defaultPort = u.getPort() == -1 || (scheme.equals("wss") && u.getPort() == 443);
            return scheme + "://" + u.getHost().toLowerCase(Locale.ROOT)
                + (defaultPort ? "" : ":" + u.getPort()) + "/ws";
        } catch (Exception e) {
            return null;
        }
    }

    static String httpBase(String wsUrl) {
        return wsUrl.replaceFirst("^ws", "http").replaceAll("/ws$", "");
    }

    static Set<String> mine(Context ctx) {
        String all = prefs(ctx).getString(PREF_MINE, "");
        Set<String> out = new LinkedHashSet<>();
        for (String s : all.split("\n")) {
            String n = normalize(s);
            if (n != null) {
                out.add(n);
            }
        }
        return out;
    }

    static void addMine(Context ctx, String url) {
        String n = normalize(url);
        if (n == null) {
            return;
        }
        Set<String> all = mine(ctx);
        all.add(n);
        prefs(ctx).edit().putString(PREF_MINE, android.text.TextUtils.join("\n", all)).apply();
    }

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** Blocking: reads the list, asks the servers, and returns them, reachable ones first by ping. */
    static List<Server> load(Context ctx) {
        Set<String> myServers = mine(ctx);
        Set<String> candidates = new LinkedHashSet<>(myServers);
        String registry = UpdateManager.remoteConfig(ctx, "rooms_registry", DEFAULT_REGISTRY);
        try {
            JSONArray list = new JSONObject(get(registry, 10000)).optJSONArray("servers");
            for (int i = 0; list != null && i < list.length(); i++) {
                String n = normalize(list.getJSONObject(i).optString("url"));
                if (n != null) {
                    candidates.add(n);
                }
            }
        } catch (Exception e) {
            // Offline or GitHub unreachable: the player's own servers are still worth showing.
        }

        ExecutorService pool = Executors.newFixedThreadPool(8);
        try {
            List<Future<List<String>>> peerLookups = new ArrayList<>();
            for (final String c : candidates) {
                peerLookups.add(pool.submit(() -> peersOf(c)));
            }
            for (Future<List<String>> f : peerLookups) {
                try {
                    for (String p : f.get(12, TimeUnit.SECONDS)) {
                        String n = normalize(p);
                        if (n != null && candidates.size() < MAX_CANDIDATES) {
                            candidates.add(n);
                        }
                    }
                } catch (Exception e) {
                    // That server did not answer; its peers stay unknown.
                }
            }

            List<Server> servers = new ArrayList<>();
            List<Future<?>> checks = new ArrayList<>();
            for (String c : candidates) {
                final Server s = new Server(c, myServers.contains(c));
                servers.add(s);
                checks.add(pool.submit(() -> health(s)));
            }
            for (Future<?> f : checks) {
                try {
                    f.get(12, TimeUnit.SECONDS);
                } catch (Exception e) {
                    // Counted as not answering.
                }
            }
            List<Server> shown = new ArrayList<>();
            for (Server s : servers) {
                if (s.alive || s.mine) {
                    shown.add(s);
                }
            }
            Collections.sort(shown, (a, b) -> a.alive != b.alive ? (a.alive ? -1 : 1) : Long.compare(a.pingMs, b.pingMs));
            return shown;
        } finally {
            pool.shutdownNow();
        }
    }

    private static List<String> peersOf(String wsUrl) throws Exception {
        List<String> out = new ArrayList<>();
        JSONArray peers = new JSONObject(get(httpBase(wsUrl) + "/peers", 6000)).optJSONArray("peers");
        for (int i = 0; peers != null && i < peers.length(); i++) {
            out.add(peers.getJSONObject(i).optString("url"));
        }
        return out;
    }

    private static void health(Server s) {
        long t0 = System.nanoTime();
        try {
            JSONObject h = new JSONObject(get(httpBase(s.url) + "/health", 6000));
            if (h.optInt("protocol") != RoomSession.PROTOCOL) {
                return;
            }
            s.pingMs = (System.nanoTime() - t0) / 1_000_000;
            s.name = h.optString("name", "");
            s.rooms = h.optInt("rooms");
            s.players = h.optInt("players");
            s.alive = true;
        } catch (Exception e) {
            s.alive = false;
        }
    }

    static String get(String url, int timeoutMs) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs);
        c.setInstanceFollowRedirects(false);
        try (InputStream in = c.getInputStream()) {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0 && body.size() < (1 << 20)) {
                body.write(buf, 0, n);
            }
            return body.toString(StandardCharsets.UTF_8.name());
        } finally {
            c.disconnect();
        }
    }
}
