# XPMultiCrew

Cross-platform (Windows + Linux) tool for X-Plane 12 that connects multiple
pilots over the internet in two modes:

1. **Formation** — everyone flies their own aircraft, sees the others
   correctly animated via CSL models (TCAS-override API + `XPLMInstance`).
2. **Shared Cockpit** — multiple pilots fly the same aircraft across
   separate X-Plane instances, master/client with request-release control.

ATC, flight plans and voice routing are explicitly out of scope. The
sections below cover the architecture and rationale for each part
(internal design notes in `docs/plan.md` are not part of this repo).

## Quick start (just want to use it)

Download the latest release for your OS from the
[Releases page](https://github.com/marcelwirtz/XPMultiCrew/releases) — one
zip, no separate plugin download:

1. Unzip it and run the app inside (`xpmulticrew-companion` /
   `xpmulticrew-companion.exe`).
2. In the **Setup** panel: click **Choose…** and pick your X-Plane 12
   installation folder, then click **Install / Update Plugin** — the app
   has the matching plugin build embedded and copies it into
   `Resources/plugins/XPMultiCrew/` for you (atomically, so it's safe even
   if X-Plane happens to be running - see `companion/plugin_install.go`).
3. Restart X-Plane (plugins are only loaded at startup) and use the
   Formation/Shared Cockpit panels below the Setup one.

Building from source (plugin and/or companion app separately) is still
fully supported - see the sections below. The combined download above is
the only pre-built release artifact; there's no separate plugin-only zip
anymore (there was for `v0.1.0` — dropped once the companion app could
install the plugin itself, see `companion/README.md`).

## Repo layout

- `plugin/` — the X-Plane plugin (C++17, XPLM SDK), builds on Windows and
  Linux from the same source.
- `netcore/` — network client library. Currently just the Phase 0 UDP spike
  (shared wire struct + socket wrapper); the real transport lands Phase 1/2.
- `tools/udp_position_listener/` — standalone CLI used to verify the Phase 0
  UDP spike without a second X-Plane instance.
- `tools/formation_fake_peer/` — standalone CLI that sends a synthetic
  orbiting aircraft, for testing Formation mode solo (see Phase 1 below).
- `tests/` — pure-logic unit tests (no XPLM/network dependency), run via CTest.
- `server/` — rendezvous/relay server (Go, Phase 2 — see below).
- `plugin/include+src/shared_cockpit/` — Shared Cockpit (Phase 3 — see below).
- `companion/` — cross-platform desktop companion app (Go) for session
  management (create/join session, Shared Cockpit setup) — see
  [`companion/README.md`](companion/README.md). Replaced an earlier in-sim
  X-Plane window; the plugin now only has a small local control listener
  (`plugin/include/control/control_listener.h`) that this app talks to.

## Building (Phase 0)

The X-Plane SDK itself is not redistributed here (Laminar Research
license). Download it from
<https://developer.x-plane.com/sdk/plugin-sdk-downloads/>; the zip extracts
into a top-level `SDK/` folder — rename/move that to `third_party/XPSDK` (so
that `third_party/XPSDK/CHeaders/...` exists), or point CMake at wherever
you put it.

```sh
# Linux/macOS
./scripts/build-all.sh [/path/to/extracted/XPSDK]

# Windows
scripts\build-all.bat [C:\path\to\extracted\XPSDK]
```

This builds both the plugin and `udp_position_listener`. The plugin lands
as `lin.xpl` / `win.xpl` / `mac.xpl` under `build/XPMultiCrew/` (on
Windows/MSVC, one config subfolder deeper, e.g.
`build/XPMultiCrew/RelWithDebInfo/win.xpl`). Copy that file into
`<X-Plane>/Resources/plugins/XPMultiCrew/<lin|mac|win>.xpl` to load it.

`tools/udp_position_listener` has no SDK dependency and can also be
configured as its own standalone CMake project if you just want to build
that piece (`cmake -S tools/udp_position_listener -B build-listener`).

### What the Phase 0 plugin proves

- Reads the aircraft's own position dataref
  (`sim/flightmodel/position/{latitude,longitude,elevation}`) every 5
  seconds and writes it to X-Plane's `Log.txt` — proof the SDK/CMake setup
  works on both platforms.
- Sends the same position at 5 Hz as a raw UDP packet to `127.0.0.1:49001` —
  proof the plugin can get a dataref to a second, separate process over a
  real socket, before any internet/NAT-traversal logic (Phase 2) exists.

To see the second part in action: run `udp_position_listener` first, then
load the plugin into a running X-Plane instance on the same machine and
start a flight. The listener prints one line per received packet
(`seq=... lat=... lon=... elev=...m`).

## Phase 1: Formation mode LAN MVP

Adds a richer aircraft-state protocol (position, attitude, gear/flap/
speedbrake, lights, ICAO type — `netcore/include/flytogether/aircraft_state.h`)
and exchanges it with any number of LAN peers over UDP port 49002, tracking
each one's position with dead-reckoning interpolation
(`plugin/include/sync/remote_aircraft.h`) so it stays smooth between
updates, and draws each tracked peer via
[XPMP2](https://github.com/TwinFan/XPMP2) (TCAS-override API + `XPLMInstance`,
`plugin/include/formation/csl_aircraft.h`).

**CSL models:** rather than requiring a manual CSL package download (the
community-standard "Bluebell" package's textures can't legally be
auto-redistributed by us — see the CSL strategy discussion), the plugin
ships and auto-loads a small generic placeholder model
(`plugin/Resources/CSL/Generic`, a hand-built box-shaped aircraft, MIT/CC0
throughout) used for every peer regardless of their real aircraft type.
Zero manual setup required.

**Want real aircraft models instead of the box?** Install a CSL package
yourself and drop it in — no code changes, no rebuild:

1. Get a package, e.g.
   [Bluebell](https://forums.x-plane.org/files/file/37041-bluebell-obj8-csl-packages/)
   or [X-CSL](https://x-csl.ru/) (check each package's own license before
   using it — we can't vet or bundle either for you, see the CSL strategy
   discussion for why).
2. Extract it as its own folder under
   `<X-Plane>/Resources/plugins/XPMultiCrew/Resources/CSL/`, e.g.
   `.../Resources/CSL/Bluebell/` — the folder name doesn't matter, XPMP2
   just needs to find an `xsb_aircraft.txt` in it.
3. Restart X-Plane (CSL packages are only scanned once at plugin startup,
   not on plugin re-enable). `XPMPLoadCSLPackage` searches `Resources/CSL/`
   recursively up to 5 folders deep, so any number of packages can sit
   there side by side with the bundled `Generic` one - existing peers just
   keep using whichever model wins X-Plane's own type/livery matching.

**Extra build dependency:** [XPMP2](https://github.com/TwinFan/XPMP2) (MIT
licensed), vendored the same way as the X-Plane SDK:

```sh
git clone https://github.com/TwinFan/XPMP2.git third_party/XPMP2
```

Then build as before (`scripts/build-all.sh` / `.bat`) — this also builds
`tools/formation_fake_peer`, see below.

**Setup for a LAN test with a friend:**

1. Both of you build and install the plugin (see above), including
   `third_party/XPMP2`.
2. Copy [`XPMultiCrew_peers.example.txt`](XPMultiCrew_peers.example.txt) to
   `XPMultiCrew_peers.txt` in X-Plane's working directory (or set
   `XPMULTICREW_PEERS_FILE` to point at it), and list the other person's LAN
   IP with port 49002.
3. Start X-Plane on both machines. `Log.txt` should show
   `XPMultiCrew: formation sync ready, sender_id=..., 1 peer(s) loaded...`
   on enable, then once packets arrive, `XPMultiCrew[formation]: tracking N
   peer(s), N drawn via CSL` once per second, and the other pilot's aircraft
   should appear (as the generic placeholder shape) in the 3D world, on
   TCAS, and on the map.

**Testing solo, without a second pilot:** build and run
`tools/formation_fake_peer` while the plugin is loaded in a running
X-Plane — it sends a synthetic aircraft slowly orbiting a position you give
it (pass your own aircraft's current lat/lon/elevation as arguments) as
real UDP packets on the same port the plugin listens on. No peer list
entry needed since this only exercises the *receiving* side. You should see
a banked, circling generic-shaped aircraft nearby within a few seconds.

The protocol and the `FormationSync`/`RemoteAircraft` code are not tied to
exactly 2 participants — any number of peers (each identified by a random
per-session `sender_id`, not by which line of the peer list they're on) get
tracked independently, each with its own XPMP2 aircraft created/destroyed
as it appears/goes stale. This is verified by a 3-node local mesh test, not
just asserted.

**Testing without X-Plane:**

```sh
cmake --build build --target remote_aircraft_test
ctest --test-dir build --output-on-failure
```

Runs the dead-reckoning math (linear position extrapolation, heading
wraparound, stale/out-of-order packet handling, extrapolation timeout) as
pure unit tests with no XPLM or network dependency.

## Phase 2: internet play (rendezvous/relay server)

Two people no longer need to be on the same LAN. `server/` (Go, stdlib
only, see [`server/README.md`](server/README.md)) is a small rendezvous
server: clients `create_session` and get back a short code
(e.g. `2K2SYJ`), others `join_session <code>`, and the server tells
everyone in the session everyone else's public `ip:port` (as it observed
it — that's the actual NAT-traversal trick) so clients can try direct UDP
hole-punching. A `relay` message is always fanned out to the rest of the
session too, as an always-on fallback for when hole-punching doesn't work
(e.g. CGNAT) — the plugin currently uses both paths in parallel rather
than detecting which one actually got through, relying on the existing
sequence-number dedup (`sync/remote_aircraft.cpp`) to make that harmless.

**Plugin side** (`plugin/include/formation/rendezvous_client.h` +
`rendezvous_protocol.h` + `base64.h`): speaks the server's JSON-over-UDP
protocol on its own control socket, feeds discovered peer addresses into
`FormationSync::AddPeer()` for direct sends, and decodes relayed packets
back into `AircraftStatePacket`s via `FormationSync::IngestPacket()` — the
exact same dead-reckoning/CSL-drawing pipeline Formation mode already uses
for LAN peers, so nothing downstream needed to change.

**Setup:** run the server somewhere reachable (e.g. your VPS, see
`server/DEPLOY.md`), then either:

- **Recommended:** run the [companion app](companion/) and use its
  "Create Session" / "Join Session" buttons - this also sidesteps a real
  bug the file-based path can hit (see `server/session.go`'s
  `clientTimeout` comment: X-Plane's loading screen can eat the whole
  window before a file-triggered auto-connect gets a chance to keep itself
  alive; clicking a button only happens once the flight loop - and thus
  the connection's keepalive - is already running).
- Or create `XPMultiCrew_rendezvous.txt` next to X-Plane (or set
  `XPMULTICREW_RENDEZVOUS_FILE`), which still works exactly as before:

```
# whoever starts first:
SERVER your-vps.example.com:45000
CREATE

# everyone else, once they have the code (shown in the companion app's
# status, or the first person's Log.txt):
SERVER your-vps.example.com:45000
JOIN 2K2SYJ
```

`Log.txt` prints the session code and each peer join/leave/error either way.

**Verified so far:** the Go server has a full unit + real-socket
integration test suite (`server/`, 12+ tests); the C++ `RendezvousClient`
similarly (`tests/rendezvous_client_test.cpp`, a real client against a
real fake-server socket). **Flown for real** with two separate people over
the internet (one Linux, one Windows, server deployed on a real VPS) —
this surfaced and fixed several real bugs the local/simulated tests
couldn't have caught:

- The plugin's flight loop (where it polls for server replies and sends
  keepalives) doesn't run during X-Plane's loading screen, which the
  server's original 30s client timeout didn't survive — see
  `server/session.go`'s `clientTimeout` comment.
- The person who *created* a session had no visible feedback when someone
  joined (their aircraft would just start appearing, silently) - Formation
  status now includes a live peer count, updated on every join/leave.
- Quitting the plugin (or switching sessions) never told the server -
  remaining peers only found out up to 120s later via the stale-client
  timeout. Fixed with an explicit `leave_session` message sent on
  disconnect - see `server/README.md`.
- No way to tell from the companion app whether X-Plane had actually
  finished loading a flight yet - added a `SIM_READY` status signal
  (`XPLM_MSG_PLANE_LOADED`) the companion app shows a banner for.

**Not yet verified:** whether direct P2P (UDP hole-punching) actually
succeeded versus falling back to relay for that test - both work from the
user's perspective, so there was no way to tell without packet capture;
worth checking next time if latency matters to you.

**Auto-reconnect:** the plugin keeps retrying (every 5s) to rejoin your
current session on its own if the connection to the rendezvous server is
lost - a transient network/server blip no longer strands you mid-flight
waiting for someone to notice and re-click Create/Join. Detected via a
`keepalive_ack` the server now sends back for every keepalive
(`RendezvousClient::on_disconnected`, see its comment for why a plain
"haven't heard anything" timeout alone isn't enough while alone in a
quiet session with no peer yet). This only stops once you explicitly hit
**Disconnect** in the companion app - the same button also finally
provides a way to leave a session at all, short of quitting the plugin.
Same behavior for Shared Cockpit, with one difference: losing the
rendezvous heartbeat does *not* interrupt an already-running flight, since
the direct-UDP sync to your co-pilot's already-known address may well
still be working fine even though the backup relay path briefly dropped.

## Phase 3: Shared Cockpit MVP

Two pilots fly the *same* aircraft in separate X-Plane instances. One is
**MASTER** — its position/attitude is authoritative — the other is
**CLIENT**, whose own physics gets overridden
(`sim/operation/override/override_planepath[0]`, see
`shared_cockpit/shared_cockpit_sync.h`) and driven directly from the
master's state, reusing Formation mode's exact dead-reckoning
(`RemoteAircraft`) for smoothness between updates.

**Systems (radios, autopilot, switches):** no per-aircraft mapping file
with custom code — since both pilots fly an identical aircraft, a watched
dataref name resolves to the same thing on both sides. A generic,
*bidirectional* sync (`shared_cockpit/dataref_sync.h`) reads any configured
dataref's type automatically (`XPLMGetDataRefTypes`) and mirrors changes
either direction — either pilot can operate the listed systems, modeling a
real shared cockpit's pilot-flying/pilot-monitoring split. Only flight
control (position/attitude) is one-way, master → client. There's no
per-switch ownership/request-release arbitration yet — whichever side
writes last wins if both touch the same dataref near-simultaneously.

**The CLIENT gets the MASTER's complete systems state once, right when it
connects** — not just future changes from that point on. Both sides
technically broadcast everything they're watching the moment they start
(nothing's "known yet" to compare against), but the CLIENT seeds its own
baseline from its current cold-start values first so it doesn't
reciprocally broadcast *those* back at the MASTER it just joined — see
`DatarefSync::Start`'s `seedFromCurrentValues` comment. Without this, both
sides' initial dumps would race, and whichever arrived last would
silently clobber the other's already-configured cockpit.

**Peer discovery works exactly like Formation's internet play** (same
rendezvous/relay server, `server/`, docs/plan.md section 7) rather than a
manually-typed peer address — this is what lets two people play over the
internet without port forwarding. From the [companion app](companion/):
MASTER picks a server and leaves the session code blank (a new session is
created and the code is shown in the Shared Cockpit status — share it with
your co-pilot), CLIENT enters that server + code. Once the rendezvous
server has introduced both sides, the plugin starts sending
position/dataref updates both directly (peer-to-peer UDP, for lower
latency) and relayed through the server in parallel — the same
"send both, let sequence-number dedup make it harmless" policy Formation
already uses, needed here because Shared Cockpit's client side never sends
anything on its own to open a NAT hole for the master's inbound stream, so
direct UDP alone isn't reliably reachable from the internet.

The `DATAREF` list (which systems to sync — radios, transponder, AP bugs,
trim, flaps, lights, etc.) still has to come from a file, and is picked
per-aircraft automatically: the plugin resolves it in this order (see
`ResolveSharedCockpitConfigPath`) —

1. `$XPMULTICREW_SHARED_COCKPIT_FILE`, if set — explicit override.
2. `XPMultiCrew_shared_cockpit_profiles/<ICAO type>.txt`, next to X-Plane,
   if that file exists for the aircraft you're currently flying — your own
   override/customization, checked first so it always wins if present. Add
   a `<ICAO>.txt` file here for any aircraft, no ROLE/PEER needed in it,
   just `DATAREF` lines.
3. The profile bundled *with the plugin itself*
   (`Resources/plugins/XPMultiCrew/Resources/shared_cockpit_profiles/<ICAO type>.txt`) —
   ships pre-populated (currently
   [`C172.txt`](plugin/Resources/shared_cockpit_profiles/C172.txt), a
   complete, real-`DataRefs.txt`-checked list for the default X-Plane 12
   Cessna 172SP) and is (re)installed automatically every time the plugin
   is installed/updated via the companion app, so only one person ever has
   to figure out a correct `DATAREF` list for a given aircraft and everyone
   benefits — no manual setup needed for aircraft that already have one.
4. `XPMultiCrew_shared_cockpit.txt` (copy from
   [`XPMultiCrew_shared_cockpit.example.txt`](XPMultiCrew_shared_cockpit.example.txt)) —
   flat fallback for aircraft without a profile anywhere above. If this
   file also sets `ROLE`/a direct `PEER` address, the plugin auto-starts
   with that manual peer on `XPluginEnable` instead of waiting for the
   companion app — that ROLE/PEER pair is ignored once you start Shared
   Cockpit from the companion app instead (which always goes through the
   rendezvous server).

Both of you need to be flying the same aircraft (and have the same profile
file on both machines) — plan's first target: standard Cessna 172, which
now works out of the box thanks to the bundled profile above. Want another
aircraft supported for everyone? Add a `<ICAO>.txt` under
`plugin/Resources/shared_cockpit_profiles/` and open a PR.

**Verified so far:** the wire format (`shared_cockpit/dataref_sync_protocol.h`)
and the Euler→quaternion conversion used to hand physics back cleanly
(`shared_cockpit/quaternion.h`, formula from X-Plane's own
["Moving the Plane"](https://developer.x-plane.com/article/movingtheplane/)
article) are unit tested, as is the rendezvous-based ROLE/session-code
control-listener protocol (`tests/control_listener_test.cpp`), the
per-aircraft profile file resolution (`tests/shared_cockpit_config_test.cpp`),
and the companion app's command formatting (`companion/app_test.go`). All
the datarefs involved (`override_planepath`, `local_x/y/z`, `q`,
`local_vx/vy/vz`, and every dataref in the C172 profile and the example
systems list) were checked against a real X-Plane 12 `DataRefs.txt`, not
guessed. **Not yet verified:** actually
flying with two real X-Plane instances over the internet — this needs your
co-pilot and hasn't happened yet, so treat the position takeover, dataref
mirroring, and the new rendezvous-based peer discovery as
implemented-but-unflown until that first real test.

## License

[MIT](LICENSE). [XPMP2](https://github.com/TwinFan/XPMP2) (vendored under
`third_party/XPMP2`, not committed here) is MIT licensed; the X-Plane SDK
(`third_party/XPSDK`, also not committed) is Laminar Research/Sandy
Barbour/Ben Supnik's own BSD-style license, see the copy downloaded
alongside it.
