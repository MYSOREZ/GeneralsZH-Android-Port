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

// GeneralsX @feature Android port 04/10/2026 Rooms: pick a server, create or join a room, get
// ready, and launch. When the creator launches, every member's screen returns RESULT_OK and
// SetupActivity starts the game, whose LAN screen then runs over the room (RoomSession).

package com.generalsx.zerohour;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.android.material.materialswitch.MaterialSwitch;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.UUID;

public class RoomActivity extends Activity {
    private static final String PREFS = "rooms";
    private static final String PREF_SERVER = "server";
    private static final String PREF_NAME = "name";
    private static final String PREF_DEVICE = "device";
    private static final int[] CAPACITIES = { 2, 3, 4, 5, 6, 7, 8 };

    private final RoomSession room = RoomSession.INSTANCE;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private LinearLayout page;
    private RoomSession.State shown;
    private boolean checkingFiles;
    private Thread filesThread;
    private String filesStatus = "";

    private EditText serverInput;
    private EditText nameInput;
    private TextView serverStatus;
    private int capacityIndex;
    private EditText createPassword;
    private MaterialSwitch publicSwitch;
    private EditText codeInput;
    private EditText joinPassword;

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.rooms_title);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(UiKit.color(this, R.color.gzh_background));
        UiKit.appBar(root, getString(R.string.setup_window_title), getString(R.string.rooms_title), 0, null, null);
        LinearLayout host = new LinearLayout(this);
        root.addView(host, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        page = UiKit.scrollingPage(host);
        setContentView(root);
        InsetUtil.applySafeInsets(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        room.listener = this::render;
        shown = null;
        render();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (room.listener != null) {
            room.listener = null;
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (filesThread != null) {
            filesThread.interrupt();
        }
    }

    // ------------------------------------------------------------------ render

    private void render() {
        RoomSession.State state;
        boolean launch;
        synchronized (room) {
            state = room.state;
            launch = room.launchPending && state == RoomSession.State.IN_ROOM;
        }
        if (launch) {
            room.beginGame();
            setResult(RESULT_OK);
            finish();
            return;
        }
        // The forms are rebuilt only when the screen changes, so typing is never interrupted.
        if (state == RoomSession.State.IDLE && !checkingFiles) {
            if (shown != RoomSession.State.IDLE) {
                shown = RoomSession.State.IDLE;
                buildLobby();
            }
            updateError();
            return;
        }
        shown = state;
        buildRoom();
    }

    private TextView errorView;

    private void updateError() {
        if (errorView == null) {
            return;
        }
        String e;
        synchronized (room) {
            e = room.error;
        }
        errorView.setText(e.isEmpty() ? "" : errorText(e));
        errorView.setVisibility(e.isEmpty() ? TextView.GONE : TextView.VISIBLE);
    }

    private void buildLobby() {
        page.removeAllViews();
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        LinearLayout server = UiKit.card(page);
        UiKit.sectionHeader(server, R.drawable.ic_gzh_globe, getString(R.string.rooms_card_server), false);
        UiKit.supporting(server, getString(R.string.rooms_server_desc));
        serverInput = field(server, getString(R.string.rooms_server_hint), prefs.getString(PREF_SERVER, ""),
            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        nameInput = field(server, getString(R.string.rooms_name_hint), prefs.getString(PREF_NAME, ""),
            InputType.TYPE_CLASS_TEXT);
        serverStatus = UiKit.supporting(server, "");
        UiKit.button(server, UiKit.BTN_TONAL, R.drawable.ic_gzh_refresh, getString(R.string.rooms_server_check),
            this::checkServer);

        // GeneralsX @feature Android port 04/10/2026 The servers players run, closest first
        // (RoomServers), and installing one's own on a VPS (RoomInstallActivity).
        LinearLayout list = UiKit.card(page);
        UiKit.sectionHeader(list, R.drawable.ic_gzh_globe, getString(R.string.rooms_card_list), false);
        serverList = new LinearLayout(this);
        serverList.setOrientation(LinearLayout.VERTICAL);
        list.addView(serverList);
        UiKit.button(list, UiKit.BTN_TONAL, R.drawable.ic_gzh_refresh, getString(R.string.rooms_list_refresh),
            this::loadServers);
        UiKit.button(list, UiKit.BTN_OUTLINE, R.drawable.ic_gzh_download, getString(R.string.rooms_install_open),
            () -> startActivityForResult(new Intent(this, RoomInstallActivity.class), REQUEST_INSTALL));
        loadServers();

        errorView = UiKit.body(page, "");
        errorView.setTextColor(UiKit.color(this, R.color.gzh_tertiary));

        LinearLayout create = UiKit.card(page);
        UiKit.sectionHeader(create, R.drawable.ic_gzh_play, getString(R.string.rooms_card_create), false);
        UiKit.caption(create, getString(R.string.rooms_capacity));
        CharSequence[] labels = new CharSequence[CAPACITIES.length];
        for (int i = 0; i < CAPACITIES.length; i++) {
            labels[i] = String.valueOf(CAPACITIES[i]);
        }
        UiKit.segmented(create, labels, capacityIndex, i -> capacityIndex = i);
        createPassword = field(create, getString(R.string.rooms_password_hint), "", InputType.TYPE_CLASS_TEXT);
        publicSwitch = UiKit.switchRow(create, getString(R.string.rooms_public), getString(R.string.rooms_public_desc));
        UiKit.button(create, UiKit.BTN_PRIMARY, R.drawable.ic_gzh_play, getString(R.string.rooms_button_create),
            () -> start(true));

        LinearLayout join = UiKit.card(page);
        UiKit.sectionHeader(join, R.drawable.ic_gzh_account, getString(R.string.rooms_card_join), false);
        codeInput = field(join, getString(R.string.rooms_code_hint), "",
            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS);
        joinPassword = field(join, getString(R.string.rooms_password_hint), "", InputType.TYPE_CLASS_TEXT);
        UiKit.button(join, UiKit.BTN_TONAL, R.drawable.ic_gzh_account, getString(R.string.rooms_button_join),
            () -> start(false));

        UiKit.helpText(page, getString(R.string.rooms_how));
    }

    private static final int REQUEST_INSTALL = 1;
    private LinearLayout serverList;
    private int serverListGeneration;

    private void loadServers() {
        if (serverList == null) {
            return;
        }
        final int generation = ++serverListGeneration;
        serverList.removeAllViews();
        UiKit.supporting(serverList, getString(R.string.rooms_list_loading));
        new Thread(() -> {
            final java.util.List<RoomServers.Server> servers = RoomServers.load(getApplicationContext());
            handler.post(() -> {
                if (generation != serverListGeneration || serverList == null || isFinishing()) {
                    return;
                }
                serverList.removeAllViews();
                if (servers.isEmpty()) {
                    UiKit.supporting(serverList, getString(R.string.rooms_list_empty));
                    return;
                }
                for (final RoomServers.Server srv : servers) {
                    String title = srv.name.isEmpty() ? srv.url : srv.name;
                    if (srv.mine) {
                        title = getString(R.string.rooms_list_mine, title);
                    }
                    String detail = srv.alive
                        ? getString(R.string.rooms_list_detail, srv.pingMs, srv.rooms, srv.players)
                        : getString(R.string.rooms_list_down);
                    UiKit.listRow(serverList, R.drawable.ic_gzh_globe, title, detail + "\n" + srv.url, () -> {
                        serverInput.setText(srv.url);
                        serverStatus.setText(srv.alive ? getString(R.string.rooms_server_ok, title(srv), srv.pingMs, srv.rooms)
                            : getString(R.string.rooms_server_down));
                    });
                }
            });
        }, "GXRooms-list").start();
    }

    private static String title(RoomServers.Server s) {
        return s.name.isEmpty() ? s.url : s.name;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_INSTALL && resultCode == RESULT_OK && data != null) {
            String url = data.getStringExtra(RoomInstallActivity.EXTRA_URL);
            String removedHost = data.getStringExtra(RoomInstallActivity.EXTRA_REMOVED_HOST);
            android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
            if (url != null) {
                prefs.edit().putString(PREF_SERVER, url).apply();
            } else if (removedHost != null) {
                // The removed server is no longer worth keeping selected.
                String selected = prefs.getString(PREF_SERVER, "");
                String h = removedHost.trim().toLowerCase(java.util.Locale.ROOT);
                if (selected.contains(h) || selected.contains(h.replace('.', '-') + ".sslip.io")) {
                    prefs.edit().putString(PREF_SERVER, "").apply();
                }
            }
            if (url != null || removedHost != null) {
                shown = null;
                render();
            }
        }
    }

    private EditText field(LinearLayout parent, String hint, String value, int type) {
        UiKit.caption(parent, hint);
        EditText e = new EditText(this);
        e.setInputType(type);
        e.setSingleLine(true);
        e.setText(value);
        parent.addView(e);
        return e;
    }

    private void buildRoom() {
        page.removeAllViews();
        errorView = null;
        serverList = null;
        LinearLayout card = UiKit.card(page);
        synchronized (room) {
            if (checkingFiles) {
                UiKit.sectionHeader(card, R.drawable.ic_gzh_refresh, getString(R.string.rooms_title), false);
                UiKit.body(card, getString(R.string.rooms_checking_files, filesStatus));
                return;
            }
            if (room.state == RoomSession.State.CONNECTING) {
                UiKit.sectionHeader(card, R.drawable.ic_gzh_refresh, getString(R.string.rooms_title), false);
                UiKit.body(card, getString(R.string.rooms_connecting));
                UiKit.button(card, UiKit.BTN_DANGER, R.drawable.ic_gzh_broom, getString(R.string.rooms_button_leave),
                    room::leave);
                return;
            }
            UiKit.sectionHeader(card, R.drawable.ic_gzh_account, getString(R.string.rooms_card_room, room.code), false);
            TextView codeView = UiKit.body(card, room.code);
            codeView.setTextSize(32);
            codeView.setTextIsSelectable(true);
            if (room.state == RoomSession.State.RECONNECTING) {
                UiKit.supporting(card, getString(R.string.rooms_reconnecting));
            }
            StringBuilder list = new StringBuilder();
            for (RoomSession.Member m : room.members) {
                list.append(m.slot).append(". ").append(m.name);
                if (m.slot == room.slot) {
                    list.append(getString(R.string.rooms_member_you));
                }
                if (m.slot == 1) {
                    list.append(getString(R.string.rooms_member_host));
                }
                list.append(" · ").append(getString(!m.connected ? R.string.rooms_member_offline
                    : m.ready ? R.string.rooms_member_ready : R.string.rooms_member_not_ready));
                if ("differs".equals(m.files)) {
                    list.append(" · ").append(getString(R.string.rooms_member_files_differ));
                }
                list.append('\n');
            }
            UiKit.body(card, list.toString().trim());
            UiKit.supporting(card, room.canLaunch
                ? getString(R.string.rooms_can_launch)
                : getString(R.string.rooms_waiting, room.members.size(), room.capacity));
            if (!room.filesDiffer.isEmpty()) {
                TextView diff = UiKit.body(card, getString(R.string.rooms_files_differ,
                    android.text.TextUtils.join(", ", room.filesDiffer.subList(0, Math.min(8, room.filesDiffer.size())))));
                diff.setTextColor(UiKit.color(this, R.color.gzh_tertiary));
            }
            if (!room.error.isEmpty()) {
                TextView err = UiKit.body(card, errorText(room.error));
                err.setTextColor(UiKit.color(this, R.color.gzh_tertiary));
            }
            final boolean ready = room.ready;
            UiKit.button(card, UiKit.BTN_TONAL, R.drawable.ic_gzh_check,
                getString(ready ? R.string.rooms_button_not_ready : R.string.rooms_button_ready),
                () -> room.setReady(!ready));
            if (room.slot == 1) {
                UiKit.button(card, UiKit.BTN_PRIMARY, R.drawable.ic_gzh_play,
                    getString(R.string.rooms_button_launch), room::launch).setEnabled(room.canLaunch);
            }
            final String shareText = getString(R.string.rooms_share_text, room.code, room.server);
            UiKit.button(card, UiKit.BTN_TONAL, R.drawable.ic_gzh_share, getString(R.string.rooms_share),
                () -> share(shareText));
            UiKit.button(card, UiKit.BTN_DANGER, R.drawable.ic_gzh_broom, getString(R.string.rooms_button_leave),
                room::leave);
        }
        UiKit.helpText(page, getString(R.string.rooms_launch_hint));
    }

    private void share(String text) {
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_TEXT, text);
        startActivity(Intent.createChooser(send, getString(R.string.rooms_share)));
    }

    private String errorText(String code) {
        switch (code) {
            case "connection": return getString(R.string.rooms_error_connection);
            case "bad_address": return getString(R.string.rooms_error_address);
            case "no_room": return getString(R.string.rooms_error_no_room);
            case "full": return getString(R.string.rooms_error_full);
            case "password": return getString(R.string.rooms_error_password);
            case "started": return getString(R.string.rooms_error_started);
            case "game_mismatch":
            case "compat_mismatch": return getString(R.string.rooms_error_version);
            case "not_ready": return getString(R.string.rooms_error_not_ready);
            case "server_full": return getString(R.string.rooms_error_server_full);
            case "closed": return getString(R.string.rooms_error_closed);
            case "resume_expired": return getString(R.string.rooms_error_expired);
            case "files": return getString(R.string.rooms_error_files);
            default: return getString(R.string.rooms_error_other, code);
        }
    }

    // ------------------------------------------------------------------ actions

    private String serverUrl() {
        String s = serverInput.getText().toString().trim();
        if (!s.isEmpty() && !s.contains("://")) {
            s = "wss://" + s;
        }
        if (!s.isEmpty() && !s.endsWith("/ws")) {
            s = s.replaceAll("/+$", "") + "/ws";
        }
        return s;
    }

    private void checkServer() {
        final String url = serverUrl();
        if (url.isEmpty()) {
            return;
        }
        serverStatus.setText(R.string.rooms_connecting);
        new Thread(() -> {
            String result;
            long t0 = System.nanoTime();
            try {
                String health = url.replaceFirst("^ws", "http").replaceAll("/ws$", "/health");
                HttpURLConnection c = (HttpURLConnection) new URL(health).openConnection();
                c.setConnectTimeout(8000);
                c.setReadTimeout(8000);
                java.io.ByteArrayOutputStream body = new java.io.ByteArrayOutputStream();
                try (InputStream in = c.getInputStream()) {
                    byte[] buf = new byte[4096];
                    int n;
                    while ((n = in.read(buf)) > 0 && body.size() < 65536) {
                        body.write(buf, 0, n);
                    }
                }
                long ms = (System.nanoTime() - t0) / 1_000_000;
                JSONObject h = new JSONObject(body.toString(StandardCharsets.UTF_8.name()));
                result = h.optInt("protocol") == RoomSession.PROTOCOL
                    ? getString(R.string.rooms_server_ok, h.optString("name"), ms, h.optInt("rooms"))
                    : getString(R.string.rooms_error_version);
            } catch (IOException | JSONException | RuntimeException e) {
                result = getString(R.string.rooms_server_down);
            }
            final String text = result;
            handler.post(() -> {
                if (serverStatus != null) {
                    serverStatus.setText(text);
                }
            });
        }, "GXRooms-health").start();
    }

    private void start(final boolean create) {
        final String url = serverUrl();
        final String name = nameInput.getText().toString().trim();
        final String code = codeInput.getText().toString().trim();
        if (url.isEmpty() || (!create && code.isEmpty())) {
            synchronized (room) {
                room.error = url.isEmpty() ? "bad_address" : "no_room";
            }
            updateError();
            return;
        }
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String device = prefs.getString(PREF_DEVICE, "");
        if (!device.matches("[0-9a-f]{32}")) {
            device = UUID.randomUUID().toString().replace("-", "");
        }
        prefs.edit().putString(PREF_SERVER, serverInput.getText().toString().trim())
            .putString(PREF_NAME, name).putString(PREF_DEVICE, device).apply();

        final String game = SetupActivity.getSelectedGame(this);
        final JSONObject hello = new JSONObject();
        try {
            hello.put("type", "hello").put("protocol", RoomSession.PROTOCOL).put("game", game)
                .put("compat", game + "-" + UpdateManager.activeEngineSeq(this) + "-" + SetupActivity.getSimHz(this) + "hz")
                .put("name", name.isEmpty() ? android.os.Build.MODEL : name)
                .put("device", device);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        final int cap = CAPACITIES[capacityIndex];
        final String password = (create ? createPassword : joinPassword).getText().toString();
        final boolean isPublic = publicSwitch.isChecked();

        checkingFiles = true;
        filesStatus = "";
        shown = null;
        render();
        filesThread = new Thread(() -> {
            Map<String, String> files = null;
            try {
                files = RoomFiles.compute(this, game, f -> handler.post(() -> {
                    filesStatus = f;
                    if (checkingFiles) {
                        render();
                    }
                }));
            } catch (IOException e) {
                files = null;
            }
            final Map<String, String> list = files;
            handler.post(() -> {
                checkingFiles = false;
                if (isFinishing()) {
                    return;
                }
                if (list == null) {
                    synchronized (room) {
                        room.error = "files";
                    }
                    shown = null;
                    render();
                    return;
                }
                if (create) {
                    room.create(url, hello, list, cap, getString(R.string.rooms_default_title, hello.optString("name")),
                        password, isPublic);
                } else {
                    room.join(url, hello, list, code, password);
                }
            });
        }, "GXRooms-files");
        filesThread.start();
    }
}
