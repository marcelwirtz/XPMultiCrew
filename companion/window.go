package main

import (
	"context"
	"os"
	"runtime"
	"strings"

	wailsruntime "github.com/wailsapp/wails/v2/pkg/runtime"
)

// The window comes back where it was: size and maximized state on every
// platform, the position too where the window system lets an app place
// its own windows (Windows, X11 - not Wayland, where the compositor
// decides). Tracked from the status loop once a second rather than on
// close, so it also survives a crash or a kill.

// WindowState is the remembered window geometry (logical pixels).
type WindowState struct {
	Width       int  `json:"width"`
	Height      int  `json:"height"`
	X           int  `json:"x"`
	Y           int  `json:"y"`
	HasPosition bool `json:"hasPosition"`
	Maximised   bool `json:"maximised"`
}

const (
	defaultWindowWidth  = 760
	defaultWindowHeight = 520
	minWindowWidth      = 600
	minWindowHeight     = 420
	maxWindowSide       = 10000 // anything bigger is a bogus reading
)

// positionSupported: Wayland gives apps no way to read or set their window
// position (Wails always reports 0,0 there).
func positionSupported() bool {
	if runtime.GOOS != "linux" {
		return true
	}
	if strings.EqualFold(os.Getenv("GDK_BACKEND"), "x11") {
		return true
	}
	return os.Getenv("WAYLAND_DISPLAY") == ""
}

// initialWindowSize is what main.go opens the window with.
func initialWindowSize(cfg companionConfig) (width, height int, maximised bool) {
	w := cfg.Window
	if w == nil || w.Width < minWindowWidth || w.Height < minWindowHeight || w.Width > maxWindowSide || w.Height > maxWindowSide {
		return defaultWindowWidth, defaultWindowHeight, w != nil && w.Maximised
	}
	return w.Width, w.Height, w.Maximised
}

// restoreWindowPosition runs once the window exists (OnDomReady). The
// saved position is only used if it's still on a screen - a monitor may
// have been unplugged since.
func (a *App) restoreWindowPosition(ctx context.Context) {
	defer func() {
		a.mu.Lock()
		a.windowReady = true
		a.mu.Unlock()
	}()
	w := loadConfig().Window
	if w == nil || !w.HasPosition || w.Maximised || !positionSupported() {
		return
	}
	screens, err := wailsruntime.ScreenGetAll(ctx)
	if err != nil || len(screens) == 0 {
		return
	}
	// Screens come without their offsets, so check against the combined
	// desktop: at least 100 px of the window must stay reachable.
	totalWidth, maxHeight := 0, 0
	for _, s := range screens {
		totalWidth += s.Size.Width
		if s.Size.Height > maxHeight {
			maxHeight = s.Size.Height
		}
	}
	if w.X < -w.Width+100 || w.X > totalWidth-100 || w.Y < 0 || w.Y > maxHeight-100 {
		return
	}
	wailsruntime.WindowSetPosition(ctx, w.X, w.Y)
}

// trackWindow saves the window geometry when it changed.
func (a *App) trackWindow() {
	a.mu.Lock()
	ready := a.windowReady
	a.mu.Unlock()
	if !ready || wailsruntime.WindowIsMinimised(a.ctx) {
		return
	}
	cur := loadConfig().Window
	next := WindowState{Width: defaultWindowWidth, Height: defaultWindowHeight}
	if cur != nil {
		next = *cur
	}
	next.Maximised = wailsruntime.WindowIsMaximised(a.ctx)
	if !next.Maximised {
		// The normal size/position - kept as they were while maximised, so
		// un-maximising after a restart gives the old window back.
		w, h := wailsruntime.WindowGetSize(a.ctx)
		if w >= minWindowWidth && h >= minWindowHeight && w <= maxWindowSide && h <= maxWindowSide {
			next.Width, next.Height = w, h
		}
		if positionSupported() {
			next.X, next.Y = wailsruntime.WindowGetPosition(a.ctx)
			next.HasPosition = true
		}
	}
	if cur != nil && *cur == next {
		return
	}
	_ = updateConfig(func(cfg *companionConfig) { cfg.Window = &next })
}

// GetLastPage returns the page that was open last ("" on the first start).
func (a *App) GetLastPage() string {
	return loadConfig().LastPage
}

// SetLastPage remembers the open page.
func (a *App) SetLastPage(page string) error {
	if len(page) > 40 {
		return nil
	}
	if loadConfig().LastPage == page {
		return nil
	}
	return updateConfig(func(cfg *companionConfig) { cfg.LastPage = page })
}
