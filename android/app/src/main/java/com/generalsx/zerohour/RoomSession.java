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

// GeneralsX @feature Android port 04/10/2026 Rooms: one room's WebSocket, for the whole process.
//
// The room is joined in the launcher (RoomActivity) and carried on into the game: once the
// creator launches, the engine's LAN sockets (udp.cpp) send and receive through this session via
// GeneralsZHActivity.roomSend / roomReceive, and the game is played from its ordinary LAN screen.
// Protocol: docs/port/ROOMS_PROTOCOL.md. The relay is any server the player picks.
//
// Everything here is reached from two threads -- the UI and the engine's -- so the state is
// guarded by this object; OkHttp's callbacks take the same lock.

package com.generalsx.zerohour;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

final class RoomSession {
    private static final String TAG = "GXRooms";

    static final int PROTOCOL = 1;
    static final int MAGIC = 0x47585231; // "GXR1"
    static final int IP_BASE = 0x0AF00000; // 10.240.0.0
    static final int BROADCAST = 0xFFFFFFFF;
    static final int HEADER = 16;
    static final int MAX_PAYLOAD = 16384;
    private static final int MAX_QUEUED_FRAMES = 512;
    private static final int MAX_QUEUED_BYTES = 1 << 20;
    private static final long RESUME_WINDOW_MS = 55_000;

    static final RoomSession INSTANCE = new RoomSession();

    static final class Member {
        final int slot;
        final String name;
        final boolean ready;
        final boolean connected;
        final String files;

        Member(int slot, String name, boolean ready, boolean connected, String files) {
            this.slot = slot;
            this.name = name;
            this.ready = ready;
            this.connected = connected;
            this.files = files;
        }
    }

    enum State { IDLE, CONNECTING, IN_ROOM, RECONNECTING }

    private final OkHttpClient http = new OkHttpClient.Builder()
        .pingInterval(15, TimeUnit.SECONDS)
        .connectTimeout(12, TimeUnit.SECONDS)
        .build();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Map<Integer, ArrayDeque<byte[]>> inbound = new HashMap<>();
    private int queuedBytes;

    private WebSocket socket;
    private int generation;
    private JSONObject hello;
    private Map<String, String> files;
    private long lostAt;
    private int retries;

    // What the room screen shows. Read under the lock.
    State state = State.IDLE;
    String server = "";
    String code = "";
    String title = "";
    int slot;
    int ip;
    int capacity;
    boolean hasPassword;
    boolean locked;
    boolean canLaunch;
    boolean allReady;
    boolean ready;
    String token = "";
    List<Member> members = Collections.emptyList();
    List<String> filesDiffer = Collections.emptyList();
    /** The last error code from the server or the connection ("" when none). */
    String error = "";
    /** The creator launched: the game should start now. */
    boolean launchPending;
    /** The game is running on this room: the engine's LAN sockets go through it. */
    boolean gameActive;
    long framesSent;
    long framesReceived;

    Runnable listener;

    private RoomSession() {
    }

    private void changed() {
        ui.post(() -> {
            Runnable l = listener;
            if (l != null) {
                l.run();
            }
        });
    }

    // ------------------------------------------------------------------ lobby

