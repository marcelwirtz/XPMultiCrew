package main

import (
	"os"
	"path/filepath"
	"testing"
	"testing/fstest"
)

// fakePluginAssets stands in for the real compiled-in embed.FS, so these
// tests exercise the actual atomic-swap logic without needing a real
// built plugin - see installPluginFrom's comment for why that matters.
func fakePluginAssets() fstest.MapFS {
	return fstest.MapFS{
		"embedded_plugin/XPMultiCrew/lin.xpl":                     {Data: []byte("fake plugin binary v1")},
		"embedded_plugin/XPMultiCrew/Resources/CSL/Generic/x.txt": {Data: []byte("resource file")},
	}
}

func TestInstallPluginFromFreshInstall(t *testing.T) {
	xplaneRoot := t.TempDir()

	if err := installPluginFrom(fakePluginAssets(), "embedded_plugin/XPMultiCrew", xplaneRoot); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	pluginFile := filepath.Join(xplaneRoot, "Resources", "plugins", "XPMultiCrew", "lin.xpl")
	data, err := os.ReadFile(pluginFile)
	if err != nil {
		t.Fatalf("expected plugin file at %s: %v", pluginFile, err)
	}
	if string(data) != "fake plugin binary v1" {
		t.Fatalf("unexpected plugin file contents: %q", data)
	}

	resourceFile := filepath.Join(xplaneRoot, "Resources", "plugins", "XPMultiCrew", "Resources", "CSL", "Generic", "x.txt")
	if _, err := os.Stat(resourceFile); err != nil {
		t.Fatalf("expected resource file at %s: %v", resourceFile, err)
	}
}

func TestInstallPluginFromReplacesExistingInstall(t *testing.T) {
	xplaneRoot := t.TempDir()
	oldPluginDir := filepath.Join(xplaneRoot, "Resources", "plugins", "XPMultiCrew")
	if err := os.MkdirAll(oldPluginDir, 0755); err != nil {
		t.Fatalf("setup failed: %v", err)
	}
	if err := os.WriteFile(filepath.Join(oldPluginDir, "lin.xpl"), []byte("old version"), 0644); err != nil {
		t.Fatalf("setup failed: %v", err)
	}
	// A leftover file from a previous, differently-shaped install - must
	// not survive the swap (this is a fresh directory, not a merge).
	if err := os.WriteFile(filepath.Join(oldPluginDir, "stale_leftover.txt"), []byte("x"), 0644); err != nil {
		t.Fatalf("setup failed: %v", err)
	}

	if err := installPluginFrom(fakePluginAssets(), "embedded_plugin/XPMultiCrew", xplaneRoot); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	data, err := os.ReadFile(filepath.Join(oldPluginDir, "lin.xpl"))
	if err != nil {
		t.Fatalf("expected plugin file to still exist: %v", err)
	}
	if string(data) != "fake plugin binary v1" {
		t.Fatalf("expected old install to be replaced, got: %q", data)
	}
	if _, err := os.Stat(filepath.Join(oldPluginDir, "stale_leftover.txt")); !os.IsNotExist(err) {
		t.Fatalf("expected stale leftover file to be gone, stat error: %v", err)
	}

	// No .installing-/.old- temp directories should be left behind.
	entries, err := os.ReadDir(filepath.Join(xplaneRoot, "Resources", "plugins"))
	if err != nil {
		t.Fatalf("failed to list plugins dir: %v", err)
	}
	if len(entries) != 1 || entries[0].Name() != "XPMultiCrew" {
		names := make([]string, len(entries))
		for i, e := range entries {
			names[i] = e.Name()
		}
		t.Fatalf("expected only XPMultiCrew/ to remain, found: %v", names)
	}
}

func TestReadVersionFileReturnsUnknownWhenMissing(t *testing.T) {
	assets := fstest.MapFS{} // no version.txt at all
	if got := readVersionFile(assets, "embedded_plugin/XPMultiCrew/version.txt"); got != "unknown" {
		t.Fatalf("expected \"unknown\", got %q", got)
	}
}

func TestReadVersionFileReturnsUnknownWhenEmpty(t *testing.T) {
	assets := fstest.MapFS{
		"embedded_plugin/XPMultiCrew/version.txt": {Data: []byte("")},
	}
	if got := readVersionFile(assets, "embedded_plugin/XPMultiCrew/version.txt"); got != "unknown" {
		t.Fatalf("expected \"unknown\" for an empty file, got %q", got)
	}
}

func TestReadVersionFileTrimsWhitespace(t *testing.T) {
	assets := fstest.MapFS{
		"embedded_plugin/XPMultiCrew/version.txt": {Data: []byte("v0.1.0\n")},
	}
	if got := readVersionFile(assets, "embedded_plugin/XPMultiCrew/version.txt"); got != "v0.1.0" {
		t.Fatalf("expected \"v0.1.0\", got %q", got)
	}
}

func TestInstalledPluginVersionEmptyWhenNotInstalled(t *testing.T) {
	xplaneRoot := t.TempDir()
	if got := InstalledPluginVersion(xplaneRoot); got != "" {
		t.Fatalf("expected \"\" when nothing is installed, got %q", got)
	}
}

func TestInstalledPluginVersionReadsFromDisk(t *testing.T) {
	xplaneRoot := t.TempDir()
	pluginDir := filepath.Join(xplaneRoot, "Resources", "plugins", "XPMultiCrew")
	if err := os.MkdirAll(pluginDir, 0755); err != nil {
		t.Fatalf("setup failed: %v", err)
	}
	if err := os.WriteFile(filepath.Join(pluginDir, "version.txt"), []byte("v0.1.0\n"), 0644); err != nil {
		t.Fatalf("setup failed: %v", err)
	}
	if got := InstalledPluginVersion(xplaneRoot); got != "v0.1.0" {
		t.Fatalf("expected \"v0.1.0\", got %q", got)
	}
}

func TestInstallPluginFromCarriesVersionFile(t *testing.T) {
	xplaneRoot := t.TempDir()
	assets := fstest.MapFS{
		"embedded_plugin/XPMultiCrew/lin.xpl":     {Data: []byte("fake plugin binary")},
		"embedded_plugin/XPMultiCrew/version.txt": {Data: []byte("v0.2.0")},
	}
	if err := installPluginFrom(assets, "embedded_plugin/XPMultiCrew", xplaneRoot); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := InstalledPluginVersion(xplaneRoot); got != "v0.2.0" {
		t.Fatalf("expected InstalledPluginVersion to pick up the freshly-installed version.txt, got %q", got)
	}
}

func TestLooksLikeXPlaneRoot(t *testing.T) {
	root := t.TempDir()
	if LooksLikeXPlaneRoot(root) {
		t.Fatal("empty directory should not look like an X-Plane root")
	}
	if err := os.Mkdir(filepath.Join(root, "Resources"), 0755); err != nil {
		t.Fatalf("setup failed: %v", err)
	}
	if !LooksLikeXPlaneRoot(root) {
		t.Fatal("directory with a Resources/ subfolder should look like an X-Plane root")
	}
}
