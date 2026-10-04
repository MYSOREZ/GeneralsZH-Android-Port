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

// GeneralsX @feature Android port 04/10/2026 Rooms: install a rooms server on the player's VPS.
//
// One button: connect over SSH with the address and login the player types, run the installer of
// MYSOREZ/Generals-Servers (Docker, the relay, Caddy for HTTPS, start on boot), follow its
// "GXROOMS:" progress lines, and come back with the server's wss:// address, which is kept as one of
// the player's servers. Unless "private" is chosen the server then lists itself publicly.
//
// The password or key is used for this one connection and never stored. The server's host key is
// remembered per host after the first install; if it is different the next time, the install
// stops instead of sending the password to whoever answered.

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

import com.google.android.material.button.MaterialButton;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.jcraft.jsch.ChannelExec;
import com.jcraft.jsch.JSch;
import com.jcraft.jsch.JSchException;
import com.jcraft.jsch.Session;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Properties;

public class RoomInstallActivity extends Activity {
    static final String EXTRA_URL = "url";
    static final String EXTRA_REMOVED_HOST = "removed_host";
    private static final String REMOVED = "removed";
    private static final String PREFS = "rooms";
    private static final String HOSTKEY_PREFIX = "hostkey:";
    private static final String DEFAULT_INSTALLER =
        "https://raw.githubusercontent.com/MYSOREZ/Generals-Servers/main/install.sh";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private EditText hostInput;
    private EditText portInput;
    private EditText userInput;
    private EditText passwordInput;
    private EditText keyInput;
    private EditText nameInput;
    private MaterialSwitch listedSwitch;
    private MaterialButton installButton;
    private MaterialButton removeButton;
    private TextView stepView;
    private TextView logView;
    private final StringBuilder log = new StringBuilder();
    private volatile boolean running;
    private volatile Session session;

