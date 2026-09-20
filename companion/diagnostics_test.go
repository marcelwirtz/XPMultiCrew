package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func writeLogFile(t *testing.T, dir string, contents string) string {
	t.Helper()
	path := filepath.Join(dir, "Log.txt")
	if err := os.WriteFile(path, []byte(contents), 0644); err != nil {
		t.Fatalf("writing test Log.txt: %v", err)
	}
	return path
}

func TestGetRecentLogLinesReturnsEmptyWhenNoXPlanePathChosen(t *testing.T) {
	got, err := GetRecentLogLines("", 0)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got.Lines) != 0 || got.Offset != 0 {
		t.Fatalf("expected an empty result, got %+v", got)
	}
}

func TestGetRecentLogLinesReturnsEmptyWhenLogFileMissing(t *testing.T) {
	dir := t.TempDir() // no Log.txt written here

	got, err := GetRecentLogLines(dir, 0)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(got.Lines) != 0 || got.Offset != 0 {
		t.Fatalf("expected an empty result, got %+v", got)
	}
}

func TestGetRecentLogLinesFiltersToPluginPrefix(t *testing.T) {
	dir := t.TempDir()
	writeLogFile(t, dir, "X-Plane starting up\nXPMultiCrew: shared cockpit ready\nSome other plugin: hello\n")

	got, err := GetRecentLogLines(dir, 0)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := []string{"XPMultiCrew: shared cockpit ready"}
	if !reflect.DeepEqual(got.Lines, want) {
		t.Fatalf("unexpected lines: got %+v, want %+v", got.Lines, want)
	}
	if got.Offset == 0 {
		t.Fatal("expected a non-zero offset after reading a non-empty file")
	}
}

func TestGetRecentLogLinesOnlyReturnsLinesAppendedSinceOffset(t *testing.T) {
	dir := t.TempDir()
	writeLogFile(t, dir, "XPMultiCrew: first line\n")

	first, err := GetRecentLogLines(dir, 0)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(first.Lines) != 1 {
		t.Fatalf("expected 1 line on the first read, got %+v", first.Lines)
	}

	// Nothing new yet - same offset, no new lines.
	again, err := GetRecentLogLines(dir, first.Offset)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(again.Lines) != 0 {
		t.Fatalf("expected no new lines when nothing was appended, got %+v", again.Lines)
	}
	if again.Offset != first.Offset {
		t.Fatalf("expected the offset to stay the same, got %d want %d", again.Offset, first.Offset)
	}

	// Append a second line and confirm only the new one comes back.
	f, err := os.OpenFile(filepath.Join(dir, "Log.txt"), os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		t.Fatalf("opening log for append: %v", err)
	}
	if _, err := f.WriteString("XPMultiCrew: second line\n"); err != nil {
		t.Fatalf("appending to log: %v", err)
	}
	_ = f.Close()

	second, err := GetRecentLogLines(dir, first.Offset)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := []string{"XPMultiCrew: second line"}
	if !reflect.DeepEqual(second.Lines, want) {
		t.Fatalf("unexpected lines: got %+v, want %+v", second.Lines, want)
	}
}

func TestGetRecentLogLinesRestartsFromBeginningWhenFileShrinks(t *testing.T) {
	dir := t.TempDir()
	writeLogFile(t, dir, "XPMultiCrew: a fairly long line that takes up some space\n")

	first, err := GetRecentLogLines(dir, 0)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	// Simulate X-Plane restarting: Log.txt truncated and rewritten short.
	writeLogFile(t, dir, "XPMultiCrew: fresh start\n")

	got, err := GetRecentLogLines(dir, first.Offset)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := []string{"XPMultiCrew: fresh start"}
	if !reflect.DeepEqual(got.Lines, want) {
		t.Fatalf("expected a restart from the beginning after truncation: got %+v, want %+v", got.Lines, want)
	}
}

func TestAppGetRecentLogLinesUsesConfiguredXPlanePath(t *testing.T) {
	useTempConfigDir(t)

	dir := t.TempDir()
	writeLogFile(t, dir, "XPMultiCrew: hello from the configured install\n")

	cfg := loadConfig()
	cfg.XPlanePath = dir
	if err := saveConfig(cfg); err != nil {
		t.Fatalf("unexpected error saving config: %v", err)
	}

	app := NewApp()
	got, err := app.GetRecentLogLines(0)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := []string{"XPMultiCrew: hello from the configured install"}
	if !reflect.DeepEqual(got.Lines, want) {
		t.Fatalf("unexpected lines: got %+v, want %+v", got.Lines, want)
	}
}
