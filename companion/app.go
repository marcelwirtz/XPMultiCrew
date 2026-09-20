package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"time"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// ChooseXPlaneResult is what ChooseXPlanePath returns to the frontend.
type ChooseXPlaneResult struct {
	Path    string `json:"path"`
	Warning string `json:"warning,omitempty"`
}

// statusEvent is what "status" events carry to the frontend (see
// frontend/src/main.js's runtime.EventsOn("status", ...)). Replaces the old
// HTTP-server version's polling GET /api/status.
type statusEvent struct {
	Formation              string            `json:"formation"`
	FormationCode          string            `json:"formationCode"`
	SharedCockpit          string            `json:"sharedCockpit"`
	SharedCockpitCode      string            `json:"sharedCockpitCode"`
	SharedCockpitOwnership map[string]string `json:"sharedCockpitOwnership"`
	Peers                  []FormationPeer   `json:"peers"`
	LinkQuality            LinkQuality       `json:"linkQuality"`
	SharedCockpitMismatch  string            `json:"sharedCockpitMismatch"`
	SimReady               bool              `json:"simReady"`
	RunningVersion         string            `json:"runningVersion"`
}

// App is bound to the frontend via wails.Run's Bind option - every exported
// method becomes an async JS function in frontend/wailsjs/go/main/App.js.
type App struct {
	ctx    context.Context
	plugin *PluginClient
}

func NewApp() *App {
	return &App{plugin: NewPluginClient()}
}

// startup is called once the native window/webview is ready. It starts
// talking to the X-Plane plugin's control listener (see plugin_client.go)
// and pushes every status change to the frontend as a "status" event,
// instead of the frontend polling an HTTP endpoint.
func (a *App) startup(ctx context.Context) {
	a.ctx = ctx

	go a.plugin.ListenForStatus()
	go a.pollStatus()
}

// pollStatus periodically asks the plugin for its current status (in case
// it started after us, or a push was lost) and emits whatever PluginClient
// currently holds as a "status" event every tick, so the frontend doesn't
// need its own polling loop at all.
func (a *App) pollStatus() {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		_ = a.plugin.Send("GET_STATUS")
		formation, sharedCockpit := a.plugin.Status()
		formationCode, sharedCockpitCode := a.plugin.Codes()
		runtime.EventsEmit(a.ctx, "status", statusEvent{
			Formation:              formation,
			FormationCode:          formationCode,
			SharedCockpit:          sharedCockpit,
			SharedCockpitCode:      sharedCockpitCode,
			SharedCockpitOwnership: a.plugin.SharedCockpitOwnership(),
			Peers:                  a.plugin.FormationPeers(),
			LinkQuality:            a.plugin.LinkQuality(),
			SharedCockpitMismatch:  a.plugin.SharedCockpitAircraftMismatch(),
			SimReady:               a.plugin.SimReady(),
			RunningVersion:         a.plugin.RunningVersion(),
		})
	}
}

// CreateSession asks the plugin to open a new Formation/rendezvous session
// on the given server. Bound to the frontend's "Create Session" button.
func (a *App) CreateSession(server string) error {
	server = strings.TrimSpace(server)
	if server == "" {
		return errors.New("server address is required")
	}
	return a.plugin.Send("CREATE_SESSION " + server)
}

// JoinSession asks the plugin to join an existing Formation/rendezvous
// session. Bound to the frontend's "Join Session" button.
func (a *App) JoinSession(server, code string) error {
	server = strings.TrimSpace(server)
	code = strings.TrimSpace(code)
	if server == "" || code == "" {
		return errors.New("server address and session code are required")
	}
	return a.plugin.Send("JOIN_SESSION " + server + " " + code)
}

// StartSharedCockpit asks the plugin to start Shared Cockpit with the given
// role (MASTER/CLIENT), using the same rendezvous-server session-code flow
// as Formation's CreateSession/JoinSession (see control_listener.h's
// protocol comment) - this is what makes it work over the internet, not
// just LAN with manually-typed peer addresses. MASTER can leave code blank
// (the plugin creates a new session and reports the code back via
// status); CLIENT must supply one. Bound to the frontend's "Start Shared
// Cockpit" button.
func (a *App) StartSharedCockpit(role, server, code string) error {
	server = strings.TrimSpace(server)
	code = strings.TrimSpace(code)
	if server == "" {
		return errors.New("server address is required")
	}
	if role != "MASTER" {
		role = "CLIENT"
		if code == "" {
			return errors.New("session code is required to join as CLIENT")
		}
	}
	return a.plugin.Send("START_SHARED_COCKPIT " + role + " " + server + " " + code)
}

