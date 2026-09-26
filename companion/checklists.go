package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// Shared Cockpit checklists: per aircraft type, a plain text file (format
// in plugin/Resources/checklists/C172.txt). Your own copy lives in
// <X-Plane>/XPMultiCrew_checklists/<ICAO>.txt and wins over the one
// bundled with the plugin - same scheme as the dataref profiles.

const userChecklistsDirName = "XPMultiCrew_checklists"

// ChecklistCondition makes an item tick itself: dataref (optionally
// "name[index]") compared with a value.
type ChecklistCondition struct {
	Key   string  `json:"key"` // as sent to the plugin's WATCH, e.g. "sim/cockpit2/electrical/battery_on[0]"
	Op    string  `json:"op"`  // == != < <= > >=
	Value float64 `json:"value"`
}

// ChecklistItem is one ITEM line.
type ChecklistItem struct {
	Challenge string              `json:"challenge"`
	Response  string              `json:"response"`
	Condition *ChecklistCondition `json:"condition,omitempty"`
}

// Checklist is one CHECKLIST block.
type Checklist struct {
	Title string          `json:"title"`
	Items []ChecklistItem `json:"items"`
}

// ChecklistFile is what the Checklists page loads for an aircraft type.
type ChecklistFile struct {
	ICAO   string      `json:"icao"`
	Source string      `json:"source"` // "user", "bundled" or "none"
	Text   string      `json:"text"`
	Lists  []Checklist `json:"lists"`
}

var conditionPattern = regexp.MustCompile(`^(\S+?)(\[\d+\])?\s*(==|!=|<=|>=|<|>)\s*(-?\d+(\.\d+)?)$`)

// parseChecklists parses the text and reports the first problem with its
// line number, so the editor can say what's wrong.
func parseChecklists(text string) ([]Checklist, error) {
	lists := []Checklist{}
	for n, raw := range strings.Split(strings.ReplaceAll(text, "\r", ""), "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		keyword, rest, _ := strings.Cut(line, " ")
		rest = strings.TrimSpace(rest)
		switch keyword {
		case "CHECKLIST":
			if rest == "" {
				return nil, fmt.Errorf("line %d: CHECKLIST needs a title", n+1)
			}
			lists = append(lists, Checklist{Title: rest, Items: []ChecklistItem{}})
		case "ITEM":
			if len(lists) == 0 {
				return nil, fmt.Errorf("line %d: ITEM before the first CHECKLIST", n+1)
			}
			parts := strings.Split(rest, "|")
			if len(parts) < 2 || len(parts) > 3 || strings.TrimSpace(parts[0]) == "" {
				return nil, fmt.Errorf("line %d: expected ITEM <challenge> | <response> [| <condition>]", n+1)
			}
			item := ChecklistItem{Challenge: strings.TrimSpace(parts[0]), Response: strings.TrimSpace(parts[1])}
			if len(parts) == 3 {
				m := conditionPattern.FindStringSubmatch(strings.TrimSpace(parts[2]))
				if m == nil {
					return nil, fmt.Errorf("line %d: condition must look like \"sim/some/dataref[0] == 1\"", n+1)
				}
				v, _ := strconv.ParseFloat(m[4], 64)
				item.Condition = &ChecklistCondition{Key: m[1] + m[2], Op: m[3], Value: v}
			}
			last := &lists[len(lists)-1]
			last.Items = append(last.Items, item)
		default:
			return nil, fmt.Errorf("line %d: unknown keyword %q (use CHECKLIST or ITEM)", n+1, keyword)
		}
	}
	return lists, nil
}

func checklistPaths(xplaneRoot, icao string) (user, bundled string) {
	return filepath.Join(xplaneRoot, userChecklistsDirName, icao+".txt"),
		filepath.Join(xplaneRoot, "Resources", "plugins", pluginDirName, "Resources", "checklists", icao+".txt")
}

func loadChecklists(xplaneRoot, icao string) (ChecklistFile, error) {
	icao, err := normalizeIcao(icao)
	if err != nil {
		return ChecklistFile{}, err
	}
	out := ChecklistFile{ICAO: icao, Source: "none", Lists: []Checklist{}}
	user, bundled := checklistPaths(xplaneRoot, icao)
	for _, c := range []struct{ path, source string }{{user, "user"}, {bundled, "bundled"}} {
		raw, err := os.ReadFile(c.path)
		if err != nil {
			continue
		}
		out.Source = c.source
		out.Text = string(raw)
		lists, err := parseChecklists(out.Text)
		if err != nil {
			return out, fmt.Errorf("%s checklist for %s: %w", c.source, icao, err)
		}
		out.Lists = lists
		break
	}
	return out, nil
}

func saveChecklists(xplaneRoot, icao, text string) (ChecklistFile, error) {
	icao, err := normalizeIcao(icao)
	if err != nil {
		return ChecklistFile{}, err
	}
	if _, err := parseChecklists(text); err != nil {
		return ChecklistFile{}, err
	}
	user, _ := checklistPaths(xplaneRoot, icao)
	if err := os.MkdirAll(filepath.Dir(user), 0755); err != nil {
		return ChecklistFile{}, err
	}
	tmp := user + ".tmp"
	if err := os.WriteFile(tmp, []byte(text), 0644); err != nil {
		return ChecklistFile{}, err
	}
	if err := os.Rename(tmp, user); err != nil {
		_ = os.Remove(tmp)
		return ChecklistFile{}, err
	}
	return loadChecklists(xplaneRoot, icao)
}

func deleteUserChecklists(xplaneRoot, icao string) error {
	icao, err := normalizeIcao(icao)
	if err != nil {
		return err
	}
	user, _ := checklistPaths(xplaneRoot, icao)
	if err := os.Remove(user); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}