    /** Creates a room. game is "zh" or "generals"; compat names the engine build and sim rate. */
    synchronized void create(String serverUrl, JSONObject helloMsg, Map<String, String> fileList,
                             int cap, String roomTitle, String password, boolean isPublic) {
        JSONObject req = new JSONObject();
        try {
            req.put("type", "create").put("capacity", cap).put("title", roomTitle)
                .put("password", password).put("public", isPublic);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        open(serverUrl, helloMsg, fileList, req);
    }

    synchronized void join(String serverUrl, JSONObject helloMsg, Map<String, String> fileList,
                           String roomCode, String password) {
        JSONObject req = new JSONObject();
        try {
            req.put("type", "join").put("code", roomCode.trim().toUpperCase(java.util.Locale.ROOT))
                .put("password", password);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        open(serverUrl, helloMsg, fileList, req);
    }

    private void open(String serverUrl, JSONObject helloMsg, Map<String, String> fileList, JSONObject request) {
        closeLocked("");
        server = serverUrl;
        hello = helloMsg;
        files = fileList;
        state = State.CONNECTING;
        connect(request);
    }

    private void connect(JSONObject request) {
        final int epoch = ++generation;
        error = "";
        changed();
        Request req;
        try {
            req = new Request.Builder().url(server).build();
        } catch (IllegalArgumentException e) {
            state = State.IDLE;
            error = "bad_address";
            changed();
            return;
        }
        socket = http.newWebSocket(req, new Listener(epoch, request));
    }

    synchronized void setReady(boolean value) {
        if (state == State.IN_ROOM && !locked && socket != null) {
            socket.send("{\"type\":\"ready\",\"ready\":" + value + "}");
        }
    }

    synchronized void launch() {
        if (state == State.IN_ROOM && slot == 1 && canLaunch && socket != null) {
            socket.send("{\"type\":\"launch\"}");
        }
    }

    /** Leaves the room for good. The creator leaving before launch closes it for everybody. */
    synchronized void leave() {
        if (socket != null) {
            socket.send("{\"type\":\"leave\"}");
        }
        closeLocked("");
        changed();
    }

    private void closeLocked(String why) {
        generation++;
        if (socket != null) {
            socket.close(1000, "left");
            socket = null;
        }
        if (gameActive) {
            Log.i(TAG, "room session ended: sent " + framesSent + ", received " + framesReceived);
        }
        state = State.IDLE;
        error = why;
        code = "";
        title = "";
        slot = 0;
        ip = 0;
        capacity = 0;
        locked = false;
        canLaunch = false;
        allReady = false;
        ready = false;
        token = "";
        members = Collections.emptyList();
        filesDiffer = Collections.emptyList();
        launchPending = false;
        gameActive = false;
        inbound.clear();
        queuedBytes = 0;
        framesSent = 0;
        framesReceived = 0;
        retries = 0;
        filesSentRoom = "";
    }

    // ------------------------------------------------------------------ game

    /** The game is starting on this room: from now on the engine's LAN sockets are carried. */
    synchronized void beginGame() {
        launchPending = false;
        gameActive = true;
        inbound.clear();
        queuedBytes = 0;
        Log.i(TAG, "game on room " + code + " as 10.240.0." + slot);
    }

    /** The game process part is over (activity destroyed): the room has done its job. */
    synchronized void endGame() {
        if (gameActive) {
            leave();
        }
    }

    synchronized int engineIP() {
        return gameActive ? ip : 0;
    }

    synchronized int send(int srcPort, int dstIP, int dstPort, byte[] data) {
        if (!gameActive || socket == null || state != State.IN_ROOM || data.length > MAX_PAYLOAD) {
            return -1;
        }
        if (dstIP != BROADCAST && (dstIP <= IP_BASE || dstIP > IP_BASE + 8)) {
            return -1;
        }
        // A send queue this deep means the link cannot keep up with a lockstep game.
        if (socket.queueSize() > MAX_QUEUED_BYTES) {
            return -1;
        }
        ByteBuffer frame = ByteBuffer.allocate(HEADER + data.length)
            .putInt(MAGIC).putInt(ip).putInt(dstIP)
            .putShort((short) srcPort).putShort((short) dstPort).put(data);
        if (!socket.send(ByteString.of(frame.array()))) {
            return -1;
        }
        framesSent++;
        return data.length;
    }

    /** The next frame for a local port, header included, or null. */
    synchronized byte[] receive(int port) {
        ArrayDeque<byte[]> q = inbound.get(port);
        byte[] f = q == null ? null : q.poll();
        if (f != null) {
            queuedBytes -= f.length;
        }
        return f;
    }

    synchronized void reset(int port) {
        ArrayDeque<byte[]> q = inbound.remove(port);
        if (q != null) {
            for (Iterator<byte[]> it = q.iterator(); it.hasNext(); ) {
                queuedBytes -= it.next().length;
            }
        }
    }

    // ------------------------------------------------------------------ socket

    private final class Listener extends WebSocketListener {
        private final int epoch;
        private final JSONObject request;

        Listener(int epoch, JSONObject request) {
            this.epoch = epoch;
            this.request = request;
        }

        @Override
        public void onOpen(WebSocket ws, Response response) {
            synchronized (RoomSession.this) {
                if (epoch != generation) {
                    ws.close(1000, "stale");
                    return;
                }
                ws.send(hello.toString());
                ws.send(request.toString());
            }
        }

        @Override
        public void onMessage(WebSocket ws, String text) {
            synchronized (RoomSession.this) {
                if (epoch != generation) {
                    return;
                }
                try {
                    handle(new JSONObject(text));
                } catch (JSONException e) {
                    Log.w(TAG, "bad message from the server", e);
                    error = "bad_reply";
                }
                changed();
            }
        }

        @Override
        public void onMessage(WebSocket ws, ByteString bytes) {
            synchronized (RoomSession.this) {
                if (epoch != generation || !gameActive) {
                    return;
                }
                byte[] f = bytes.toByteArray();
                if (f.length <= HEADER || f.length > HEADER + MAX_PAYLOAD) {
                    return;
                }
                ByteBuffer b = ByteBuffer.wrap(f);
                int magic = b.getInt();
                int src = b.getInt();
                int dst = b.getInt();
                b.getShort();
                int dport = b.getShort() & 0xFFFF;
                if (magic != MAGIC || src == ip || (dst != BROADCAST && dst != ip)) {
                    return;
                }
                ArrayDeque<byte[]> q = inbound.get(dport);
                if (q == null) {
                    q = new ArrayDeque<>();
                    inbound.put(dport, q);
                }
                // A port nobody reads (the lobby's, during the match) must not grow forever.
                if (q.size() >= MAX_QUEUED_FRAMES || queuedBytes + f.length > MAX_QUEUED_BYTES) {
                    queuedBytes -= q.poll().length;
                }
                q.add(f);
                queuedBytes += f.length;
                framesReceived++;
            }
        }

        @Override
        public void onClosing(WebSocket ws, int code, String reason) {
            ws.close(code, null);
        }

        @Override
        public void onClosed(WebSocket ws, int closeCode, String reason) {
            lost(epoch, reason == null ? "" : reason);
        }

        @Override
        public void onFailure(WebSocket ws, Throwable t, Response response) {
            Log.w(TAG, "room connection failed", t);
            lost(epoch, "connection");
        }
    }

    private void handle(JSONObject m) throws JSONException {
        switch (m.optString("type")) {
            case "room": {
                int s = m.getInt("slot");
                int address = m.getInt("ip");
                if (s < 1 || s > 8 || address != IP_BASE + s) {
                    throw new JSONException("identity");
                }
                // The engine has bound its sockets to this address; it cannot change mid-game.
                if (gameActive && address != ip) {
                    throw new JSONException("identity changed");
                }
                state = State.IN_ROOM;
                retries = 0;
                slot = s;
                ip = address;
                code = m.getString("code");
                title = m.optString("title", "");
                capacity = m.getInt("capacity");
                hasPassword = m.optBoolean("password");
                locked = m.optBoolean("locked");
                canLaunch = m.optBoolean("canLaunch");
                allReady = m.optBoolean("allReady");
                token = m.optString("token", token);
                JSONArray list = m.getJSONArray("members");
                List<Member> ms = new ArrayList<>();
                for (int i = 0; i < list.length(); i++) {
                    JSONObject o = list.getJSONObject(i);
                    Member mem = new Member(o.getInt("slot"), o.optString("name", "?"),
                        o.optBoolean("ready"), o.optBoolean("connected", true), o.optString("files", "pending"));
                    ms.add(mem);
                    if (mem.slot == slot) {
                        ready = mem.ready;
                    }
                }
                members = Collections.unmodifiableList(ms);
                // The room is new or resumed: say which files decide the match.
                if (files != null && !m.optBoolean("locked") && !filesSentFor(code)) {
                    sendFiles();
                }
                break;
            }
            case "files": {
                List<String> d = new ArrayList<>();
                for (String key : new String[] { "differs", "missing", "extra" }) {
                    JSONArray a = m.optJSONArray(key);
                    for (int i = 0; a != null && i < a.length(); i++) {
                        d.add(a.getString(i));
                    }
                }
                filesDiffer = Collections.unmodifiableList(d);
                break;
            }
            case "launch":
                launchPending = true;
                break;
            case "error": {
                String c = m.optString("code", "error");
                error = c;
                // Refused at the door: there is no room to stay in.
                if (state != State.IN_ROOM || "closed".equals(c)) {
                    closeLocked(c);
                }
                break;
            }
            default:
                break;
        }
    }

    private String filesSentRoom = "";

    private boolean filesSentFor(String roomCode) {
        return roomCode.equals(filesSentRoom);
    }

    private void sendFiles() {
        JSONObject list = new JSONObject();
        try {
            for (Map.Entry<String, String> e : files.entrySet()) {
                list.put(e.getKey(), e.getValue());
            }
            socket.send(new JSONObject().put("type", "files").put("list", list).toString());
            filesSentRoom = code;
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    // A dropped connection keeps the slot on the server for a minute: come back with the token,
    // which matters most mid-match, when a phone hops from Wi-Fi to mobile data.
    private void lost(int epoch, String reason) {
        synchronized (this) {
            if (epoch != generation) {
                return;
            }
            socket = null;
            if (state == State.IDLE) {
                return;
            }
            if (state == State.CONNECTING || token.isEmpty()) {
                closeLocked(error.isEmpty() ? (reason.isEmpty() ? "connection" : reason) : error);
                changed();
                return;
            }
            long now = System.currentTimeMillis();
            if (state != State.RECONNECTING) {
                lostAt = now;
                state = State.RECONNECTING;
            }
            if (now - lostAt > RESUME_WINDOW_MS) {
                closeLocked("connection");
                changed();
                return;
            }
            long delay = Math.min(8000, 500L << Math.min(retries, 4));
            retries++;
            final int scheduled = generation;
            changed();
            ui.postDelayed(() -> resume(scheduled), delay);
        }
    }

    private synchronized void resume(int scheduled) {
        if (scheduled != generation || state != State.RECONNECTING) {
            return;
        }
        JSONObject req = new JSONObject();
        try {
            req.put("type", "resume").put("token", token);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        filesSentRoom = "";
        connect(req);
    }
}
