package main

import (
	"sync"
	"testing"
)

func TestInitialWindowSize(t *testing.T) {
	if w, h, m := initialWindowSize(companionConfig{}); w != defaultWindowWidth || h != defaultWindowHeight || m {
		t.Fatalf("defaults: %d %d %v", w, h, m)
	}
	cfg := companionConfig{Window: &WindowState{Width: 1200, Height: 800, Maximised: true}}
	if w, h, m := initialWindowSize(cfg); w != 1200 || h != 800 || !m {
		t.Fatalf("saved: %d %d %v", w, h, m)
	}
	cfg.Window = &WindowState{Width: 200, Height: 100}
	if w, h, _ := initialWindowSize(cfg); w != defaultWindowWidth || h != defaultWindowHeight {
		t.Fatalf("too small falls back to the default: %d %d", w, h)
	}
}

func TestLastPageAndConcurrentConfigUpdates(t *testing.T) {
	useTempConfigDir(t)
	a := NewApp()
	if a.GetLastPage() != "" {
		t.Fatal("no page on the first start")
	}
	if err := a.SetLastPage("debrief"); err != nil || a.GetLastPage() != "debrief" {
		t.Fatalf("last page not saved: %v %q", err, a.GetLastPage())
	}
	// Different fields updated at the same time must all survive.
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(2)
		go func(i int) {
			defer wg.Done()
			_ = updateConfig(func(c *companionConfig) { c.Window = &WindowState{Width: 700 + i, Height: 500} })
		}(i)
		go func() {
			defer wg.Done()
			_ = updateConfig(func(c *companionConfig) { c.XPlanePath = "/xp" })
		}()
	}
	wg.Wait()
	cfg := loadConfig()
	if cfg.XPlanePath != "/xp" || cfg.Window == nil || cfg.LastPage != "debrief" {
		t.Fatalf("an update got lost: %+v", cfg)
	}
}
