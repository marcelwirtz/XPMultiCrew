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

// ProfileEntry is one DATAREF or COMMAND line.
type ProfileEntry struct {
	// "dataref" (a value kept in sync) or "command" (a button press
	// mirrored to the other cockpit, see the plugin's command_sync.h).
	Kind     string `json:"kind"`
	Name     string `json:"name"`
	Stream   bool   `json:"stream"`
	// OUTPUT: a result of the pilot flying's simulation (ENGN_running ...),
	// only displayed in the co-pilot's cockpit - see the plugin's sync_policy.h.
	Output   bool   `json:"output"`
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

// Flight controls the plugin never syncs as DATAREF lines - they come from
// the pilot flying as a stream. Mirrors the plugin's IsFlightControlDataref.
var flightControlDatarefs = map[string]bool{
	"sim/joystick/yoke_pitch_ratio":                             true,
	"sim/joystick/yoke_roll_ratio":                              true,
	"sim/joystick/yoke_heading_ratio":                           true,
	"sim/cockpit2/controls/yoke_pitch_ratio":                    true,
	"sim/cockpit2/controls/yoke_roll_ratio":                     true,
	"sim/cockpit2/controls/yoke_heading_ratio":                  true,
	"sim/cockpit2/controls/left_brake_ratio":                    true,
	"sim/cockpit2/controls/right_brake_ratio":                   true,
	"sim/cockpit2/engine/actuators/throttle_ratio":              true,
	"sim/cockpit2/engine/actuators/throttle_ratio_all":          true,
	"sim/cockpit2/engine/actuators/throttle_beta_rev_ratio":     true,
	"sim/cockpit2/engine/actuators/throttle_beta_rev_ratio_all": true,
	"sim/cockpit2/engine/actuators/throttle_jet_rev_ratio":      true,
	"sim/cockpit2/engine/actuators/throttle_jet_rev_ratio_all":  true,
	"sim/cockpit2/engine/actuators/mixture_ratio":               true,
	"sim/cockpit2/engine/actuators/mixture_ratio_all":           true,
	"sim/cockpit2/engine/actuators/prop_ratio":                  true,
	"sim/cockpit2/engine/actuators/prop_ratio_all":              true,
	"sim/flightmodel/engine/ENGN_thro":                          true,
	"sim/flightmodel/engine/ENGN_thro_use":                      true,
	"sim/flightmodel/engine/ENGN_mixt":                          true,
	"sim/flightmodel/engine/ENGN_prop":                          true,
}

func isFlightControlDataref(name string) bool {
	if i := strings.Index(name, "["); i >= 0 {
		name = name[:i]
	}
	return flightControlDatarefs[name]
}

// parseProfile mirrors the plugin's LoadSharedCockpitConfig: DATAREF
// <path> [STREAM] [OUTPUT] [CATEGORY <name>] in any order, comments/blank
// lines and unknown tokens ignored, unknown categories fall back to systems.
func parseProfile(text string) []ProfileEntry {
	entries := []ProfileEntry{}
	for _, raw := range strings.Split(text, "\n") {
		line := raw
		if i := strings.Index(line, "#"); i >= 0 {
			line = line[:i]
		}
		fields := strings.Fields(line)
		if len(fields) < 2 || (fields[0] != "DATAREF" && fields[0] != "COMMAND") {
			continue
		}
		e := ProfileEntry{Kind: "dataref", Name: fields[1], Category: "systems"}
		if fields[0] == "COMMAND" {
			e.Kind = "command"
		}
		for i := 2; i < len(fields); i++ {
			switch fields[i] {
			case "STREAM":
				e.Stream = e.Kind == "dataref"
			case "OUTPUT":
				e.Output = e.Kind == "dataref"
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
	b.WriteString("# Format: DATAREF <path> [STREAM] [OUTPUT] [CATEGORY engine|avionics|systems]\n")
	b.WriteString("#         COMMAND <command> [CATEGORY engine|avionics|systems]\n\n")
	for _, e := range entries {
		if e.Kind == "command" {
			b.WriteString("COMMAND ")
		} else {
			b.WriteString("DATAREF ")
		}
		b.WriteString(e.Name)
		if e.Stream && e.Kind != "command" {
			b.WriteString(" STREAM")
		}
		if e.Output && e.Kind != "command" {
			b.WriteString(" OUTPUT")
		}
		b.WriteString(" CATEGORY ")
		b.WriteString(e.Category)
		b.WriteString("\n")
	}
	return b.String()
}

// DatarefInfo is one DataRefs.txt entry - also what SearchDatarefs returns
// to the profile editor.
type DatarefInfo struct {
	Kind        string `json:"kind"` // "dataref" or "command"
	Name        string `json:"name"`
	Type        string `json:"type"`
	Writable    bool   `json:"writable"`
	Units       string `json:"units,omitempty"`
	Description string `json:"description,omitempty"`
	// Suggested profile category for it, see suggestCategory.
	Category string `json:"category"`
}

// datarefIndex holds <X-Plane>/Resources/plugins/DataRefs.txt (a few MB,
// so parsed once and cached until the file changes).
type datarefIndex struct {
	path     string
	modTime  time.Time
	writable map[string]bool
	byName   map[string]*DatarefInfo
	all      []DatarefInfo // file order
}

var (
	datarefCacheMu sync.Mutex
	datarefCache   *datarefIndex
)

func loadDatarefIndex(xplaneRoot string) map[string]bool {
	idx := loadFullDatarefIndex(xplaneRoot)
	if idx == nil {
		return nil
	}
	return idx.writable
}

func loadFullDatarefIndex(xplaneRoot string) *datarefIndex {
	path := filepath.Join(xplaneRoot, "Resources", "plugins", "DataRefs.txt")
	st, err := os.Stat(path)
	if err != nil {
		return nil
	}
	datarefCacheMu.Lock()
	defer datarefCacheMu.Unlock()
	if datarefCache != nil && datarefCache.path == path && datarefCache.modTime.Equal(st.ModTime()) {
		return datarefCache
	}
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	idx := &datarefIndex{path: path, modTime: st.ModTime(), writable: map[string]bool{}, byName: map[string]*DatarefInfo{}}
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 64*1024), 1024*1024)
	for scanner.Scan() {
		// name \t type \t writable(y/n) \t units \t description
		fields := strings.Split(scanner.Text(), "\t")
		if len(fields) < 3 || !strings.HasPrefix(fields[0], "sim/") {
			continue
		}
		info := DatarefInfo{Kind: "dataref", Name: fields[0], Type: strings.TrimSpace(fields[1]), Writable: strings.TrimSpace(fields[2]) == "y"}
		if len(fields) > 3 {
			info.Units = strings.TrimSpace(fields[3])
		}
		if len(fields) > 4 {
			info.Description = strings.TrimSpace(strings.Join(fields[4:], " "))
		}
		info.Category = suggestCategory(info.Name, info.Description)
		idx.writable[info.Name] = info.Writable
		idx.all = append(idx.all, info)
	}
	for i := range idx.all {
		idx.byName[idx.all[i].Name] = &idx.all[i]
	}
	datarefCache = idx
	return idx
}

// suggestCategory guesses the profile category from a dataref's path and
// description - the editor pre-selects it, the user can still change it.
func suggestCategory(name, description string) string {
	text := strings.ToLower(name + " " + description)
	for _, kw := range []string{"radios", "radio", "transponder", "autopilot", "/gps", "audio", "com1", "com2",
		"nav1", "nav2", "adf", "obs", "dme", "fms", "g1000", "avionics"} {
		if strings.Contains(text, kw) {
			return "avionics"
		}
	}
	for _, kw := range []string{"engine", "fuel", "mixture", "throttle", "prop", "ignition", "magneto", "primer",
		"starter", "carb", "cowl", "igniter"} {
		if strings.Contains(text, kw) {
			return "engine"
		}
	}
	return "systems"
}

// searchDatarefs finds writable DataRefs.txt entries whose name or
// description contains every word of the query (case-insensitive). Name
// hits rank before description-only hits, sim/cockpit2 (the modern,
// recommended paths) before the rest.
func searchDatarefs(xplaneRoot, query string, limit int) []DatarefInfo {
	idx := loadFullDatarefIndex(xplaneRoot)
	words := strings.Fields(strings.ToLower(query))
	if idx == nil || len(words) == 0 {
		return []DatarefInfo{}
	}
	type hit struct {
		info  DatarefInfo
		score int
	}
	hits := []hit{}
	candidates := idx.all
	for _, c := range loadCommandIndex(xplaneRoot).all {
		if isCommandCandidate(c.Name) {
			candidates = append(candidates, c)
		}
	}
	for _, info := range candidates {
		if !info.Writable || (info.Kind == "dataref" && !isProfileCandidate(info.Name)) {
			continue
		}
		name := strings.ToLower(info.Name)
		desc := strings.ToLower(info.Description)
		score := 0
		matched := true
		for _, w := range words {
			switch {
			case strings.Contains(name, w):
				score += 2
			case strings.Contains(desc, w):
				score++
			default:
				matched = false
			}
			if !matched {
				break
			}
		}
		if !matched {
			continue
		}
		if strings.HasPrefix(info.Name, "sim/cockpit2/") {
			score++
		}
		if strings.Contains(info.Description, "REPLACED") || strings.Contains(strings.ToLower(info.Description), "deprecated") {
			score -= 3
		}
		hits = append(hits, hit{info, score})
	}
	sort.SliceStable(hits, func(i, j int) bool { return hits[i].score > hits[j].score })
	if len(hits) > limit {
		hits = hits[:limit]
	}
	out := make([]DatarefInfo, len(hits))
	for i, h := range hits {
		out[i] = h.info
	}
	return out
}

// commandIndex is X-Plane's Resources/plugins/Commands.txt ("<name>
// <spaces> <description>"), cached like the dataref index.
type commandIndex struct {
	path    string
	modTime time.Time
	byName  map[string]string
	all     []DatarefInfo
}

var (
	commandCacheMu sync.Mutex
	commandCache   *commandIndex
)

func loadCommandIndex(xplaneRoot string) *commandIndex {
	empty := &commandIndex{byName: nil}
	path := filepath.Join(xplaneRoot, "Resources", "plugins", "Commands.txt")
	st, err := os.Stat(path)
	if err != nil {
		return empty
	}
	commandCacheMu.Lock()
	defer commandCacheMu.Unlock()
	if commandCache != nil && commandCache.path == path && commandCache.modTime.Equal(st.ModTime()) {
		return commandCache
	}
	f, err := os.Open(path)
	if err != nil {
		return empty
	}
	defer f.Close()
	idx := &commandIndex{path: path, modTime: st.ModTime(), byName: map[string]string{}}
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) == 0 || !strings.HasPrefix(fields[0], "sim/") {
			continue
		}
		desc := strings.Join(fields[1:], " ")
		idx.byName[fields[0]] = desc
		idx.all = append(idx.all, DatarefInfo{Kind: "command", Name: fields[0], Writable: true, Description: desc,
			Category: suggestCategory(fields[0], desc)})
	}
	commandCache = idx
	return idx
}

