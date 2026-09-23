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

func TestApplyStatusMessageParsesCslStatus(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("CSL_STATUS 3 model(s) loaded\n")
	if got := c.CslStatus(); got != "3 model(s) loaded" {
		t.Fatalf("unexpected CSL status: %q", got)
	}
}

func TestSharedCockpitOwnershipDefaultsToEmptyNotNil(t *testing.T) {
	c := NewPluginClient()
	ownership := c.SharedCockpitOwnership()
	if ownership == nil {
		t.Fatal("SharedCockpitOwnership() should never return nil (marshals to JSON null, not {})")
	}
	if len(ownership) != 0 {
		t.Fatalf("expected no ownership entries before the plugin is seen, got %+v", ownership)
	}
}

func TestApplyStatusMessageParsesSharedCockpitOwnership(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("SHARED_COCKPIT_OWNERSHIP systems:me engine:peer avionics:me\n")
	got := c.SharedCockpitOwnership()
	want := map[string]string{"systems": "me", "engine": "peer", "avionics": "me"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("unexpected ownership: got %+v, want %+v", got, want)
	}
}

func TestParseSharedCockpitOwnershipSkipsMalformedEntries(t *testing.T) {
	got := parseSharedCockpitOwnership("engine:me garbage avionics:peer")
	want := map[string]string{"engine": "me", "avionics": "peer"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("unexpected ownership: got %+v, want %+v", got, want)
	}
}

func TestParseSharedCockpitOwnershipEmptyStringGivesEmptyMap(t *testing.T) {
	got := parseSharedCockpitOwnership("")
	if len(got) != 0 {
		t.Fatalf("expected empty map for empty input, got %+v", got)
	}
}

func int64Ptr(v int64) *int64 { return &v }
func intPtr(v int) *int       { return &v }

func TestParseLinkQualityAllFieldsPresent(t *testing.T) {
	got := parseLinkQuality("formation_server_rtt_ms:42 formation_peer_loss_pct:111:3;222:10 " +
		"sc_server_rtt_ms:17 sc_master_loss_pct:0")
	want := LinkQuality{
		FormationServerRttMs:       int64Ptr(42),
		FormationPeerLossPct:       map[uint32]int{111: 3, 222: 10},
		SharedCockpitServerRttMs:   int64Ptr(17),
		SharedCockpitMasterLossPct: intPtr(0),
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("unexpected link quality: got %+v, want %+v", got, want)
	}
}

func TestParseLinkQualityUnknownFieldsAreNil(t *testing.T) {
	got := parseLinkQuality("formation_server_rtt_ms:? formation_peer_loss_pct: sc_server_rtt_ms:? sc_master_loss_pct:?")
	if got.FormationServerRttMs != nil || got.SharedCockpitServerRttMs != nil || got.SharedCockpitMasterLossPct != nil {
		t.Fatalf("expected nil for '?' fields, got %+v", got)
	}
	if got.FormationPeerLossPct != nil {
		t.Fatalf("expected nil map for empty peer loss list, got %+v", got.FormationPeerLossPct)
	}
}

func TestParseLinkQualitySkipsMalformedEntries(t *testing.T) {
	got := parseLinkQuality("formation_server_rtt_ms:garbage sc_server_rtt_ms:17")
	if got.FormationServerRttMs != nil {
		t.Fatalf("expected nil for unparseable rtt, got %+v", got.FormationServerRttMs)
	}
	if got.SharedCockpitServerRttMs == nil || *got.SharedCockpitServerRttMs != 17 {
		t.Fatalf("expected sc_server_rtt_ms to still parse, got %+v", got.SharedCockpitServerRttMs)
	}
}

func TestApplyStatusMessageParsesLinkQualityAndAircraftMismatch(t *testing.T) {
	c := NewPluginClient()
	c.applyStatusMessage("LINK_QUALITY formation_server_rtt_ms:42 sc_server_rtt_ms:?\n" +
		"SHARED_COCKPIT_AIRCRAFT_MISMATCH C172:B738\n")
	lq := c.LinkQuality()
	if lq.FormationServerRttMs == nil || *lq.FormationServerRttMs != 42 {
		t.Fatalf("unexpected formation server RTT: %+v", lq.FormationServerRttMs)
	}
	if lq.SharedCockpitServerRttMs != nil {
		t.Fatalf("expected nil sc server RTT for '?', got %+v", lq.SharedCockpitServerRttMs)
	}
	if got := c.SharedCockpitAircraftMismatch(); got != "C172:B738" {
		t.Fatalf("unexpected aircraft mismatch: got %q", got)
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
