# PLAN-023: Rooms — LAN over community relays

Goal: friends play **Network (LAN)** over the internet through a relay, in both games, without
NAT traversal (issue #37 is the case it ends) and without GeneralsOnline — which also gives the
original Generals an internet mode. Anyone can run a relay; the list is kept by the community.

Reference: the Mobsik build of this port (1.4.0-mobsik.23) — studied 04/10/2026. Its relay is
tied to its website (tickets) and to its own game download (`compat` holds that archive's
SHA-256), so this port cannot use it without impersonating that client; the frame layout and
virtual addresses are taken from it, the rest is our own. Protocol: `docs/port/ROOMS_PROTOCOL.md`.

## Parts

1. **Relay** — `tools/rooms-relay/` (Go, one static binary, Docker + Caddy, `install.sh`).
   Done 04/10/2026, with tests (`go test -race`). How-to: `docs/HOWTO/ROOMS_SERVER.md`.
2. **Server list** — issues with the label `rooms-server` (`.github/ISSUE_TEMPLATE/rooms-server.yml`).
   The launcher reads them through the GitHub API (no Actions, no token), asks each `/health`,
   sorts by ping. Needs the label created in the repository once.
3. **Launcher**
   - Rooms screen: server picker (list + custom address), public rooms of a server, create
     (capacity, title, password, public), join by code / link / QR, ready, launch.
   - Files: SHA-256 of the `.big` archives, `Data/INI` and maps, cached by size+mtime.
   - Install on my VPS: host, port, user, password or key → SSH (JSch fork `com.github.mwiede:jsch`,
     no other dependencies), host key shown on first connect, runs `install.sh`, follows its
     `GXROOMS:` lines, then offers the pre-filled issue form. Credentials are never stored.
   - Invite links `gxrooms://join?server=…&code=…` and QR.
   - Strings in all 13 locales.
4. **Engine** — `Core/GameEngine/Source/GameNetwork/udp.cpp`: when a room session is active, the
   LAN socket sends to and receives from the room (JNI to the launcher's WebSocket, as the Mobsik
   build does) instead of the network; the local address becomes `10.240.0.<slot>`. Both games
   share that file. Logic CRC revision pinned for the match like LAN.

## Order

Relay (done) → engine bridge + minimal room screen (test build: two phones in one room) →
server list, invites, QR → in-app VPS install → docs (ANDROID_PORT.md, README).
