package main

import (
	"reflect"
	"testing"
)

func TestApplyStatusMessageSingleLine(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("FORMATION connected, code 'ABC', peer 1\n")
	formation, _ := c.Status()
	if formation != "connected, code 'ABC', peer 1" {
		t.Fatalf("unexpected formation status: %q", formation)
	}
}

func TestApplyStatusMessageBothLines(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("FORMATION connected\nSHARED_COCKPIT running as MASTER\n")
	formation, sc := c.Status()
	if formation != "connected" {
		t.Fatalf("unexpected formation status: %q", formation)
	}
	if sc != "running as MASTER" {
		t.Fatalf("unexpected shared cockpit status: %q", sc)
	}
}

func TestApplyStatusMessageIgnoresUnknownLines(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("SOMETHING_ELSE foo\nFORMATION bar\n")
	formation, _ := c.Status()
	if formation != "bar" {
		t.Fatalf("unexpected formation status: %q", formation)
	}
}

func TestApplyStatusMessagePreservesOtherFieldWhenOnlyOneLineSent(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("FORMATION connected\nSHARED_COCKPIT running as MASTER\n")
	c.applyStatusMessage("FORMATION reconnecting...\n")
	formation, sc := c.Status()
	if formation != "reconnecting..." {
		t.Fatalf("unexpected formation status: %q", formation)
	}
	if sc != "running as MASTER" {
		t.Fatalf("shared cockpit status should be unchanged, got: %q", sc)
	}
}

func TestSimReadyDefaultsFalse(t *testing.T) {
	c := NewPluginClient()
	if c.SimReady() {
		t.Fatal("SimReady should default to false before the plugin is ever seen")
	}
}

func TestApplyStatusMessageParsesSimReady(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("FORMATION connected\nSHARED_COCKPIT not started\nSIM_READY 1\n")
	if !c.SimReady() {
		t.Fatal("expected SimReady() to be true after SIM_READY 1")
	}

	c.applyStatusMessage("SIM_READY 0\n")
	if c.SimReady() {
		t.Fatal("expected SimReady() to be false after SIM_READY 0")
	}
}

func TestCodesDefaultEmpty(t *testing.T) {
	c := NewPluginClient()
	formationCode, sharedCockpitCode := c.Codes()
	if formationCode != "" || sharedCockpitCode != "" {
		t.Fatalf("expected both codes empty before the plugin is seen, got %q / %q",
			formationCode, sharedCockpitCode)
	}
}

func TestApplyStatusMessageParsesCodes(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("FORMATION connected\nFORMATION_CODE ABC123\nSHARED_COCKPIT_CODE DEF456\n")
	formationCode, sharedCockpitCode := c.Codes()
	if formationCode != "ABC123" {
		t.Fatalf("unexpected formation code: %q", formationCode)
	}
	if sharedCockpitCode != "DEF456" {
		t.Fatalf("unexpected shared cockpit code: %q", sharedCockpitCode)
	}
	// FORMATION_CODE must not have been swallowed by the "FORMATION "
	// prefix match meant for the plain status line.
	formation, _ := c.Status()
	if formation != "connected" {
		t.Fatalf("FORMATION_CODE line corrupted the FORMATION status line: %q", formation)
	}
}

func TestApplyStatusMessageClearsCodeOnEmptyValue(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("FORMATION_CODE ABC123\n")
	c.applyStatusMessage("FORMATION_CODE \n")
	formationCode, _ := c.Codes()
	if formationCode != "" {
		t.Fatalf("expected FORMATION_CODE to clear to empty, got %q", formationCode)
	}
}

func TestFormationPeersDefaultsToEmptyNotNil(t *testing.T) {
	c := NewPluginClient()
	peers := c.FormationPeers()
	if peers == nil {
		t.Fatal("FormationPeers() should never return nil (marshals to JSON null, not [])")
	}
	if len(peers) != 0 {
		t.Fatalf("expected no peers before the plugin is seen, got %+v", peers)
	}
}

func TestApplyStatusMessageParsesPeers(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("PEERS 111:C172;222:B738\n")
	got := c.FormationPeers()
	want := []FormationPeer{{ID: 111, ICAO: "C172"}, {ID: 222, ICAO: "B738"}}
	if !reflect.DeepEqual(sortedByID(got), sortedByID(want)) {
		t.Fatalf("unexpected peers: got %+v, want %+v", got, want)
	}
}

func TestApplyStatusMessagePeersEmptyClearsList(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("PEERS 111:C172\n")
	c.applyStatusMessage("PEERS \n")
	peers := c.FormationPeers()
	if len(peers) != 0 {
		t.Fatalf("expected peer list to clear, got %+v", peers)
	}
}

func TestParseFormationPeersSkipsMalformedEntries(t *testing.T) {
	// "garbage" has no colon at all; "abc:C172" has a non-numeric id - both
	// should be dropped rather than corrupting the rest of the list.
	got := parseFormationPeers("111:C172;garbage;abc:C172;444:B738")
	want := []FormationPeer{{ID: 111, ICAO: "C172"}, {ID: 444, ICAO: "B738"}}
	if !reflect.DeepEqual(sortedByID(got), sortedByID(want)) {
		t.Fatalf("unexpected peers: got %+v, want %+v", got, want)
	}
}

func TestRunningVersionDefaultsEmpty(t *testing.T) {
	c := NewPluginClient()
	if got := c.RunningVersion(); got != "" {
		t.Fatalf("expected empty RunningVersion before the plugin is seen, got %q", got)
	}
}

func TestApplyStatusMessageParsesRunningVersion(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("PLUGIN_VERSION v0.1.0\n")
	if got := c.RunningVersion(); got != "v0.1.0" {
		t.Fatalf("unexpected running version: %q", got)
	}
}

func sortedByID(peers []FormationPeer) []FormationPeer {
	out := make([]FormationPeer, len(peers))
	copy(out, peers)
	for i := 1; i < len(out); i++ {
		for j := i; j > 0 && out[j-1].ID > out[j].ID; j-- {
			out[j-1], out[j] = out[j], out[j-1]
		}
	}
	return out
}