// RequestOwnership asks the plugin to claim a Shared Cockpit "systems"
// dataref category (engine/avionics/systems) for this side - see
// control_listener.h's REQUEST_OWNERSHIP and
// shared_cockpit/ownership_tracker.h. Bound to the frontend's ownership
// buttons. Unlike StartSharedCockpit's role validation, an unrecognized
// category is the plugin's problem to ignore (see
// ControlListener::HandleLine), not this method's - the frontend only
// ever sends the three fixed category names its buttons carry.
func (a *App) RequestOwnership(category string) error {
	return a.plugin.Send("REQUEST_OWNERSHIP " + strings.TrimSpace(category))
}

// RespondOwnership answers a pending incoming ownership request (see
// control_listener.h's SHARED_COCKPIT_OWNERSHIP "requested" state) for
// category with Grant (grant=true) or Deny (grant=false) - see
// control_listener.h's RESPOND_OWNERSHIP. Bound to the frontend's
// Grant/Deny prompt buttons.
func (a *App) RespondOwnership(category string, grant bool) error {
	decision := "deny"
	if grant {
		decision = "grant"
	}
	return a.plugin.Send("RESPOND_OWNERSHIP " + strings.TrimSpace(category) + " " + decision)
}

// DisconnectFormation asks the plugin to leave its current Formation
// session (if any) and stop auto-reconnecting - see control_listener.h's
// DISCONNECT_FORMATION and RendezvousClient::on_disconnected's comment.
// Bound to the frontend's "Disconnect" button in the Formation panel.
func (a *App) DisconnectFormation() error {
	return a.plugin.Send("DISCONNECT_FORMATION")
}

// DisconnectSharedCockpit is DisconnectFormation's Shared Cockpit
// equivalent. Bound to the frontend's "Disconnect" button in the Shared
// Cockpit panel.
func (a *App) DisconnectSharedCockpit() error {
	return a.plugin.Send("DISCONNECT_SHARED_COCKPIT")
}

// GetSavedServers returns the address book - previously-saved rendezvous
// server addresses (see config.go's SavedServer) - so reconnecting to a
// server used before doesn't mean retyping it. Never nil, so it marshals
// to JSON `[]` rather than `null`.
func (a *App) GetSavedServers() []SavedServer {
	servers := loadConfig().SavedServers
	if servers == nil {
		servers = []SavedServer{}
	}
	return servers
}

// SaveServer adds a server to the address book, or updates its address if
// a saved entry with the same label already exists (label is the dedup
// key - saving under a label you've already used just updates that
// entry's address rather than creating a duplicate). Bound to the
// frontend's "save this address" action.
func (a *App) SaveServer(label, hostPort string) error {
	label = strings.TrimSpace(label)
	hostPort = strings.TrimSpace(hostPort)
	if label == "" || hostPort == "" {
		return errors.New("a label and a server address are both required")
	}
	cfg := loadConfig()
	for i, s := range cfg.SavedServers {
		if s.Label == label {
			cfg.SavedServers[i].HostPort = hostPort
			return saveConfig(cfg)
		}
	}
	cfg.SavedServers = append(cfg.SavedServers, SavedServer{Label: label, HostPort: hostPort})
	return saveConfig(cfg)
}

// DeleteSavedServer removes a server from the address book by label. A
// no-op (not an error) if no saved server has that label - deleting
// something already gone isn't a failure. Bound to the frontend's address
// book entries' delete action.
func (a *App) DeleteSavedServer(label string) error {
	cfg := loadConfig()
	for i, s := range cfg.SavedServers {
		if s.Label == label {
			cfg.SavedServers = append(cfg.SavedServers[:i], cfg.SavedServers[i+1:]...)
			return saveConfig(cfg)
		}
	}
	return nil
}

