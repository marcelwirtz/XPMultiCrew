package main

import (
	"bufio"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// pluginLogPrefix is what every one of the plugin's own XPLMDebugString
// calls starts with (plugin/src/core/plugin_main.cpp) - used to filter
// X-Plane's Log.txt (which also carries X-Plane's own and every other
// plugin's messages) down to just this plugin's lines.
const pluginLogPrefix = "XPMultiCrew: "

// LogLinesResult is GetRecentLogLines' return value - a struct rather
// than multiple return values, matching this codebase's existing Wails-
// binding convention (see ChooseXPlaneResult in app.go).
type LogLinesResult struct {
	Lines []string `json:"lines"`
	// Offset is a byte position into Log.txt - pass it back as the next
	// call's sinceOffset so only newly-appended lines are returned
	// instead of rereading the whole file every poll.
	Offset int64 `json:"offset"`
}

// GetRecentLogLines returns the plugin's own lines appended to X-Plane's
// Log.txt since sinceOffset (0 to start from the current end of the file
// - see main.js's first call). Polled the same way app.go's pollStatus()
// already polls plugin status every second, not pushed - matches the
// existing convention instead of introducing a new push mechanism.
//
// Not an error, just an empty result: no X-Plane folder chosen yet, or
// Log.txt doesn't exist (X-Plane never run, or a fresh install). If
// Log.txt is shorter than sinceOffset (X-Plane restarted, which
// truncates and rewrites Log.txt from scratch), starts over from the
// beginning rather than erroring or returning nothing forever.
//
// A log line that's still being written by X-Plane exactly when this
// reads up to the file's current end can appear split across two
// consecutive polls - a cosmetic edge case not worth extra complexity to
// avoid in a troubleshooting view.
func GetRecentLogLines(xplanePath string, sinceOffset int64) (LogLinesResult, error) {
	if xplanePath == "" {
		return LogLinesResult{Offset: sinceOffset}, nil
	}

	f, err := os.Open(filepath.Join(xplanePath, "Log.txt"))
	if err != nil {
		return LogLinesResult{Offset: sinceOffset}, nil
	}
	defer f.Close()

	info, err := f.Stat()
	if err != nil {
		return LogLinesResult{Offset: sinceOffset}, nil
	}
	size := info.Size()

	if sinceOffset > size {
		sinceOffset = 0 // Log.txt was truncated/rewritten (e.g. X-Plane restarted)
	}
	if sinceOffset == size {
		return LogLinesResult{Offset: size}, nil // nothing new
	}

	if _, err := f.Seek(sinceOffset, io.SeekStart); err != nil {
		return LogLinesResult{Offset: sinceOffset}, err
	}

	var lines []string
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for scanner.Scan() {
		line := scanner.Text()
		if strings.Contains(line, pluginLogPrefix) {
			lines = append(lines, line)
		}
	}
	if err := scanner.Err(); err != nil {
		return LogLinesResult{Lines: lines, Offset: size}, err
	}

	return LogLinesResult{Lines: lines, Offset: size}, nil
}
