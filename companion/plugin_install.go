package main

import (
	"embed"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The platform-specific plugin build (build/XPMultiCrew/ from the CMake
// build, i.e. the .xpl plus its Resources/ folder) gets staged here as
// embedded_plugin/XPMultiCrew/ before `wails build` runs - see
// .github/workflows/{ci,release}.yml and companion/.gitignore. This is
// what lets one companion download also install the plugin, instead of
// shipping the plugin as a separate zip the user has to unpack manually.
//
//go:embed all:embedded_plugin
var pluginAssets embed.FS

// pluginDirName is both the embedded root's single subdirectory and the
// name the plugin is installed under inside <X-Plane>/Resources/plugins/.
const pluginDirName = "XPMultiCrew"

// LooksLikeXPlaneRoot does a light sanity check - every real X-Plane
// install has a Resources/ folder at its root. Not a hard requirement
// (an unusual setup might differ), just a signal the frontend can warn
// on before the user commits to installing there.
func LooksLikeXPlaneRoot(path string) bool {
	info, err := os.Stat(filepath.Join(path, "Resources"))
	return err == nil && info.IsDir()
}

// InstallPlugin copies the embedded, platform-specific plugin build into
// <xplaneRoot>/Resources/plugins/XPMultiCrew/, replacing any existing
// install there.
func InstallPlugin(xplaneRoot string) error {
	return installPluginFrom(pluginAssets, "embedded_plugin/"+pluginDirName, xplaneRoot)
}

// AvailablePluginVersion returns the version of the plugin embedded in
// *this* companion build (i.e. what Install/Update Plugin would give
// you) - see plugin/CMakeLists.txt's version.txt generation, staged here
// the same way as the plugin binary itself. "unknown" if the embedded
// plugin doesn't have a version.txt (e.g. a local dev build where only
// the .gitkeep placeholder was ever staged, see companion/README.md).
func AvailablePluginVersion() string {
	return readVersionFile(pluginAssets, "embedded_plugin/"+pluginDirName+"/version.txt")
}

// InstalledPluginVersion returns the version of whatever plugin is
// currently sitting in <xplaneRoot>/Resources/plugins/XPMultiCrew/ - read
// straight off disk, so this works whether or not X-Plane (or the plugin)
// is actually running right now. "" (not "unknown") if nothing is
// installed there at all, so callers can tell "no install" from "an
// install with no version info".
func InstalledPluginVersion(xplaneRoot string) string {
	path := filepath.Join(xplaneRoot, "Resources", "plugins", pluginDirName, "version.txt")
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}

func readVersionFile(assets fs.FS, path string) string {
	data, err := fs.ReadFile(assets, path)
	if err != nil {
		return "unknown"
	}
	version := strings.TrimSpace(string(data))
	if version == "" {
		return "unknown"
	}
	return version
}

// installPluginFrom does the actual work, taking the embedded filesystem
// and root as parameters so the atomic-swap logic below is testable
// without needing a real compiled-in plugin (see plugin_install_test.go,
// which passes a fake fstest.MapFS instead).
//
// Never overwrites files in place: in-place-overwriting a currently-loaded
// .xpl crashed a real X-Plane instance once during this project's
// development, because the OS can't safely swap out a memory-mapped
// file's contents under it. Renaming a whole new directory into place
// instead leaves the old, still-mapped files' inodes alone until X-Plane
// itself releases them - same principle the plugin's manual deploy docs
// already recommend, just automated here.
func installPluginFrom(assets fs.FS, root, xplaneRoot string) error {
	pluginsDir := filepath.Join(xplaneRoot, "Resources", "plugins")
	if err := os.MkdirAll(pluginsDir, 0755); err != nil {
		return fmt.Errorf("could not create %s: %w", pluginsDir, err)
	}

	finalDir := filepath.Join(pluginsDir, pluginDirName)
	suffix := time.Now().UnixNano()
	tmpDir := filepath.Join(pluginsDir, fmt.Sprintf(".%s.installing-%d", pluginDirName, suffix))
	oldDir := filepath.Join(pluginsDir, fmt.Sprintf(".%s.old-%d", pluginDirName, suffix))

	if err := extractEmbedded(assets, root, tmpDir); err != nil {
		_ = os.RemoveAll(tmpDir)
		return fmt.Errorf("could not extract plugin files: %w", err)
	}

	if _, err := os.Stat(finalDir); err == nil {
		// rename() can't atomically *replace* a non-empty directory on
		// either POSIX or Windows, so move the previous install aside
		// first and only delete it once the new one is safely in place.
		if err := os.Rename(finalDir, oldDir); err != nil {
			_ = os.RemoveAll(tmpDir)
			return fmt.Errorf("could not move aside existing install: %w", err)
		}
	}

	if err := os.Rename(tmpDir, finalDir); err != nil {
		_ = os.Rename(oldDir, finalDir) // best-effort: restore the previous install rather than leaving neither
		_ = os.RemoveAll(tmpDir)
		return fmt.Errorf("could not move new plugin into place: %w", err)
	}

	_ = os.RemoveAll(oldDir) // best-effort cleanup, not safety-critical
	return nil
}

// extractEmbedded writes every file under root (in assets) to destDir,
// preserving relative structure. destDir must not already exist.
func extractEmbedded(assets fs.FS, root, destDir string) error {
	return fs.WalkDir(assets, root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		target := filepath.Join(destDir, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0755)
		}
		data, err := fs.ReadFile(assets, path)
		if err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return err
		}
		return os.WriteFile(target, data, 0644)
	})
}
