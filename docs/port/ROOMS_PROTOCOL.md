# Rooms: LAN over a relay — protocol v1

A room is a virtual LAN. Players join it on a relay server, then pick
**Multiplayer → Network (LAN)** in the game exactly as on Wi-Fi. The engine's LAN socket
(`Core/GameEngine/Source/GameNetwork/udp.cpp`) sends its datagrams into the room instead of the
network, so NAT, STUN/TURN and carrier blocking do not matter: every packet goes through one
WebSocket to the relay. Both games use it — Zero Hour and the original Generals — because both
use the same LAN code in Core.

Anybody can run a relay (`tools/rooms-relay/`, one Docker command), and the launcher can install
one on a VPS over SSH. Relays are listed by the community in GitHub issues (see *Server list*).

The design follows the room client of the Mobsik build of this port (1.4.0-mobsik.23): virtual
addresses `10.240.0.<slot>`, a 16-byte frame header, LAN ports only. It differs where that one is
tied to its own website and game distribution: rooms are created in the app, and files are
compared between the players of a room, not against a reference copy.

## Transport

`wss://<host>/ws` (plain `ws://` only for local testing). Text frames carry JSON control
messages, binary frames carry game datagrams. The server pings every 15 s; a client that does not
answer for 45 s is dropped.

`GET /health` answers `{"protocol":1,"version":"…","name":"…","rooms":N,"players":N,"maxRooms":N}`.
The launcher uses it to check a listed server and to measure its ping.

`GET /rooms` lists the rooms that are open to everyone (`public`, not started, not full):
`{"rooms":[{"code","title","game","compat","members","capacity","password":bool}]}`.

## Control messages (JSON, text frames)

Every message has `"type"`. The first message on a connection must be `hello`; until the server
answers with `room`, the client may send nothing else but `create`, `join` or `resume`.

### Client → server

| type | fields | meaning |
|---|---|---|
| `hello` | `protocol` (1), `game` (`zh`\|`generals`), `compat` (string), `name`, `device` (32 hex) | who this is. `compat` names the engine build and sim rate; players with different `compat` cannot share a room |
| `create` | `capacity` (2–8), `title`, `password` (optional), `public` (bool) | create a room and take slot 1 |
| `join` | `code`, `password` (optional) | take the lowest free slot |
| `resume` | `token` | take back your slot after a dropped connection (within 60 s) |
| `files` | `list`: `{path: sha256}` | the game files the match depends on; see *Files* |
| `ready` | `ready` (bool) | |
| `capacity` | `capacity` | creator only, before launch, not below the current member count |
| `launch` | | creator only, when everybody is ready and the files agree |
| `leave` | | leave; the creator's leaving closes the room before launch |

### Server → client

| type | fields |
|---|---|
| `room` | `code`, `slot` (1–8), `ip` (`10.240.0.<slot>` as a 32-bit integer), `capacity`, `title`, `game`, `public`, `locked` (launched), `canLaunch`, `allReady`, `token` (resume), `members`: `[{slot, name, ready, connected, files: "ok"\|"differs"\|"missing"\|"pending"}]` |
| `files` | `differs`: `[path…]`, `missing`: `[path…]`, `extra`: `[path…]` — this player's differences from the creator's list |
| `launch` | the creator launched: start the game and open the LAN screen |
| `error` | `code`, `retry` (bool). The launcher localizes by `code`; the server never sends text meant for the player |

Error codes: `protocol`, `bad_request`, `no_room`, `full`, `password`, `started`, `game_mismatch`,
`compat_mismatch`, `not_creator`, `not_ready`, `files_mismatch`, `server_full`, `rate`,
`resume_expired`, `closed`.

`room` is sent to every member whenever anything in it changes.

## Game datagrams (binary frames)

```
offset size  field
0      4     magic "GXR1" (0x47585231), big-endian like every field below
4      4     source IP       10.240.0.<slot> of the sender
8      4     destination IP  10.240.0.<slot>, or 0xFFFFFFFF for broadcast
12     2     source port
14     2     destination port
16     ≤16384 payload (one UDP datagram)
```

Ports are the game's LAN ports: 8086 (LAN lobby) and 8088–8095 (match). The server drops a frame
whose source IP is not the sender's own, whose destination is not a member, whose ports are not
LAN ports, or that is larger than the limit; broadcast goes to every other member. A member may
send at most 1000 frames and 4 MB per second; a slower receiver whose queue passes 1 MB is
disconnected rather than delayed, because a lockstep game cannot use late packets.

## Files

Before launch every member sends `files`: SHA-256 of the files that decide the simulation (the
`.big` archives, `Data/INI`, maps). The server compares each list with the creator's and marks the
member `ok` or `differs`; `launch` is refused until all are `ok`. The player is told which files
differ, so a mismatch is fixed before the match instead of ending it in a desync.

## Server list

Relays are listed in this repository's issues with the label `rooms-server`, filed through the
"Rooms server" issue form (the launcher opens it pre-filled after installing a server). The
launcher reads the open issues of that label from the GitHub API, takes the `wss://` address from
each, asks `/health`, and shows the servers that answer, by ping. A server that is gone simply does
not answer; the maintainer closes issues that abuse the list. A player can also type any address
by hand.
