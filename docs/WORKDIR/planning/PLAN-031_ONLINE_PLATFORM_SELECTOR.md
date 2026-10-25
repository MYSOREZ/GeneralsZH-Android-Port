# PLAN-031: Online platform selector -- not only GeneralsOnline

> Status: proposed 25/10/2026 at the owner's request ("otherwise we depend only on GeneralsOnline").

Goal: a switch in the launcher, **Online: GeneralsOnline | Rooms | ...**, so that online play does
not stop if one service goes away, refuses Android clients, or is unreachable from a country.
Each platform is a separate path to a match; the engine's match itself (lockstep, CRC, the LAN or
the GeneralsOnline transport) does not change.

## Platforms

1. **GeneralsOnline** -- as now, the default. Lobby, quickmatch, stats; Android-only matches since
   cross-play with PC is off. No mods.
2. **Rooms** -- our own: LAN over the internet through a relay (PLAN-023). Everything is written
   and was tested on two phones (test build rooms-r1, 04/10/2026), then taken out on 04/10 because
   no server was running. In this repository's history: engine bridge (`udp.cpp`, `LANAPI.cpp`),
   launcher screens (`RoomActivity`, `RoomServers`, `RoomSession`, `RoomFiles`,
   `RoomInstallActivity`), 105 strings in 13 languages -- commits 17506c26..3799f17d, removed
   together by 94e80308. The relay, installer and the self-maintaining server list live in
   https://github.com/MYSOREZ/Generals-Servers. Advantages over GeneralsOnline: works in **both
   games** (Generals has no GeneralsOnline at all), **with mods** (it is LAN: same files, same
   game), invite links and QR, anyone can run a server from the launcher (SSH install on a VPS).
3. **GameSpy-compatible community services** (later, only with their consent) -- C&C:Online
   (Revora) and OpenSpy run the original GameSpy protocol for Generals / Zero Hour, and the engine
   still carries its GameSpy client (`libgamespy.so`). Pointing it at their hosts is a
   configuration change, but their players are on PC: with cross-play off, Android games there
   would need to be kept apart, and that is their decision to make. Ask before writing code.

Not an option: the room server of another repackaged build (tied to its own site and download;
using it would mean impersonating that client).

## Order

1. Bring Rooms back: revert 94e80308 (conflicts only in `strings.xml`, `SetupActivity.java` and
   the diary -- resolvable), put it behind the selector instead of its own Home button.
2. The selector on the Online card: GeneralsOnline / Rooms; the choice decides what "Online" in
   the game's main menu opens (Rooms opens the LAN screen with the room joined). Strings in 13
   languages; texts that mention "online = GeneralsOnline" updated in the same commit.
3. One default Rooms server run by the owner (a ~$5/month VPS is enough: the relay is a single Go
   binary behind Caddy), in the signed remote config, so that it can be changed without an APK.
4. Test build, two phones on different networks, both games, one with a mod.
5. Then, only if C&C:Online / OpenSpy agree: a third entry.

## Needed from the owner

- A VPS (or a decision to rely on community servers only) for the default Rooms server.
- Whether to write to C&C:Online / OpenSpy.
