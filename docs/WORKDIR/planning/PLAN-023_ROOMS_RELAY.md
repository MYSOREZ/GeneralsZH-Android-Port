# PLAN-023: Rooms — LAN over community relays

> **Status: paused (04/10/2026), code reverted from this repository.** The owner's call, after
> weighing it: ordinary players need GeneralsOnline working on mobile networks first, and that
> turned out to be a timing problem in reaching GeneralsOnline's own TURN relay (#37), fixed by
> the STUN/TURN order in the signed config -- not a missing relay. Rooms would only help once
> there is a live server, and asked a lot of whoever runs it. What exists, to pick up from:
> the relay, installer, protocol and self-maintaining server list in
> https://github.com/MYSOREZ/Generals-Servers (unchanged), and in this repository's history the
> engine bridge, launcher screens, SSH install/remove (commits 17506c268..3799f17d2; reverted
> together, so `git revert` of the revert brings them back). If it returns: one server the owner
> runs as the default, SSH install under "Advanced", and the game opening its LAN screen itself.

Goal: friends play **Network (LAN)** over the internet through a relay, in both games, without
NAT traversal (issue #37 is the case it ends) and without GeneralsOnline — which also gives the
original Generals an internet mode. Anyone can run a relay; the list is kept by the community.

Reference: the Mobsik build of this port (1.4.0-mobsik.23) — studied 04/10/2026. Its relay is
tied to its website (tickets) and to its own game download (`compat` holds that archive's
SHA-256), so this port cannot use it without impersonating that client; the frame layout and
virtual addresses are taken from it, the rest is our own. Protocol: `docs/port/ROOMS_PROTOCOL.md`.

## Parts

1. **Relay** — its own repository, https://github.com/MYSOREZ/Generals-Servers (`relay/`: Go,
   one static binary, Docker + Caddy; `install.sh`; `PROTOCOL.md`). Done 04/10/2026, with tests
   (`go test -race`). Moved out of this repository the same day.
2. **Server list** — the open issues of Generals-Servers with the label `server` (its "Add a
   server" form; the label exists). The launcher reads them through the GitHub API (no Actions,
   no token), asks each `/health`, sorts by ping. The repository name should come from the signed
   remote config (`rooms_registry`) so the list can move without a new APK.
3. **Launcher**
   - Rooms screen: server picker (list + custom address), public rooms of a server, create
     (capacity, title, password, public), join by code / link / QR, ready, launch.
   - Files: SHA-256 of the `.big` archives, `Data/INI` and maps, cached by size+mtime.
   - Install on my VPS: host, port, user, password or key → SSH (JSch fork `com.github.mwiede:jsch`,
     no other dependencies), host key shown on first connect, runs Generals-Servers `install.sh`, follows its
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