    @Override
    protected void attachBaseContext(android.content.Context newBase) {
        super.attachBaseContext(LocaleHelper.wrap(newBase));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.rooms_install_title);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(UiKit.color(this, R.color.gzh_background));
        UiKit.appBar(root, getString(R.string.rooms_title), getString(R.string.rooms_install_title), 0, null, null);
        LinearLayout host = new LinearLayout(this);
        root.addView(host, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        LinearLayout page = UiKit.scrollingPage(host);

        LinearLayout vps = UiKit.card(page);
        UiKit.sectionHeader(vps, R.drawable.ic_gzh_terminal, getString(R.string.rooms_install_card_vps), false);
        UiKit.supporting(vps, getString(R.string.rooms_install_desc));
        hostInput = field(vps, R.string.rooms_install_host, "", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        portInput = field(vps, R.string.rooms_install_port, "22", InputType.TYPE_CLASS_NUMBER);
        userInput = field(vps, R.string.rooms_install_user, "root", InputType.TYPE_CLASS_TEXT);
        passwordInput = field(vps, R.string.rooms_install_password, "",
            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        keyInput = field(vps, R.string.rooms_install_key, "",
            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        keyInput.setSingleLine(false);
        keyInput.setMaxLines(4);

        LinearLayout server = UiKit.card(page);
        UiKit.sectionHeader(server, R.drawable.ic_gzh_globe, getString(R.string.rooms_install_card_server), false);
        nameInput = field(server, R.string.rooms_install_name, "", InputType.TYPE_CLASS_TEXT);
        listedSwitch = UiKit.switchRow(server, getString(R.string.rooms_install_listed),
            getString(R.string.rooms_install_listed_desc));
        listedSwitch.setChecked(true);
        UiKit.supporting(server, getString(R.string.rooms_install_ports));
        installButton = UiKit.button(server, UiKit.BTN_PRIMARY, R.drawable.ic_gzh_download,
            getString(R.string.rooms_install_button), this::install);
        removeButton = UiKit.button(server, UiKit.BTN_DANGER, R.drawable.ic_gzh_trash,
            getString(R.string.rooms_remove_button), this::confirmRemove);

        LinearLayout progress = UiKit.card(page);
        UiKit.sectionHeader(progress, R.drawable.ic_gzh_refresh, getString(R.string.rooms_install_card_progress), false);
        stepView = UiKit.body(progress, getString(R.string.rooms_install_idle));
        logView = UiKit.caption(progress, "");
        logView.setTypeface(android.graphics.Typeface.MONOSPACE);
        logView.setTextIsSelectable(true);

        setContentView(root);
        InsetUtil.applySafeInsets(root);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Session s = session;
        if (s != null) {
            s.disconnect();
        }
    }

    private EditText field(LinearLayout parent, int hint, String value, int type) {
        UiKit.caption(parent, getString(hint));
        EditText e = new EditText(this);
        e.setInputType(type);
        e.setSingleLine(true);
        e.setText(value);
        parent.addView(e);
        return e;
    }

    // ------------------------------------------------------------------ install

    private static String shellQuote(String s) {
        return "'" + s.replace("'", "'\\''") + "'";
    }

    private void install() {
        start(false);
    }

    // GeneralsX @feature Android port 04/10/2026 The same connection, running the installer with
    // --uninstall: the server, its containers and /opt/gx-rooms go; it leaves the public list by
    // itself once it stops announcing.
    private void confirmRemove() {
        if (running) {
            return;
        }
        final String host = hostInput.getText().toString().trim();
        if (host.isEmpty()) {
            stepView.setText(R.string.rooms_install_missing);
            return;
        }
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.rooms_remove_button)
            .setMessage(getString(R.string.rooms_remove_confirm, host))
            .setPositiveButton(R.string.rooms_remove_yes, (d, w) -> start(true))
            .setNegativeButton(android.R.string.cancel, null)
            .show();
    }

    private void start(final boolean remove) {
        if (running) {
            return;
        }
        final String host = hostInput.getText().toString().trim();
        final String user = userInput.getText().toString().trim();
        final String password = passwordInput.getText().toString();
        final String key = keyInput.getText().toString().trim();
        final String name = nameInput.getText().toString().trim();
        int port;
        try {
            port = Integer.parseInt(portInput.getText().toString().trim());
        } catch (NumberFormatException e) {
            port = 22;
        }
        if (host.isEmpty() || user.isEmpty() || (password.isEmpty() && key.isEmpty())) {
            stepView.setText(R.string.rooms_install_missing);
            return;
        }
        final int sshPort = port;
        final boolean listed = listedSwitch.isChecked();
        running = true;
        installButton.setEnabled(false);
        removeButton.setEnabled(false);
        log.setLength(0);
        logView.setText("");
        step(getString(R.string.rooms_install_connecting, host));
        // The fields hold a password: clear it now that it has been read.
        passwordInput.setText("");
        keyInput.setText("");

        new Thread(() -> {
            String url = null;
            String failure = null;
            try {
                url = runInstall(host, sshPort, user, password, key, name.isEmpty() ? "Rooms server" : name, listed, remove);
                if (url == null) {
                    failure = getString(R.string.rooms_install_failed_unknown);
                }
            } catch (InstallError e) {
                failure = e.getMessage();
            } catch (JSchException e) {
                String m = String.valueOf(e.getMessage());
                failure = m.contains("Auth fail") || m.contains("auth")
                    ? getString(R.string.rooms_install_error_auth)
                    : getString(R.string.rooms_install_error_connect, m);
            } catch (Exception e) {
                failure = getString(R.string.rooms_install_error_connect, String.valueOf(e.getMessage()));
            }
            final String done = url;
            final String error = failure;
            handler.post(() -> {
                if (remove) {
                    finishRemove(host, done != null, error);
                } else {
                    finishInstall(done, error);
                }
            });
        }, "GXRooms-install").start();
    }

    private static final class InstallError extends Exception {
        InstallError(String message) {
            super(message);
        }
    }

    /** Runs the installer; returns the server's address, or REMOVED after an uninstall. */
    private String runInstall(String host, int port, String user, String password, String key,
                              String name, boolean listed, boolean remove) throws Exception {
        JSch jsch = new JSch();
        if (!key.isEmpty()) {
            jsch.addIdentity("key", key.getBytes(StandardCharsets.UTF_8), null,
                password.isEmpty() ? null : password.getBytes(StandardCharsets.UTF_8));
        }
        Session s = jsch.getSession(user, host, port);
        session = s;
        if (key.isEmpty()) {
            s.setPassword(password);
        }
        Properties config = new Properties();
        // The key is checked below against the one remembered for this host.
        config.put("StrictHostKeyChecking", "no");
        config.put("PreferredAuthentications", key.isEmpty() ? "password,keyboard-interactive" : "publickey");
        s.setConfig(config);
        s.setServerAliveInterval(15000);
        s.connect(20000);

        String fingerprint = s.getHostKey().getFingerPrint(jsch);
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String known = prefs.getString(HOSTKEY_PREFIX + host + ":" + port, null);
        if (known != null && !known.equals(fingerprint)) {
            s.disconnect();
            throw new InstallError(getString(R.string.rooms_install_error_hostkey, fingerprint));
        }
        prefs.edit().putString(HOSTKEY_PREFIX + host + ":" + port, fingerprint).apply();
        line(getString(R.string.rooms_install_hostkey, fingerprint));

        String installer = UpdateManager.remoteConfig(this, "rooms_installer", DEFAULT_INSTALLER);
        String script = "set -e; f=/tmp/gx-rooms-install.sh; "
            + "if command -v curl >/dev/null 2>&1; then curl -fsSL " + shellQuote(installer) + " -o $f; "
            + "else wget -qO $f " + shellQuote(installer) + "; fi; "
            + (remove ? "sh $f --uninstall" : "sh $f --name " + shellQuote(name) + (listed ? " --public" : " --private"));
        boolean root = "root".equals(user);
        String command = root
            ? "sh -c " + shellQuote(script) + " 2>&1"
            : "sudo -S -p '' sh -c " + shellQuote(script) + " 2>&1";

        ChannelExec ch = (ChannelExec) s.openChannel("exec");
        ch.setCommand(command);
        BufferedReader out = new BufferedReader(new InputStreamReader(ch.getInputStream(), StandardCharsets.UTF_8));
        OutputStream in = ch.getOutputStream();
        ch.connect(20000);
        if (!root) {
            in.write((password + "\n").getBytes(StandardCharsets.UTF_8));
            in.flush();
        }
        in.close();

        String url = null;
        String errorCode = null;
        String l;
        while ((l = out.readLine()) != null) {
            if (l.startsWith("GXROOMS:STEP ")) {
                final String code = l.substring(13).trim();
                handler.post(() -> step(stepText(code)));
            } else if (l.startsWith("GXROOMS:URL ")) {
                url = l.substring(12).trim();
            } else if (l.startsWith("GXROOMS:REMOVED")) {
                url = REMOVED;
            } else if (l.startsWith("GXROOMS:ERROR ")) {
                errorCode = l.substring(14).trim();
            } else {
                line(l);
            }
        }
        ch.disconnect();
        s.disconnect();
        session = null;
        if (errorCode != null) {
            throw new InstallError(errorText(errorCode));
        }
        if (url == null && ch.getExitStatus() != 0) {
            throw new InstallError(getString(R.string.rooms_install_failed_unknown));
        }
        return url;
    }

    private void line(final String l) {
        handler.post(() -> {
            log.append(l).append('\n');
            // Only the tail: Docker's install prints hundreds of lines.
            if (log.length() > 6000) {
                log.delete(0, log.length() - 6000);
            }
            logView.setText(log);
        });
    }

    private void step(String text) {
        stepView.setText(text);
    }

    private String stepText(String code) {
        switch (code) {
            case "packages": return getString(R.string.rooms_install_step_packages);
            case "docker": return getString(R.string.rooms_install_step_docker);
            case "download": return getString(R.string.rooms_install_step_download);
            case "config": return getString(R.string.rooms_install_step_config);
            case "start": return getString(R.string.rooms_install_step_start);
            case "check": return getString(R.string.rooms_install_step_check);
            case "uninstall": return getString(R.string.rooms_remove_step);
            default: return code;
        }
    }

    private String errorText(String code) {
        switch (code) {
            case "not_root": return getString(R.string.rooms_install_error_root);
            case "docker_install":
            case "docker_compose": return getString(R.string.rooms_install_error_docker);
            case "download": return getString(R.string.rooms_install_error_download);
            case "no_ip": return getString(R.string.rooms_install_error_ip);
            case "tls": return getString(R.string.rooms_install_error_ports);
            default: return getString(R.string.rooms_install_error_other, code);
        }
    }

    private void finishRemove(String host, boolean removed, String error) {
        running = false;
        installButton.setEnabled(true);
        removeButton.setEnabled(true);
        if (!removed) {
            step(error == null ? getString(R.string.rooms_install_failed_unknown) : error);
            return;
        }
        RoomServers.forgetHost(this, host);
        step(getString(R.string.rooms_remove_done, host));
        setResult(RESULT_OK, new Intent().putExtra(EXTRA_REMOVED_HOST, host));
    }

    private void finishInstall(String url, String error) {
        running = false;
        installButton.setEnabled(true);
        removeButton.setEnabled(true);
        if (url == null) {
            step(error == null ? getString(R.string.rooms_install_failed_unknown) : error);
            return;
        }
        RoomServers.addMine(this, url);
        step(getString(R.string.rooms_install_done, url));
        setResult(RESULT_OK, new Intent().putExtra(EXTRA_URL, url));
    }
}
