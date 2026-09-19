# companion

Cross-platform desktop companion app for the XPMultiCrew plugin. Handles
all session-management UI (Formation/rendezvous connect, Shared Cockpit
setup) as a native window, instead of X-Plane's own window APIs.

**Why a companion app at all:** an earlier version of this UI lived inside
the X-Plane plugin itself, first as a legacy XPWidgets window (always
renders with the old X-Plane 9 look), then as a hand-drawn
`XPLMCreateWindowEx` window (content didn't track the window when dragged,
fiddly label/field alignment). Moving session management to a normal
desktop app sidesteps all of that: real layout, real text inputs, testable
without X-Plane at all. This matches how xPilot is built (companion app +
plugin) and was floated as an option in `docs/plan.md` section 8.

**Why [Wails](https://wails.io) specifically:** a first version of this app
ran a local HTTP server (`net/http`) and opened the UI in an "app mode"
browser window (Chrome/Edge/Brave `--app=` flag). That worked, but had no
reliable signal for "the window was closed" versus e.g. a browser
tab/process doing something unexpected - a `pagehide`-based auto-quit
mechanism fired unexpectedly once and killed the backend while the window
was still open and in use. Wails wraps a real native window with a real
webview (WebKitGTK on Linux, WebView2 on Windows) in the same process as
the Go backend, so "window closed" is unambiguous and the whole class of
bug is gone by construction. It also drops the HTTP/JSON API entirely -
the frontend calls Go methods directly (`frontend/wailsjs/go/main/App.js`,
auto-generated) and receives status via a push event instead of polling.

The plugin itself only does X-Plane-side work (datarefs, networking,
drawing) plus a tiny local control listener
(`plugin/include/control/control_listener.h`) that this app talks to.

## Plugin installation

This app also embeds the platform-matching plugin build and can install
it, so end users only ever download one thing instead of a separate
plugin zip they have to unpack into `Resources/plugins/` themselves. See
`plugin_install.go`:

- `//go:embed all:embedded_plugin` embeds whatever's staged at
  `companion/embedded_plugin/XPMultiCrew/` at build time - see "Building &
  testing" below for how that gets populated.
- The **Setup** panel's "Choose…" button opens a native folder picker
  (`runtime.OpenDirectoryDialog`) for the X-Plane installation, remembered
  across restarts in a small JSON file (`config.go`,
  `os.UserConfigDir()/xpmulticrew-companion/config.json`).
- "Install / Update Plugin" copies the embedded files into
  `<chosen path>/Resources/plugins/XPMultiCrew/`, replacing any existing
  install there. This follows the same atomic-rename discipline as manual
  plugin deploys must: never overwrite files of a currently-loaded `.xpl`
  in place (that crashed a real X-Plane instance once during this
  project's development) - the new build is written to a temp directory
  first, the old install (if any) is moved aside, then the new one is
  renamed into its final place. Restarting X-Plane is still required
  either way (plugins are only scanned at startup).

`plugin_install_test.go` exercises this swap logic against a fake
in-memory filesystem (`testing/fstest.MapFS`), not a real embedded plugin,
so it passes even in a fresh checkout before anything has been staged.

**Version display:** the Setup panel shows three version strings, since
they can legitimately disagree:

- **Installed** - read straight off disk (`<X-Plane>/Resources/plugins/
  XPMultiCrew/version.txt`) once you've chosen an X-Plane folder. Works
  even if X-Plane isn't running.
- **Available** - this companion build's own embedded `version.txt` (i.e.
  what Install/Update Plugin would give you).
- **Currently loaded in X-Plane** - only shown once the plugin has
  actually been seen (`PLUGIN_VERSION` over the control listener, see
  `control_listener.h`), since installing an update doesn't take effect
  until X-Plane is restarted.

All three come from the same source: `plugin/CMakeLists.txt` derives a
version string via `git describe --tags --always --dirty` at build time
and drops it as `version.txt` next to the built `.xpl` (and bakes it into
the binary itself for the `PLUGIN_VERSION` status line) - no manually
bumped version number to forget to update.

## Protocol (companion ↔ plugin)

Plain-text UDP, two fixed local ports (127.0.0.1 only - this controls the
X-Plane instance on the same machine, not a remote one):

- This app sends to `49030` (the plugin listens there):
  ```
  CREATE_SESSION <host:port>
  JOIN_SESSION <host:port> <code>
  START_SHARED_COCKPIT <MASTER|CLIENT> <rendezvous host:port> <code, empty for MASTER>
  GET_STATUS
  ```
  Shared Cockpit's peer discovery goes through the same rendezvous/relay
  server as Formation's Create/Join Session, not a manually-typed peer
  address - see the root `README.md`'s Phase 3 section for why (it's what
  makes it work over the internet without port forwarding). MASTER leaves
  the code blank; CLIENT must supply one.
