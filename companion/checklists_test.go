package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseChecklists(t *testing.T) {
	lists, err := parseChecklists("# c\nCHECKLIST Before Start\r\nITEM Beacon | ON | sim/cockpit/electrical/beacon_lights_on == 1\n" +
		"ITEM Battery | ON | sim/cockpit2/electrical/battery_on[0]>=1\nITEM Belts | FASTENED\n\nCHECKLIST Shutdown\nITEM Mixture | CUTOFF | sim/x <= -0.5\n")
	if err != nil || len(lists) != 2 || len(lists[0].Items) != 3 || lists[1].Title != "Shutdown" {
		t.Fatalf("unexpected: %+v %v", lists, err)
	}
	c := lists[0].Items[1].Condition
	if c == nil || c.Key != "sim/cockpit2/electrical/battery_on[0]" || c.Op != ">=" || c.Value != 1 {
		t.Fatalf("condition: %+v", c)
	}
	if lists[0].Items[2].Condition != nil || lists[1].Items[0].Condition.Value != -0.5 {
		t.Fatalf("items: %+v", lists)
	}
	for _, bad := range []string{"ITEM x | y", "CHECKLIST A\nITEM only", "CHECKLIST A\nITEM a | b | sim/x = 1", "FOO"} {
		if _, err := parseChecklists(bad); err == nil {
			t.Fatalf("expected an error for %q", bad)
		} else if !strings.Contains(err.Error(), "line") {
			t.Fatalf("error should name the line: %v", err)
		}
	}
}

func TestBundledC172ChecklistParses(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "plugin", "Resources", "checklists", "C172.txt"))
	if err != nil {
		t.Fatal(err)
	}
	lists, err := parseChecklists(string(raw))
	if err != nil || len(lists) < 5 {
		t.Fatalf("bundled C172 checklist: %d lists, %v", len(lists), err)
	}
}

func TestSaveLoadDeleteChecklists(t *testing.T) {
	root := t.TempDir()
	_, bundled := checklistPaths(root, "C172")
	_ = os.MkdirAll(filepath.Dir(bundled), 0755)
	_ = os.WriteFile(bundled, []byte("CHECKLIST A\nITEM a | b\n"), 0644)
	f, err := loadChecklists(root, "c172")
	if err != nil || f.Source != "bundled" || len(f.Lists) != 1 {
		t.Fatalf("bundled: %+v %v", f, err)
	}
	if _, err := saveChecklists(root, "C172", "ITEM broken"); err == nil {
		t.Fatal("invalid text must not be saved")
	}
	f, err = saveChecklists(root, "C172", "CHECKLIST Mine\nITEM x | y\n")
	if err != nil || f.Source != "user" || f.Lists[0].Title != "Mine" {
		t.Fatalf("user: %+v %v", f, err)
	}
	_ = deleteUserChecklists(root, "C172")
	if f, _ = loadChecklists(root, "C172"); f.Source != "bundled" {
		t.Fatalf("after delete: %+v", f)
	}
	if f, _ = loadChecklists(root, "B738"); f.Source != "none" {
		t.Fatalf("missing: %+v", f)
	}
}

func TestProfileCommandsAndLearnKinds(t *testing.T) {
	got := parseProfile("DATAREF sim/a STREAM\nCOMMAND sim/autopilot/heading STREAM CATEGORY avionics\n")
	if len(got) != 2 || got[1].Kind != "command" || got[1].Stream || got[1].Category != "avionics" {
		t.Fatalf("parse: %+v", got)
	}
	if !strings.Contains(formatProfile("C172", got), "COMMAND sim/autopilot/heading CATEGORY avionics\n") {
		t.Fatalf("format: %s", formatProfile("C172", got))
	}
	c := NewPluginClient()
	c.applyStatusMessage("LEARN watching 1 0\nLEARN_CHANGES sim/autopilot/heading|CMD|2;sim/x|0|1\n")
	l := c.Learn()
	if l.Changes[0].Kind != "command" || l.Changes[0].After != "2" || l.Changes[1].Kind != "dataref" {
		t.Fatalf("learn kinds: %+v", l.Changes)
	}
	c.applyStatusMessage("SC_DESYNC 0 sim/a=1;sim/b=[0]=2\nWATCH_VALUES sim/a=1;sim/b[0]=?\nCHECKLIST_REMOTE v1|x\n")
	remote, values, desync := c.ChecklistState()
	if remote != "v1|x" || values["sim/a"] != 1 || len(values) != 1 || desync == nil || len(desync.Items) != 2 ||
		desync.Items[1].Value != "[0]=2" {
		t.Fatalf("status: %q %+v %+v", remote, values, desync)
	}
}
