package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

// Shared Cockpit dataref profiles - see the plugin's
// shared_cockpit/shared_cockpit_config.h for the file format and lookup
// order. The editor only ever writes the user override
// (<X-Plane>/XPMultiCrew_shared_cockpit_profiles/<ICAO>.txt), which the
// plugin prefers over the profile bundled with it; bundled profiles are
// read-only templates.
const userProfilesDirName = "XPMultiCrew_shared_cockpit_profiles"

var profileCategories = map[string]bool{"engine": true, "avionics": true, "systems": true}

var icaoPattern = regexp.MustCompile(`^[A-Z0-9]{2,8}$`)

// ProfileEntry is one DATAREF line.
type ProfileEntry struct {
	Name     string `json:"name"`
	Stream   bool   `json:"stream"`
	Category string `json:"category"`
	// Filled on load from X-Plane's DataRefs.txt: "" if fine, otherwise why
	// the plugin will likely skip or be unable to write it.
	Warning string `json:"warning,omitempty"`
}

// ProfileInfo is one row of the profile list.
type ProfileInfo struct {
	ICAO       string `json:"icao"`
	HasUser    bool   `json:"hasUser"`
	HasBundled bool   `json:"hasBundled"`
}

// ProfileData is a profile opened in the editor.
type ProfileData struct {
	ICAO string `json:"icao"`
	// "user" (your override), "bundled" (the plugin's default, saving
	// creates an override) or "new" (neither exists yet).
	Source  string         `json:"source"`
	Entries []ProfileEntry `json:"entries"`
	// False when X-Plane's DataRefs.txt couldn't be read, i.e. the
	// warnings above aren't meaningful.
	Validated bool `json:"validated"`
}

func userProfilesDir(xplaneRoot string) string {
	return filepath.Join(xplaneRoot, userProfilesDirName)
}

func bundledProfilesDir(xplaneRoot string) string {
	return filepath.Join(xplaneRoot, "Resources", "plugins", pluginDirName, "Resources", "shared_cockpit_profiles")
}

func normalizeIcao(icao string) (string, error) {
	icao = strings.ToUpper(strings.TrimSpace(icao))
	if !icaoPattern.MatchString(icao) {
		return "", fmt.Errorf("%q is not a valid ICAO type (2-8 letters/digits, e.g. C172)", icao)
	}
	return icao, nil
}

func listProfileNames(dir string) map[string]bool {
	out := map[string]bool{}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return out
	}
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.EqualFold(filepath.Ext(name), ".txt") {
			continue
		}
		icao := strings.ToUpper(strings.TrimSuffix(name, filepath.Ext(name)))
		if icaoPattern.MatchString(icao) {
			out[icao] = true
		}
	}
	return out
}

func listProfiles(xplaneRoot string) []ProfileInfo {
	user := listProfileNames(userProfilesDir(xplaneRoot))
	bundled := listProfileNames(bundledProfilesDir(xplaneRoot))
	all := map[string]bool{}
	for k := range user {
		all[k] = true
	}
	for k := range bundled {
		all[k] = true
	}
	out := make([]ProfileInfo, 0, len(all))
	for icao := range all {
		out = append(out, ProfileInfo{ICAO: icao, HasUser: user[icao], HasBundled: bundled[icao]})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ICAO < out[j].ICAO })
	return out
}

// parseProfile mirrors the plugin's LoadSharedCockpitConfig: DATAREF
// <path> [STREAM] [CATEGORY <name>] in any order, comments/blank lines and
// unknown tokens ignored, unknown categories fall back to systems.
func parseProfile(text string) []ProfileEntry {
	entries := []ProfileEntry{}
	for _, raw := range strings.Split(text, "\n") {
		line := raw
		if i := strings.Index(line, "#"); i >= 0 {
			line = line[:i]
		}
		fields := strings.Fields(line)
		if len(fields) < 2 || fields[0] != "DATAREF" {
			continue
		}
		e := ProfileEntry{Name: fields[1], Category: "systems"}
		for i := 2; i < len(fields); i++ {
			switch fields[i] {
			case "STREAM":
				e.Stream = true
			case "CATEGORY":
				if i+1 < len(fields) {
					i++
					if profileCategories[fields[i]] {
						e.Category = fields[i]
					}
				}
			}
		}
		entries = append(entries, e)
	}
	return entries
}

func formatProfile(icao string, entries []ProfileEntry) string {
	var b strings.Builder
	fmt.Fprintf(&b, "# Shared Cockpit profile for %s - edited with the XPMultiCrew companion app.\n", icao)
	b.WriteString("# Format: DATAREF <path> [STREAM] [CATEGORY engine|avionics|systems]\n\n")
	for _, e := range entries {
		b.WriteString("DATAREF ")
		b.WriteString(e.Name)
		if e.Stream {
			b.WriteString(" STREAM")
		}
		b.WriteString(" CATEGORY ")
		b.WriteString(e.Category)
		b.WriteString("\n")
	}
	return b.String()
}

