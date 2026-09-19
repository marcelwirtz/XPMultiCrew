package main

import (
	"context"
	"errors"
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
	Formation         string          `json:"formation"`
	FormationCode     string          `json:"formationCode"`
	SharedCockpit     string          `json:"sharedCockpit"`
	SharedCockpitCode string          `json:"sharedCockpitCode"`
	Peers             []FormationPeer `json:"peers"`
	SimReady          bool            `json:"simReady"`
	RunningVersion    string          `json:"runningVersion"`
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
			Formation:         formation,
			FormationCode:     formationCode,
			SharedCockpit:     sharedCockpit,
			SharedCockpitCode: sharedCockpitCode,
			Peers:             a.plugin.FormationPeers(),
			SimReady:          a.plugin.SimReady(),
			RunningVersion:    a.plugin.RunningVersion(),
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
