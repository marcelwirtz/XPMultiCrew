# server

Rendezvous/relay server (docs/plan.md, sections 3, 7, 11 — Phase 2). Go,
stdlib only (no external dependencies).

**What it does:** one UDP port, JSON-framed messages.

- A client sends `create_session` and gets back a short, human-typeable
  session code (e.g. `2K2SYJ`) plus its own member id.
- Other clients `join_session <code>`. The server tells the joiner about
  every existing member's public `ip:port` (as it observed it — this is the
  actual NAT-traversal trick), and tells every existing member about the
  joiner. Not capped at 2 participants — any number can join one session.
- Clients use those addresses to attempt direct UDP hole-punching with each
  other. The server never needs to know whether that succeeded: a `relay`
  message from any client is always fanned out to every other member of its
  session, so relay is an always-available fallback path, not something
  that has to be explicitly requested when hole-punching fails.
- A client sends `leave_session` when it's about to disconnect on purpose
  (plugin disable/quit, or switching to a different session/server) - the
  rest of the session is told via `peer_left` immediately, instead of
  waiting for the 120s stale-client sweep below to notice. Best-effort
  (UDP, fire-and-forget): a crash or force-kill still falls back to that
  sweep, same as always. Found via a live test with two real X-Plane
  instances that this was missing entirely - a plugin quitting left a
  ghost peer in the session for up to two minutes.
- Clients not heard from (no `relay`/`keepalive`/join/`leave_session`) for
  120s are evicted and the rest of the session is told via `peer_left`; an
  empty session is deleted. That timeout is deliberately generous, not
  30s: found via a live X-Plane test that the plugin's flight loop (where
  it polls for our replies and sends keepalives) doesn't run at all during
  a loading screen, which can easily take 30-60+ seconds - a short timeout
  evicted sessions before the plugin ever got a chance to keep one alive.
- Every `keepalive` gets a `keepalive_ack` back. This is the client's own
  auto-reconnect's only positive "the server is actually still there"
  signal: a client alone in a session (no peer yet, so no relay/peer_*
  traffic either) would otherwise see total silence and have no way to
  tell "quiet because nobody's here yet" apart from "quiet because the
  server/network is unreachable" - see
  `plugin/include/formation/rendezvous_client.h`'s `on_disconnected`.

The server is deliberately payload-agnostic: `payload` is just base64 bytes
from the plugin's netcore (e.g. an `AircraftStatePacket`) — the server never
parses it.

## Building & testing

```sh
cd server
go build ./...
go test ./...          # session/relay/eviction logic, no real sockets needed
```

## Running

```sh
go run . -port 45000
# or: go build -o xpmulticrew-rendezvous . && ./xpmulticrew-rendezvous -port 45000
```

See [`DEPLOY.md`](DEPLOY.md) for deploying a static release binary to a
Debian VPS with systemd.

## Not yet done

- No TLS/encryption on the control or relay channel (plan section 9 flags
  this as needed before real use — flight data would otherwise cross the
  open internet in cleartext via relay).
- No rate limiting / abuse protection (a stranger who finds a session code
  could join it — codes should be treated as a shared secret for now, like
  SmartCopilot's Connection-ID).
- No auto-reconnect: if a client does get evicted (e.g. a real network
  outage longer than 120s, not just a loading screen), the plugin currently
  just stops relaying/keepaliving (see `RendezvousClient::PollIncoming()`'s
  error handling) rather than automatically re-creating or re-joining the
  session. The user has to reload the plugin to get a fresh session/code.
- Not yet deployed anywhere — tested locally (127.0.0.1) against a real
  X-Plane instance and via the C++/Go integration tests. Deploying it to a
  real VPS and testing actual UDP hole-punching across two separate
  networks is still open.