// isCommandCandidate: the command areas the plugin's learn mode skips too
// (views, menus, replay, joystick axes...).
func isCommandCandidate(name string) bool {
	for _, prefix := range []string{"sim/none/", "sim/operation/", "sim/view/", "sim/general/", "sim/replay/",
		"sim/map/", "sim/flight_controls/", "sim/multiplayer/", "sim/weapons/", "sim/joystick/", "sim/ground_ops/"} {
		if strings.HasPrefix(name, prefix) {
			return false
		}
	}
	return true
}

// isProfileCandidate filters out areas that are never a cockpit switch -
// the same list the plugin's learn mode skips (plugin_main.cpp's
// IsLearnable): physics, clocks, joystick hardware, multiplayer slots,
// overrides, aircraft definition.
func isProfileCandidate(name string) bool {
	for _, prefix := range []string{"sim/time/", "sim/flightmodel/position/", "sim/flightmodel/forces/",
		"sim/flightmodel/movingparts/", "sim/flightmodel/misc/", "sim/flightmodel/ground/", "sim/flightmodel2/",
		"sim/graphics/", "sim/weather/", "sim/multiplayer/", "sim/network/", "sim/joystick/", "sim/operation/",
		"sim/cockpit2/tcas/", "sim/aircraft/", "sim/private/", "sim/test/", "sim/atc/", "sim/airfoils/",
		"sim/world/"} {
		if strings.HasPrefix(name, prefix) {
			return false
		}
	}
	return true
}

