package main

import (
	"fmt"
	"testing"
	"time"
)

func TestLogLimiterCapsLinesPerWindow(t *testing.T) {
	var lines []string
	now := time.Unix(1000, 0)
	l := newLogLimiter(2, time.Minute)
	l.now = func() time.Time { return now }
	l.printf = func(format string, args ...interface{}) { lines = append(lines, fmt.Sprintf(format, args...)) }

	for i := 0; i < 5; i++ {
		l.Printf("line %d", i)
	}
	if len(lines) != 2 {
		t.Fatalf("expected 2 lines in the first window, got %v", lines)
	}
	now = now.Add(time.Minute)
	l.Printf("next")
	if len(lines) != 4 || lines[2] != "(3 similar log lines suppressed)" || lines[3] != "next" {
		t.Fatalf("expected a suppression summary then the new line, got %v", lines)
	}
}