// GetRecentLogLines returns the plugin's own new lines from X-Plane's
// Log.txt since sinceOffset (see diagnostics.go's GetRecentLogLines) -
// bound to the frontend's Diagnostics panel, polled every second the same
// way pollStatus() already polls plugin status.
func (a *App) GetRecentLogLines(sinceOffset int64) (LogLinesResult, error) {
	return GetRecentLogLines(loadConfig().XPlanePath, sinceOffset)
}

// GetXPlanePath returns the previously chosen X-Plane installation path,
// or "" if none has been chosen yet. Called by the frontend on load to
// prefill the Setup panel.
func (a *App) GetXPlanePath() string {
	return loadConfig().XPlanePath
}

// ChooseXPlanePath opens a native folder picker for the user to select
// their X-Plane installation, and remembers the choice (see config.go) so
// it only has to be done once. Returns nil (no error) if the user
// cancelled the dialog - that's not a failure, just nothing to do.
func (a *App) ChooseXPlanePath() (*ChooseXPlaneResult, error) {
	path, err := runtime.OpenDirectoryDialog(a.ctx, runtime.OpenDialogOptions{
		Title: "Select your X-Plane 12 installation folder",
	})
	if err != nil {
		return nil, err
	}
	if path == "" {
		return nil, nil
	}

	cfg := loadConfig()
	cfg.XPlanePath = path
	if err := saveConfig(cfg); err != nil {
		return nil, err
	}

	result := &ChooseXPlaneResult{Path: path}
	if !LooksLikeXPlaneRoot(path) {
		result.Warning = "This doesn't look like an X-Plane installation (no Resources/ folder found here) - double check before installing."
	}
	return result, nil
}

// InstallPlugin copies the plugin embedded in this companion binary (see
// plugin_install.go) into the previously chosen X-Plane installation's
// Resources/plugins/ folder. Bound to the frontend's "Install/Update
// Plugin" button.
func (a *App) InstallPlugin() error {
	path := loadConfig().XPlanePath
	if path == "" {
		return errors.New("choose your X-Plane folder first")
	}
	return InstallPlugin(path)
}

// GetAvailablePluginVersion returns the version of the plugin embedded in
// this companion build - i.e. what Install/Update Plugin would give you.
func (a *App) GetAvailablePluginVersion() string {
	return AvailablePluginVersion()
}

// CheckForUpdate asks GitHub for the latest companion app release and
// compares it against this build's own version (see updater.go's
// CheckForUpdate) - called once from the frontend on load, the same way
// refreshVersions() checks the plugin's own Installed/Available versions,
// not on the 1s status-poll ticker (an API call that often would be
// wasteful and risks GitHub's unauthenticated rate limit for no benefit -
// a new release doesn't appear more than a few times a year). Returns nil
// (no error) if already up to date or this is a local dev build.
func (a *App) CheckForUpdate() (*UpdateInfo, error) {
	return CheckForUpdate()
}

// ApplyUpdate downloads, verifies and applies the latest release (see
// updater.go's applyUpdate), then relaunches the freshly-updated
// executable as a new, detached process and quits this one - bound to
// the frontend's "Update & Restart" button. The relaunch is what
// actually starts running the new code: applyUpdate only swaps the file
// on disk out from under this still-running (old-code) process, it
// doesn't change what's already loaded into memory.
func (a *App) ApplyUpdate() error {
	if err := applyUpdate(); err != nil {
		return err
	}

	exe, err := os.Executable()
	if err != nil {
		// The update was applied successfully - only the automatic
		// relaunch failed. Don't report this as an update failure; the
		// user can just start the (now updated) app again themselves.
		return nil
	}
	_ = exec.Command(exe).Start()
	runtime.Quit(a.ctx)
	return nil
}

// GetInstalledPluginVersion returns the version of whatever plugin is
// currently on disk in the chosen X-Plane installation, or "" if no
// X-Plane folder has been chosen yet or nothing is installed there.
// Works without X-Plane running - reads version.txt straight off disk.
func (a *App) GetInstalledPluginVersion() string {
	path := loadConfig().XPlanePath
	if path == "" {
		return ""
	}
	return InstalledPluginVersion(path)
}