- The plugin pushes status to `49031` (this app listens there), any time it
  changes and in reply to `GET_STATUS`:
  ```
  FORMATION <status text>
  FORMATION_CODE <code, empty if none yet>
  SHARED_COCKPIT <status text>
  SHARED_COCKPIT_CODE <code, empty if none yet - CLIENT never has one>
  PEERS <sender_id>:<icao>;<sender_id>:<icao>;... (Formation only, empty if none)
  SIM_READY <0|1>
  ```
  Formation's status text includes a live peer count (e.g. `connected,
  code 'ABC', peer 1 - 2 peer(s) online`), updated on every join/leave -
  found via a live two-person test that without this, the person who
  *created* the session had no way to tell someone had joined short of
  reading X-Plane's `Log.txt`. The `*_CODE` fields let the frontend
  auto-fill the session-code input once you create/join a session, instead
  of you reading it out of the status text and retyping it. `PEERS` drives
  the Formation panel's connected-peers list (aircraft type per peer,
  keyed by `sender_id` since the plugin doesn't currently correlate that
  with the rendezvous session's own small peer numbers). `SIM_READY`
  reflects whether X-Plane has actually finished loading a flight
  (`XPLM_MSG_PLANE_LOADED`) - the frontend shows a banner and disables
  Create/Join/Start while it's `0` (a click sent during loading wouldn't
  actually be lost either way, see `control_listener.h`'s comment, but
  blocking is clearer than a click silently doing nothing for a while).
  Create/Join/Start are also disabled once already connected/running, so
  you can't kick off a second one on top of a working session.

`app.go` polls `GET_STATUS` once a second and re-emits whatever
`PluginClient` currently holds as a `"status"` event to the frontend
(`frontend/src/main.js` listens via `EventsOn`) - no HTTP polling loop in
the browser anymore.

## System dependencies

- Go 1.22+, Node.js/npm (for the Vite-built frontend).
- The [Wails CLI](https://wails.io): `go install github.com/wailsapp/wails/v2/cmd/wails@latest`
- Linux build/runtime deps: GTK3 + WebKitGTK. **On distros that only ship
  WebKitGTK 4.1 (no `webkit2gtk-4.0.pc`, e.g. current Arch/CachyOS)**,
  `wails doctor` will wrongly report `libwebkit: Not Found` - that check is
  stale, not the actual build. Just build/run with the `webkit2_41` tag
  (see below); no need to install anything else if `pkg-config --modversion
  webkit2gtk-4.1` succeeds.
- Windows: WebView2 runtime (preinstalled on modern Windows 10/11).

## Building & testing

```sh
cd companion
go test -tags webkit2_41 ./...     # works even without a staged plugin - see plugin_install_test.go above
```

To get a binary that can actually *install* a real plugin (not just the
`.gitkeep` placeholder `go:embed` needs to compile at all), build the
plugin first and stage its output before `wails build`:

```sh
# from the repo root, build the plugin (see the root README's "Building" section)
./scripts/build-all.sh   # or scripts\build-all.bat on Windows

# stage this platform's build output for embedding, then build the companion app
cp -r build/XPMultiCrew companion/embedded_plugin/XPMultiCrew   # Windows: xcopy /E /I
cd companion
wails build -tags webkit2_41
```

CI does exactly this (build plugin → upload as an artifact → download it
into `embedded_plugin/` → build companion), see `.github/workflows/ci.yml`
and `release.yml`.

Output: `build/bin/xpmulticrew-companion` (single native binary, frontend
and - if staged - plugin assets embedded via `go:embed`).

If you change `App`'s exported methods in `app.go`, regenerate the JS
bindings without a full build:

```sh
wails generate module -tags webkit2_41
```

## Running

```sh
wails dev -tags webkit2_41   # hot-reload dev mode
# or
./build/bin/xpmulticrew-companion
```

Opens a real native window - closing it exits the process, no separate
cleanup step needed.

**Linux app menu integration:** copy `xpmulticrew-companion.desktop` to
`~/.local/share/applications/`, editing its `Exec=` line to the actual
binary path first:

```sh
sed "s|/absolute/path/to/xpmulticrew-companion-linux-amd64|$(pwd)/build/bin/xpmulticrew-companion|" \
    xpmulticrew-companion.desktop > ~/.local/share/applications/xpmulticrew-companion.desktop
```

Then "XPMultiCrew" shows up in your normal application launcher/menu -
double-click, no terminal needed.

The Shared Cockpit "DATAREF" list (which systems to sync) still comes from
`XPMultiCrew_shared_cockpit.txt` if present next to X-Plane - this app only
covers ROLE/PEER and Formation/rendezvous, not yet the dataref list itself.

## Release binaries

```sh
wails build -platform linux/amd64   -tags webkit2_41
wails build -platform windows/amd64 -tags webkit2_41
```

Windows builds need to happen on Windows (or via the Wails-documented
cross-compile toolchain) since WebView2's Go bindings are Windows-specific.
