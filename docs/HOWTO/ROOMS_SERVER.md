# How to run a rooms server

A rooms server lets players meet in a room and play over **Network (LAN)** through the internet,
with no port forwarding on their side. It only passes packets between the players of a room: no
accounts, no database, nothing stored. Any server you run can be added to the list that every
player's launcher shows.

What it needs: a Linux VPS (Ubuntu or Debian; 1 CPU and 512 MB are plenty) with a public IPv4
address and ports **80** and **443** open. Traffic is small — a few kilobytes per second per player.
Pick a location close to the players: every packet goes through the server, so its distance is
their ping.

## From the launcher

Tools → Rooms server → Install on my VPS: enter the server's address, SSH user and password (or
key). The launcher connects over SSH, runs the install script below and shows its progress. The
password is used for that connection only and is not saved. When it finishes it shows the address
(`wss://….sslip.io/ws`) and offers to add the server to the public list.

## By hand

On the VPS, as root:

```sh
curl -fsSL https://raw.githubusercontent.com/MYSOREZ/GeneralsZH-Android-Port/main/tools/rooms-relay/install.sh | sh -s -- --name "My server"
```

It installs Docker if needed, puts the server in `/opt/gx-rooms` behind Caddy, which gets a free
HTTPS certificate on its own, and starts it on every boot. Without `--domain` the address is
`<your-ip-with-dashes>.sslip.io`, which works with no DNS setup. With your own domain, point an A
record at the VPS and pass `--domain rooms.example.com`.

Options: `--name`, `--domain`, `--max-rooms` (default 200), `--max-players` (default 1000),
`--ref` (git branch or tag to install from, default `main`).

Run the same command again to update; the settings are kept in `/opt/gx-rooms/.env`.

Check it: `https://<address>/health` answers with the server's name and how many rooms are open.

## Add it to the public list

Open a new issue with the **Rooms server** form
(<https://github.com/MYSOREZ/GeneralsZH-Android-Port/issues/new?template=rooms-server.yml>) and
give the address. Launchers pick it up within minutes, check that it answers, and sort servers by
ping. Close the issue to take the server off the list.

## Without the installer

```sh
cd tools/rooms-relay
echo 'RELAY_DOMAIN=rooms.example.com' > .env
docker compose up -d --build
```

Or run the binary alone behind your own TLS proxy: `go build && ./rooms-relay -listen :8080`; the
proxy must pass WebSocket upgrades on `/ws`.

## Managing it

- Logs: `cd /opt/gx-rooms && docker compose logs -f relay`
- Stop: `docker compose down`; remove: also `rm -rf /opt/gx-rooms`
- Nothing to back up: rooms live only in memory.

Protocol and limits: `docs/port/ROOMS_PROTOCOL.md`.
