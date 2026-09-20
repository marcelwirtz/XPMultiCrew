# Deploying to a Debian VPS

The server is a single static Linux binary (no runtime dependencies, not
even a matching glibc — built with `CGO_ENABLED=0`).

## Build the release binary

```sh
cd server
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-s -w" \
    -o xpmulticrew-rendezvous .
```

(`GOARCH=arm64` instead if the VPS is ARM.)

## Install

```sh
# On the VPS, as root or with sudo:
mkdir -p /opt/xpmulticrew
# copy the binary there (scp from your machine, or build directly on the VPS
# with a Go toolchain installed - either works, it's a 2-file source tree
# plus go.mod)
cp xpmulticrew-rendezvous /opt/xpmulticrew/
cp server/deploy/xpmulticrew-rendezvous.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now xpmulticrew-rendezvous
systemctl status xpmulticrew-rendezvous
```

The unit runs as a dynamically-allocated unprivileged user
(`DynamicUser=yes`) and restarts automatically if it crashes.

## Firewall

Open UDP port 45000 (or whatever `-port` you choose) to the internet:

```sh
# ufw
ufw allow 45000/udp
# or nftables/iptables equivalent
```

## Point clients at it

In the [companion app](../companion/)'s Formation panel, enter
`your-vps-hostname-or-ip:45000` as the server and click **Create
Session** (first person) or **Join Session** with the code they share
(everyone after). See the top-level README's Phase 2 section for the
full setup flow.

## Updating

```sh
# rebuild, then on the VPS:
systemctl stop xpmulticrew-rendezvous
cp xpmulticrew-rendezvous /opt/xpmulticrew/
systemctl start xpmulticrew-rendezvous
```

Sessions are in-memory only (see `session.go`) — a restart drops all
active sessions; pilots would need to re-create/re-join.

## Logs

```sh
journalctl -u xpmulticrew-rendezvous -f
```
