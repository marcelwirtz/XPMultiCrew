package main

import "testing"

func TestWindowTitle(t *testing.T) {
	original := companionVersion
	t.Cleanup(func() { companionVersion = original })

	cases := []struct {
		version string
		want    string
	}{
		{version: "dev", want: "XPMultiCrew"}, // unset ldflag (local `wails dev`/`wails build`)
		{version: "", want: "XPMultiCrew"},    // belt-and-suspenders for an empty -X value
		{version: "v0.1.5", want: "XPMultiCrew v0.1.5"},
		{version: "v0.1.5-3-gabc1234-dirty", want: "XPMultiCrew v0.1.5-3-gabc1234-dirty"},
	}
	for _, c := range cases {
		companionVersion = c.version
		if got := windowTitle(); got != c.want {
			t.Errorf("windowTitle() with companionVersion=%q = %q, want %q", c.version, got, c.want)
		}
	}
}