// datarefIndex maps a dataref name to whether X-Plane says it's writable,
// parsed from <X-Plane>/Resources/plugins/DataRefs.txt (a few MB, so
// cached until the file changes).
type datarefIndex struct {
	path     string
	modTime  time.Time
	writable map[string]bool
}

var (
	datarefCacheMu sync.Mutex
	datarefCache   *datarefIndex
)

func loadDatarefIndex(xplaneRoot string) map[string]bool {
	path := filepath.Join(xplaneRoot, "Resources", "plugins", "DataRefs.txt")
	st, err := os.Stat(path)
	if err != nil {
		return nil
	}
	datarefCacheMu.Lock()
	defer datarefCacheMu.Unlock()
	if datarefCache != nil && datarefCache.path == path && datarefCache.modTime.Equal(st.ModTime()) {
		return datarefCache.writable
	}
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	writable := map[string]bool{}
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 64*1024), 1024*1024)
	for scanner.Scan() {
		fields := strings.Split(scanner.Text(), "\t")
		if len(fields) < 3 || !strings.HasPrefix(fields[0], "sim/") {
			continue
		}
		writable[fields[0]] = strings.TrimSpace(fields[2]) == "y"
	}
	datarefCache = &datarefIndex{path: path, modTime: st.ModTime(), writable: writable}
	return writable
}

func annotate(entries []ProfileEntry, index map[string]bool) {
	if index == nil {
		return
	}
	for i := range entries {
		name := entries[i].Name
		if !strings.HasPrefix(name, "sim/") {
			entries[i].Warning = "" // add-on dataref - can't be checked, only works if that add-on is loaded
			continue
		}
		w, ok := index[name]
		switch {
		case !ok:
			entries[i].Warning = "not in X-Plane's DataRefs.txt - typo?"
		case !w:
			entries[i].Warning = "read-only in X-Plane - can't be synced"
		default:
			entries[i].Warning = ""
		}
	}
}

func loadProfile(xplaneRoot, icao string) (ProfileData, error) {
	icao, err := normalizeIcao(icao)
	if err != nil {
		return ProfileData{}, err
	}
	data := ProfileData{ICAO: icao, Source: "new", Entries: []ProfileEntry{}}
	for _, candidate := range []struct {
		dir, source string
	}{{userProfilesDir(xplaneRoot), "user"}, {bundledProfilesDir(xplaneRoot), "bundled"}} {
		text, err := os.ReadFile(filepath.Join(candidate.dir, icao+".txt"))
		if err == nil {
			data.Source = candidate.source
			data.Entries = parseProfile(string(text))
			break
		}
	}
	index := loadDatarefIndex(xplaneRoot)
	data.Validated = index != nil
	annotate(data.Entries, index)
	return data, nil
}

func saveProfile(xplaneRoot, icao string, entries []ProfileEntry) (ProfileData, error) {
	icao, err := normalizeIcao(icao)
	if err != nil {
		return ProfileData{}, err
	}
	clean := make([]ProfileEntry, 0, len(entries))
	seen := map[string]bool{}
	for _, e := range entries {
		e.Name = strings.TrimSpace(e.Name)
		if e.Name == "" {
			continue
		}
		if strings.ContainsAny(e.Name, " \t#") {
			return ProfileData{}, fmt.Errorf("dataref %q contains spaces or '#'", e.Name)
		}
		if seen[e.Name] {
			continue
		}
		seen[e.Name] = true
		if !profileCategories[e.Category] {
			e.Category = "systems"
		}
		e.Warning = ""
		clean = append(clean, e)
	}
	dir := userProfilesDir(xplaneRoot)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return ProfileData{}, err
	}
	final := filepath.Join(dir, icao+".txt")
	tmp := final + ".tmp"
	if err := os.WriteFile(tmp, []byte(formatProfile(icao, clean)), 0644); err != nil {
		return ProfileData{}, err
	}
	if err := os.Rename(tmp, final); err != nil {
		_ = os.Remove(tmp)
		return ProfileData{}, err
	}
	return loadProfile(xplaneRoot, icao)
}

// deleteUserProfile removes your override, so the bundled profile (if any)
// applies again.
func deleteUserProfile(xplaneRoot, icao string) error {
	icao, err := normalizeIcao(icao)
	if err != nil {
		return err
	}
	err = os.Remove(filepath.Join(userProfilesDir(xplaneRoot), icao+".txt"))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	return err
}