// describeDataref returns DataRefs.txt's entry for name, or a bare entry
// with a suggested category for add-on datarefs that aren't listed there.
func describeCommand(xplaneRoot, name string) DatarefInfo {
	desc := loadCommandIndex(xplaneRoot).byName[name]
	return DatarefInfo{Kind: "command", Name: name, Writable: true, Description: desc, Category: suggestCategory(name, desc)}
}

func describeDataref(xplaneRoot, name string) DatarefInfo {
	if idx := loadFullDatarefIndex(xplaneRoot); idx != nil {
		if info, ok := idx.byName[name]; ok {
			return *info
		}
	}
	return DatarefInfo{Name: name, Writable: true, Category: suggestCategory(name, "")}
}

func annotate(entries []ProfileEntry, index map[string]bool, commands map[string]string) {
	if index == nil {
		return
	}
	for i := range entries {
		name := entries[i].Name
		if entries[i].Kind == "command" {
			entries[i].Warning = ""
			if _, ok := commands[name]; strings.HasPrefix(name, "sim/") && commands != nil && !ok {
				entries[i].Warning = "not in X-Plane's Commands.txt - typo?"
			}
			continue
		}
		if !strings.HasPrefix(name, "sim/") {
			entries[i].Warning = "" // add-on dataref - can't be checked, only works if that add-on is loaded
			continue
		}
		w, ok := index[name]
		switch {
		case isFlightControlDataref(name):
			entries[i].Warning = "flight control - the pilot flying's yoke and levers are shown automatically, this line is ignored"
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
	annotate(data.Entries, index, loadCommandIndex(xplaneRoot).byName)
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
		if e.Kind != "command" {
			e.Kind = "dataref"
		}
		if seen[e.Kind+" "+e.Name] {
			continue
		}
		seen[e.Kind+" "+e.Name] = true
		if e.Kind == "command" {
			e.Stream = false
			e.Output = false
		}
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
