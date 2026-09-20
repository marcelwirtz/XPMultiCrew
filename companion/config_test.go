package main

import (
	"reflect"
	"testing"
)

// useTempConfigDir points userConfigDir (config.go) at a fresh t.TempDir()
// for the duration of the test, so these tests never touch the real
// per-user config file on the machine running them.
func useTempConfigDir(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	old := userConfigDir
	userConfigDir = func() (string, error) { return dir, nil }
	t.Cleanup(func() { userConfigDir = old })
}

func TestLoadConfigDefaultsToZeroValue(t *testing.T) {
	useTempConfigDir(t)

	cfg := loadConfig()
	if cfg.XPlanePath != "" {
		t.Fatalf("expected empty XPlanePath before anything is saved, got %q", cfg.XPlanePath)
	}
	if len(cfg.SavedServers) != 0 {
		t.Fatalf("expected no saved servers before anything is saved, got %+v", cfg.SavedServers)
	}
}

func TestSaveConfigRoundTrips(t *testing.T) {
	useTempConfigDir(t)

	want := companionConfig{
		XPlanePath:   "/path/to/xplane",
		SavedServers: []SavedServer{{Label: "Home VPS", HostPort: "rendezvous.example.com:45000"}},
	}
	if err := saveConfig(want); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	got := loadConfig()
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("round trip mismatch: got %+v, want %+v", got, want)
	}
}

func TestGetSavedServersDefaultsToEmptyNotNil(t *testing.T) {
	useTempConfigDir(t)

	app := NewApp()
	servers := app.GetSavedServers()
	if servers == nil {
		t.Fatal("GetSavedServers() should never return nil (marshals to JSON null, not [])")
	}
	if len(servers) != 0 {
		t.Fatalf("expected no saved servers before anything is saved, got %+v", servers)
	}
}

func TestSaveServerAddsNewEntry(t *testing.T) {
	useTempConfigDir(t)

	app := NewApp()
	if err := app.SaveServer("Home VPS", "rendezvous.example.com:45000"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	got := app.GetSavedServers()
	want := []SavedServer{{Label: "Home VPS", HostPort: "rendezvous.example.com:45000"}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("unexpected saved servers: got %+v, want %+v", got, want)
	}
}

func TestSaveServerUpdatesExistingLabelInsteadOfDuplicating(t *testing.T) {
	useTempConfigDir(t)

	app := NewApp()
	_ = app.SaveServer("Home VPS", "old.example.com:45000")
	_ = app.SaveServer("Home VPS", "new.example.com:45000")

	got := app.GetSavedServers()
	want := []SavedServer{{Label: "Home VPS", HostPort: "new.example.com:45000"}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("expected the existing label to be updated in place, not duplicated: got %+v", got)
	}
}

func TestSaveServerRequiresLabelAndAddress(t *testing.T) {
	useTempConfigDir(t)

	app := NewApp()
	if err := app.SaveServer("", "rendezvous.example.com:45000"); err == nil {
		t.Fatal("expected an error when label is blank")
	}
	if err := app.SaveServer("Home VPS", ""); err == nil {
		t.Fatal("expected an error when server address is blank")
	}
}

func TestDeleteSavedServerRemovesEntry(t *testing.T) {
	useTempConfigDir(t)

	app := NewApp()
	_ = app.SaveServer("Home VPS", "rendezvous.example.com:45000")
	_ = app.SaveServer("Backup VPS", "backup.example.com:45000")

	if err := app.DeleteSavedServer("Home VPS"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	got := app.GetSavedServers()
	want := []SavedServer{{Label: "Backup VPS", HostPort: "backup.example.com:45000"}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("unexpected saved servers after delete: got %+v, want %+v", got, want)
	}
}

func TestDeleteSavedServerIsNoOpForUnknownLabel(t *testing.T) {
	useTempConfigDir(t)

	app := NewApp()
	_ = app.SaveServer("Home VPS", "rendezvous.example.com:45000")

	if err := app.DeleteSavedServer("Nonexistent"); err != nil {
		t.Fatalf("expected no error deleting an unknown label, got: %v", err)
	}
	if len(app.GetSavedServers()) != 1 {
		t.Fatal("expected the existing entry to be untouched")
	}
}

func TestSavedServersPersistAcrossLoads(t *testing.T) {
	useTempConfigDir(t)

	app := NewApp()
	_ = app.SaveServer("Home VPS", "rendezvous.example.com:45000")

	// Simulate a restart: load fresh from disk rather than reusing any
	// in-memory state (there is none held by App - loadConfig() reads the
	// file every time, this just checks that assumption holds).
	got := loadConfig().SavedServers
	want := []SavedServer{{Label: "Home VPS", HostPort: "rendezvous.example.com:45000"}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("unexpected persisted servers: got %+v, want %+v", got, want)
	}
}
