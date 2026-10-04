#!/bin/sh
# GeneralsX @feature Android port 04/10/2026 Install or update a rooms relay on a Linux VPS.
#
#   curl -fsSL https://raw.githubusercontent.com/MYSOREZ/GeneralsZH-Android-Port/main/tools/rooms-relay/install.sh | sudo sh -s -- --name "My server"
#
# Options: --name NAME (shown in the launcher), --domain DOMAIN (default: <ip>.sslip.io, which
# needs no DNS setup), --ref GIT_REF (default: main), --max-rooms N, --max-players N.
# Installs Docker if it is missing, puts the relay behind Caddy (automatic HTTPS) in
# /opt/gx-rooms, and starts it on boot. Re-running it updates the server and keeps its settings.
#
# Lines starting with "GXROOMS:" are for the launcher, which runs this over SSH and follows them.
set -eu

REPO_RAW="https://raw.githubusercontent.com/MYSOREZ/GeneralsZH-Android-Port"
DIR=/opt/gx-rooms
FILES="go.mod go.sum main.go relay.go Dockerfile docker-compose.yml Caddyfile"

NAME="" DOMAIN="" REF="main" MAX_ROOMS="" MAX_PLAYERS=""
while [ $# -gt 0 ]; do
	case "$1" in
		--name) NAME="$2"; shift 2 ;;
		--domain) DOMAIN="$2"; shift 2 ;;
		--ref) REF="$2"; shift 2 ;;
		--max-rooms) MAX_ROOMS="$2"; shift 2 ;;
		--max-players) MAX_PLAYERS="$2"; shift 2 ;;
		*) echo "unknown option: $1"; echo "GXROOMS:ERROR usage"; exit 2 ;;
	esac
done

step() { echo "GXROOMS:STEP $1"; }
fail() { echo "error: $2" >&2; echo "GXROOMS:ERROR $1"; exit 1; }

[ "$(id -u)" = 0 ] || fail not_root "run as root (sudo)"
command -v curl >/dev/null 2>&1 || {
	step packages
	if command -v apt-get >/dev/null 2>&1; then
		apt-get update -q && apt-get install -yq curl ca-certificates
	elif command -v dnf >/dev/null 2>&1; then
		dnf install -yq curl ca-certificates
	else
		fail no_curl "install curl first"
	fi
}

step docker
if ! command -v docker >/dev/null 2>&1; then
	curl -fsSL https://get.docker.com | sh || fail docker_install "Docker could not be installed"
fi
systemctl enable --now docker >/dev/null 2>&1 || true
docker compose version >/dev/null 2>&1 || fail docker_compose "docker compose plugin missing"

step download
mkdir -p "$DIR"
for f in $FILES; do
	curl -fsSL "$REPO_RAW/$REF/tools/rooms-relay/$f" -o "$DIR/$f.new" || fail download "cannot download $f ($REF)"
done
for f in $FILES; do mv "$DIR/$f.new" "$DIR/$f"; done

step config
# Keep what an earlier install chose unless this run says otherwise.
if [ -f "$DIR/.env" ]; then
	. "$DIR/.env"
	[ -n "$NAME" ] || NAME="${RELAY_NAME:-}"
	[ -n "$DOMAIN" ] || DOMAIN="${RELAY_DOMAIN:-}"
	[ -n "$MAX_ROOMS" ] || MAX_ROOMS="${RELAY_MAX_ROOMS:-}"
	[ -n "$MAX_PLAYERS" ] || MAX_PLAYERS="${RELAY_MAX_PLAYERS:-}"
fi
if [ -z "$DOMAIN" ]; then
	IP="$(curl -4 -fsS --max-time 10 https://api.ipify.org || true)"
	echo "$IP" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' || fail no_ip "cannot find this server's public IPv4; pass --domain"
	DOMAIN="$(echo "$IP" | tr . -).sslip.io"
fi
NAME="$(printf '%s' "${NAME:-Rooms server}" | tr -d '\n"\\' | cut -c1-40)"
cat > "$DIR/.env" <<EOF
RELAY_NAME="$NAME"
RELAY_DOMAIN="$DOMAIN"
RELAY_MAX_ROOMS="${MAX_ROOMS:-200}"
RELAY_MAX_PLAYERS="${MAX_PLAYERS:-1000}"
RELAY_VERSION="$REF"
EOF

# Open the web ports if a host firewall is on; the provider's own firewall is the user's part.
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
	ufw allow 80/tcp >/dev/null && ufw allow 443/tcp >/dev/null
fi

step start
cd "$DIR"
docker compose up -d --build --remove-orphans || fail start "docker compose failed"
docker image prune -f >/dev/null 2>&1 || true

step check
i=0
until curl -fsS --max-time 5 "https://$DOMAIN/health" >/dev/null 2>&1; do
	i=$((i + 1))
	[ $i -lt 36 ] || fail tls "https://$DOMAIN/health does not answer: are ports 80 and 443 open in the provider's firewall?"
	sleep 5
done
echo "Rooms server is up: wss://$DOMAIN/ws"
echo "GXROOMS:URL wss://$DOMAIN/ws"
