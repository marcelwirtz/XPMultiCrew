package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

// companionConfig persists small local settings across app restarts - the
// chosen X-Plane installation path (so the user only has to pick it once,
// see app.go's ChooseXPlanePath) and a small address book of previously-
// used rendezvous server addresses (see app.go's GetSavedServers/
// SaveServer/DeleteSavedServer).
type companionConfig struct {
	XPlanePath   string        `json:"xplanePath"`
	SavedServers []SavedServer `json:"savedServers"`
	// LastFormation/LastSharedCockpit remember the most recent session
	// this side was actually part of, so the companion app can
	// automatically rejoin it once the plugin next reports SIM_READY
	// (see app.go's maybeAutoRejoin) - covers X-Plane/the plugin
	// restarting (crash, relaunch), not just the companion app itself
	// restarting (the plugin's own in-memory reconnect logic, see
	// plugin_main.cpp's ReconnectGate, already survives that case on its
	// own without any help from here). nil means "nothing to rejoin".
	LastFormation     *PersistedSession `json:"lastFormation,omitempty"`
	LastSharedCockpit *PersistedSession `json:"lastSharedCockpit,omitempty"`

	// Settings pushed to the plugin via SET_PREFS (see app.go's
	// pluginPrefs). Pointers so an absent key (config from an older
	// version) means "default on" rather than false.
	Callsign   string `json:"callsign,omitempty"`
	ShowLabels *bool  `json:"showLabels,omitempty"`
	EnvSync    *bool  `json:"envSync,omitempty"`
}

// pluginPrefs returns the settings to push to the plugin, with defaults
// filled in.
func (c companionConfig) pluginPrefs() PluginPrefs {
	p := PluginPrefs{Callsign: c.Callsign, ShowLabels: true, EnvSync: true}
	if c.ShowLabels != nil {
		p.ShowLabels = *c.ShowLabels
	}
	if c.EnvSync != nil {
		p.EnvSync = *c.EnvSync
	}
	return p
}

// PersistedSession is one remembered Formation/Shared Cockpit session -
// just enough to repeat the equivalent of a JOIN_SESSION/
// START_SHARED_COCKPIT CLIENT control command, see app.go's
// maybeAutoRejoin.
type PersistedSession struct {
	Server string `json:"server"`
	Code   string `json:"code"`
	// Role is only ever set (and only ever meaningful) for
	// LastSharedCockpit - "CLIENT" (the only value maybeAutoRejoin acts
	// on) or "MASTER". A persisted MASTER session is deliberately never
	// auto-rejoined: the START_SHARED_COCKPIT protocol has no way to tell
	// the plugin "recreate specifically THIS code as MASTER" - a fresh
	// MASTER start always gets a brand new code from the server (see
	// control_listener.h's grammar), which the co-pilot wouldn't know
	// without being told again anyway, so there's nothing meaningful to
	// silently resume. Still recorded (rather than not persisting a
	// MASTER session at all) purely so a future feature has the
	// information available, should the protocol ever grow a "rejoin as
	// MASTER" path.
	Role string `json:"role,omitempty"`
	// IsSpectator - only ever set for LastFormation (Shared Cockpit has
	// no spectator mode, see control_listener.h's CREATE_SESSION/
	// JOIN_SESSION SPECTATOR token comment) - carried through so
	// maybeAutoRejoin resumes watching, not flying, if that's how the
	// user joined originally.
	IsSpectator bool `json:"isSpectator,omitempty"`
}

// SavedServer is one address-book entry - just the rendezvous server
// address (host:port). Session *codes* are deliberately not part of this:
// they're one-time/per-session, generated fresh by whoever creates a
// session, so there's nothing recurring about them worth saving - unlike
// the server address, which is typically the same VPS/host every time.
type SavedServer struct {
	Label    string `json:"label"`
	HostPort string `json:"hostPort"`
}

// userConfigDir is a var (not called directly as os.UserConfigDir) so
// config_test.go can point it at a t.TempDir() instead of the real
// per-user config directory.
var userConfigDir = os.UserConfigDir

func configFilePath() (string, error) {
	dir, err := userConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "xpmulticrew-companion", "config.json"), nil
}

// loadConfig returns the zero-value config (no error) if nothing has been
// saved yet or the file can't be read - callers treat an empty
// XPlanePath as "not set yet", not as a failure.
func loadConfig() companionConfig {
	path, err := configFilePath()
	if err != nil {
		return companionConfig{}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return companionConfig{}
	}
	var cfg companionConfig
	_ = json.Unmarshal(data, &cfg)
	return cfg
}

// persistLastFormation/persistLastSharedCockpit update just their one
// field, leaving the rest of the config (X-Plane path, saved servers, the
// other session) untouched - session is nil to clear it (e.g. on an
// explicit Disconnect, see app.go).
func persistLastFormation(session *PersistedSession) error {
	cfg := loadConfig()
	cfg.LastFormation = session
	return saveConfig(cfg)
}

func persistLastSharedCockpit(session *PersistedSession) error {
	cfg := loadConfig()
	cfg.LastSharedCockpit = session
	return saveConfig(cfg)
}

func saveConfig(cfg companionConfig) error {
	path, err := configFilePath()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0644)
}
