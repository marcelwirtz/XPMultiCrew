package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestParseProfileMirrorsPluginGrammar(t *testing.T) {
	text := "# comment\n" +
		"DATAREF sim/a CATEGORY engine\n" +
		"DATAREF sim/b STREAM CATEGORY avionics # trailing comment\n" +
		"DATAREF sim/c CATEGORY avionics STREAM\n" +
		"DATAREF sim/d CATEGORY bogus\n" +
		"DATAREF sim/e CATEGORY flight\n" +
		"ROLE MASTER\n\n"
	got := parseProfile(text)
	want := []ProfileEntry{
		{Name: "sim/a", Category: "engine"},
		{Name: "sim/b", Stream: true, Category: "avionics"},
		{Name: "sim/c", Stream: true, Category: "avionics"},
		{Name: "sim/d", Category: "systems"},
		{Name: "sim/e", Category: "systems"}, // flight is the MASTER role, not a dataref bucket
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("parseProfile:\n got %+v\nwant %+v", got, want)
	}
	if again := parseProfile(formatProfile("C172", got)); !reflect.DeepEqual(again, want) {
		t.Fatalf("format/parse round trip changed entries: %+v", again)
	}
}

func TestSaveLoadDeleteProfile(t *testing.T) {
	root := t.TempDir()
	bundled := bundledProfilesDir(root)
	if err := os.MkdirAll(bundled, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bundled, "C172.txt"), []byte("DATAREF sim/x\n"), 0644); err != nil {
		t.Fatal(err)
	}
	plugins := filepath.Join(root, "Resources", "plugins")
	if err := os.WriteFile(filepath.Join(plugins, "DataRefs.txt"),
		[]byte("2 1234 header\nsim/x\tint\ty\tboolean\tdesc\nsim/ro\tint\tn\tboolean\tdesc\n"), 0644); err != nil {
		t.Fatal(err)
	}

	data, err := loadProfile(root, "c172")
	if err != nil || data.Source != "bundled" || len(data.Entries) != 1 || !data.Validated {
		t.Fatalf("expected the bundled profile, got %+v (%v)", data, err)
	}

	saved, err := saveProfile(root, "C172", []ProfileEntry{
		{Name: "sim/x", Category: "engine"},
		{Name: "sim/ro", Category: "nonsense"},
		{Name: "sim/typo"},
		{Name: "  "},        // dropped
		{Name: "sim/x"},     // duplicate, dropped
		{Name: "addon/foo"}, // not checkable
	})
	if err != nil {
		t.Fatal(err)
	}
	if saved.Source != "user" || len(saved.Entries) != 4 {
		t.Fatalf("expected 4 entries from the user override, got %+v", saved)
	}
	if saved.Entries[0].Warning != "" || saved.Entries[1].Warning == "" || saved.Entries[2].Warning == "" ||
		saved.Entries[3].Warning != "" {
		t.Fatalf("unexpected validation warnings: %+v", saved.Entries)
	}
	if saved.Entries[1].Category != "systems" {
		t.Fatalf("invalid category should fall back to systems, got %q", saved.Entries[1].Category)
	}

	list := listProfiles(root)
	if len(list) != 1 || !list[0].HasUser || !list[0].HasBundled {
		t.Fatalf("unexpected profile list: %+v", list)
	}

	if err := deleteUserProfile(root, "C172"); err != nil {
		t.Fatal(err)
	}
	if data, _ := loadProfile(root, "C172"); data.Source != "bundled" {
		t.Fatalf("after deleting the override the bundled profile should apply again, got %q", data.Source)
	}
	if err := deleteUserProfile(root, "C172"); err != nil {
		t.Fatalf("deleting a missing override should be a no-op, got %v", err)
	}
	if _, err := saveProfile(root, "../evil", nil); err == nil {
		t.Fatal("expected an invalid ICAO to be rejected")
	}
}
