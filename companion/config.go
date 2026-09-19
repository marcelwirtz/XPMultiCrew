package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

// companionConfig persists small local settings across app restarts -
// currently just the chosen X-Plane installation path, so the user only
// has to pick it once (see app.go's ChooseXPlanePath).
type companionConfig struct {
	XPlanePath string `json:"xplanePath"`
}

func configFilePath() (string, error) {
	dir, err := os.UserConfigDir()
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
