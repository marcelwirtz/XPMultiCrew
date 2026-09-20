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
