package main

import (
	"log"
	"time"
)

// logLimiter lets at most `max` lines through per `window` and reports how
// many it dropped once the next window opens. Not goroutine-safe - only
// used from main's read loop.
type logLimiter struct {
	max        int
	window     time.Duration
	windowFrom time.Time
	count      int
	suppressed int
	now        func() time.Time
	printf     func(format string, args ...interface{})
}

func newLogLimiter(max int, window time.Duration) *logLimiter {
	return &logLimiter{max: max, window: window, now: time.Now, printf: log.Printf}
}

func (l *logLimiter) Printf(format string, args ...interface{}) {
	now := l.now()
	if now.Sub(l.windowFrom) >= l.window {
		if l.suppressed > 0 {
			l.printf("(%d similar log lines suppressed)", l.suppressed)
		}
		l.windowFrom = now
		l.count = 0
		l.suppressed = 0
	}
	if l.count >= l.max {
		l.suppressed++
		return
	}
	l.count++
	l.printf(format, args...)
}
