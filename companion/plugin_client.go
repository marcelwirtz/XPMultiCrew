package main

import (
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Fixed local ports agreed with the plugin - see
// plugin/include/control/control_listener.h. Both sides always talk over
// 127.0.0.1: this app controls the X-Plane instance running on the same
// machine, not a remote one.
const pluginPort = 49030
const companionPort = 49031

// FormationPeer is one entry in PluginClient's connected-peers list - see
// control_listener.h's PEERS line. ID is the packet's sender_id, not the
// rendezvous session's small peer_id: the plugin doesn't currently
// correlate the two, so this is the best available stable identifier for
// "which aircraft is this" across updates.
type FormationPeer struct {
	ID       uint32 `json:"id"`
	ICAO     string `json:"icao"`
	Callsign string `json:"callsign"`
}

// PluginPrefs mirrors control_listener.h's PREFS line / SET_PREFS command.
type PluginPrefs struct {
	Callsign   string `json:"callsign"` // "" = aircraft tail number
	ShowLabels bool   `json:"showLabels"`
	EnvSync    bool   `json:"envSync"`
}

// encode renders the SET_PREFS/PREFS argument form.
func (p PluginPrefs) encode() string {
	cs := p.Callsign
	if cs == "" {
		cs = "-"
	}
	b := func(v bool) string {
		if v {
			return "1"
		}
		return "0"
	}
	return cs + " " + b(p.ShowLabels) + " " + b(p.EnvSync)
}

// PluginClient sends commands to the X-Plane plugin's control listener and
// keeps track of the latest status it has pushed back.
type PluginClient struct {
	mu                  sync.Mutex
	formationStatus     string
	formationCode       string
	sharedCockpitStatus string
	sharedCockpitCode   string
	// sharedCockpitOwnership maps a category name ("engine"/"avionics"/
	// "systems") to "me" or "peer" - see control_listener.h's
	// SHARED_COCKPIT_OWNERSHIP line. Nil until the plugin has pushed one.
	sharedCockpitOwnership map[string]string
	formationPeers         []FormationPeer
	linkQuality            LinkQuality
	sharedCockpitMismatch  string // "" if none - see control_listener.h's SHARED_COCKPIT_AIRCRAFT_MISMATCH
	simReady               bool
	runningVersion         string
	cslStatus              string // "" until the plugin has pushed one - see control_listener.h's CSL_STATUS
	prefsEncoded           string // raw PREFS value, "" until pushed
	ownIcao                string
}

// LinkQuality mirrors control_listener.h's LINK_QUALITY line - each RTT is
// nil when the plugin reported "?" (not yet measured, e.g. no session or no
// keepalive round trip completed yet), and PeerLoss/MasterLoss are omitted
// (nil map / nil pointer) the same way, rather than defaulting to a
// misleading 0.
type LinkQuality struct {
	FormationServerRttMs *int64         `json:"formationServerRttMs,omitempty"`
	FormationPeerLossPct map[uint32]int `json:"formationPeerLossPct,omitempty"`
	// "direct" or "relay" per aircraft (sender_id) - see LINK_QUALITY's
	// formation_peer_path. Missing for LAN peers and before the first packet.
	FormationPeerPath          map[uint32]string `json:"formationPeerPath,omitempty"`
	SharedCockpitServerRttMs   *int64            `json:"sharedCockpitServerRttMs,omitempty"`
	SharedCockpitMasterLossPct *int              `json:"sharedCockpitMasterLossPct,omitempty"`
	SharedCockpitPath          string            `json:"sharedCockpitPath,omitempty"` // "direct"/"relay", "" = unknown
}

func NewPluginClient() *PluginClient {
	return &PluginClient{
		formationStatus:     "unknown (plugin not seen yet)",
		sharedCockpitStatus: "unknown (plugin not seen yet)",
		// Starts false rather than true: until the plugin actually says
		// otherwise, assume X-Plane might still be on a loading screen -
		// see control_listener.h's SIM_READY comment.
		simReady: false,
	}
}

// Send fires a single command line at the plugin. UDP is fire-and-forget -
// a successful Send only means the packet left this machine's loopback
// interface, not that the plugin is actually running/listening.
func (c *PluginClient) Send(line string) error {
	conn, err := net.Dial("udp", fmt.Sprintf("127.0.0.1:%d", pluginPort))
	if err != nil {
		return err
	}
	defer conn.Close()
	_, err = conn.Write([]byte(line + "\n"))
	return err
}

func (c *PluginClient) Status() (formation, sharedCockpit string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.formationStatus, c.sharedCockpitStatus
}

// Codes returns the current Formation/Shared Cockpit session codes (each
// "" if none is active yet) - see control_listener.h's *_CODE lines. Used
// to auto-fill the session-code input field when you create a session,
// instead of you reading it out of the status text.
func (c *PluginClient) Codes() (formationCode, sharedCockpitCode string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.formationCode, c.sharedCockpitCode
}

// FormationPeers returns the currently-tracked Formation peers and their
// aircraft types (see control_listener.h's PEERS line). Never nil (an
// empty slice when there are none), so it marshals to JSON `[]` rather
// than `null`.
func (c *PluginClient) FormationPeers() []FormationPeer {
	c.mu.Lock()
	defer c.mu.Unlock()
	peers := make([]FormationPeer, len(c.formationPeers))
	copy(peers, c.formationPeers)
	return peers
}

// SimReady reports whether the plugin last said X-Plane has finished
// loading a flight (see control_listener.h's SIM_READY line). False
// (including before the plugin has ever been seen) means "don't know
// yet, assume not ready".
func (c *PluginClient) SimReady() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.simReady
}

// SharedCockpitOwnership returns the last-pushed per-category ownership
// ("me"/"peer" keyed by "engine"/"avionics"/"systems" - see
// control_listener.h's SHARED_COCKPIT_OWNERSHIP line), or an empty map
// before Shared Cockpit's dataref sync has actually started. Never nil,
// so it marshals to JSON `{}` rather than `null`.
func (c *PluginClient) SharedCockpitOwnership() map[string]string {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make(map[string]string, len(c.sharedCockpitOwnership))
	for k, v := range c.sharedCockpitOwnership {
		out[k] = v
	}
	return out
}

// RunningVersion returns the version of the plugin currently loaded and
// running in X-Plane (see control_listener.h's PLUGIN_VERSION line), or
// "" if the plugin hasn't been seen yet (X-Plane not running, or not
// loaded there). Distinct from InstalledPluginVersion/
// AvailablePluginVersion: installing an update doesn't take effect until
// X-Plane is restarted, so this can legitimately lag behind those for a
// while.
func (c *PluginClient) RunningVersion() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.runningVersion
}

// CslStatus returns the plugin's last-pushed CSL_STATUS (e.g. "3 model(s)
// loaded" - see control_listener.h), or "" before the plugin has been seen.
func (c *PluginClient) CslStatus() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.cslStatus
}

// PrefsEncoded returns the plugin's last-pushed PREFS value ("" before the
// plugin has been seen) - compared against the wanted settings so they can
// be re-sent after an X-Plane restart.
func (c *PluginClient) PrefsEncoded() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.prefsEncoded
}

// OwnIcao returns the ICAO type of the aircraft currently loaded in
// X-Plane, "" if unknown.
func (c *PluginClient) OwnIcao() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.ownIcao
}

// LinkQuality returns the last-pushed LINK_QUALITY reading (see
// control_listener.h's wire-format comment) - a zero-value LinkQuality
// (every field nil/empty) before the plugin has ever pushed one.
func (c *PluginClient) LinkQuality() LinkQuality {
	c.mu.Lock()
	defer c.mu.Unlock()
	// Shallow copy is enough: FormationPeerLossPct is only ever replaced
	// wholesale by applyStatusMessage below, never mutated in place.
	return c.linkQuality
}

// SharedCockpitAircraftMismatch returns "<own icao>:<master icao>" if the
// client detected it's flying a different aircraft type than the master
// (see control_listener.h's SHARED_COCKPIT_AIRCRAFT_MISMATCH), or "" if
// there's no mismatch (including "not a client", "no session", or "not
// measured yet" - the plugin doesn't distinguish those cases either).
func (c *PluginClient) SharedCockpitAircraftMismatch() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sharedCockpitMismatch
}

// applyStatusMessage updates stored status from one received UDP payload
// (which may contain multiple "KEY value" lines). Pure/side-effect-only on
// the receiver, so it's testable without a real socket - see
// plugin_client_test.go.
func (c *PluginClient) applyStatusMessage(payload string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, rawLine := range strings.Split(payload, "\n") {
		// Only trim CRLF line-ending noise, not all whitespace: an
		// intentionally-empty value (e.g. "FORMATION_CODE " when there's
		// no session yet) ends in exactly one space that
		// strings.TrimSpace would eat, which used to make key/value
		// splitting below silently fail to match at all for that case.
		line := strings.TrimRight(rawLine, "\r")
		key, value, found := strings.Cut(line, " ")
		if !found {
			continue // not a "KEY value" line (also skips blank lines)
		}
		switch key {
		case "FORMATION_CODE":
			c.formationCode = value
		case "FORMATION":
			c.formationStatus = value
		case "SHARED_COCKPIT_CODE":
			c.sharedCockpitCode = value
		case "SHARED_COCKPIT":
			c.sharedCockpitStatus = value
		case "SHARED_COCKPIT_OWNERSHIP":
			c.sharedCockpitOwnership = parseSharedCockpitOwnership(value)
		case "PEERS":
			c.formationPeers = parseFormationPeers(value)
		case "LINK_QUALITY":
			c.linkQuality = parseLinkQuality(value)
		case "SHARED_COCKPIT_AIRCRAFT_MISMATCH":
			c.sharedCockpitMismatch = value
		case "SIM_READY":
			c.simReady = value == "1"
		case "PLUGIN_VERSION":
			c.runningVersion = value
		case "CSL_STATUS":
			c.cslStatus = value
		case "PREFS":
			c.prefsEncoded = value
		case "OWN_ICAO":
			c.ownIcao = value
		}
	}
}

// parseFormationPeers decodes control_listener.h's PEERS wire format:
// "<sender_id>:<icao>:<callsign>;..." (empty string means no peers; the
// callsign part is missing from pre-v0.3 plugins). Malformed entries are skipped rather than failing the whole
// line - a forward-compatible plugin build sending something this
// version doesn't expect shouldn't take down status parsing entirely.
func parseFormationPeers(encoded string) []FormationPeer {
	peers := []FormationPeer{}
	if encoded == "" {
		return peers
	}
	for _, entry := range strings.Split(encoded, ";") {
		idStr, icao, found := strings.Cut(entry, ":")
		if !found {
			continue
		}
		id, err := strconv.ParseUint(idStr, 10, 32)
		if err != nil {
			continue
		}
		icao, callsign, _ := strings.Cut(icao, ":")
		peers = append(peers, FormationPeer{ID: uint32(id), ICAO: icao, Callsign: callsign})
	}
	return peers
}

// parseSharedCockpitOwnership decodes control_listener.h's
// SHARED_COCKPIT_OWNERSHIP wire format: "engine:me avionics:peer
// systems:me" (space-separated "<category>:<me|peer>" pairs; empty string
// before a session has actually started). Malformed entries are skipped
// rather than failing the whole line, same reasoning as
// parseFormationPeers.
func parseSharedCockpitOwnership(encoded string) map[string]string {
	out := map[string]string{}
	if encoded == "" {
		return out
	}
	for _, entry := range strings.Fields(encoded) {
		category, owner, found := strings.Cut(entry, ":")
		if !found {
			continue
		}
		out[category] = owner
	}
	return out
}

// parseLinkQuality decodes control_listener.h's LINK_QUALITY wire format:
// "formation_server_rtt_ms:<ms|?> formation_peer_loss_pct:<id>:<pct>;...
// sc_server_rtt_ms:<ms|?> sc_master_loss_pct:<pct|?>" - "?" for any field
// not yet measured (see the plugin's MaybePushLinkQuality), decoded here as
// a nil pointer/omitted map entry rather than a misleading 0. Malformed
// entries are skipped rather than failing the whole line, same reasoning
// as parseFormationPeers/parseSharedCockpitOwnership.
func parseLinkQuality(encoded string) LinkQuality {
	var lq LinkQuality
	for _, field := range strings.Fields(encoded) {
		key, value, found := strings.Cut(field, ":")
		if !found {
			continue
		}
		switch key {
		case "formation_server_rtt_ms":
			lq.FormationServerRttMs = parseOptionalMs(value)
		case "sc_server_rtt_ms":
			lq.SharedCockpitServerRttMs = parseOptionalMs(value)
		case "sc_master_loss_pct":
			lq.SharedCockpitMasterLossPct = parseOptionalPct(value)
		case "formation_peer_loss_pct":
			lq.FormationPeerLossPct = parsePeerLossPct(value)
		case "formation_peer_path":
			lq.FormationPeerPath = parsePeerPath(value)
		case "sc_path":
			if value == "direct" || value == "relay" {
				lq.SharedCockpitPath = value
			}
		}
	}
	return lq
}

// parseOptionalMs parses a LINK_QUALITY millisecond field, or nil for "?".
func parseOptionalMs(value string) *int64 {
	if value == "?" {
		return nil
	}
	ms, err := strconv.ParseInt(value, 10, 64)
	if err != nil {
		return nil
	}
	return &ms
}

// parseOptionalPct parses a LINK_QUALITY percentage field, or nil for "?".
func parseOptionalPct(value string) *int {
	if value == "?" {
		return nil
	}
	pct, err := strconv.Atoi(value)
	if err != nil {
		return nil
	}
	return &pct
}

// parsePeerPath decodes formation_peer_path's "<sender_id>:<direct|relay>;..."
// sub-field.
func parsePeerPath(encoded string) map[uint32]string {
	if encoded == "" {
		return nil
	}
	out := map[uint32]string{}
	for _, entry := range strings.Split(encoded, ";") {
		idStr, path, found := strings.Cut(entry, ":")
		if !found || (path != "direct" && path != "relay") {
			continue
		}
		id, err := strconv.ParseUint(idStr, 10, 32)
		if err != nil {
			continue
		}
		out[uint32(id)] = path
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

// parsePeerLossPct decodes the "formation_peer_loss_pct" sub-field's own
// "<sender_id>:<pct>;<sender_id>:<pct>;..." format - same shape as
// control_listener.h's top-level PEERS line, just nested one level here
// since LINK_QUALITY packs several independent readings onto one line.
func parsePeerLossPct(encoded string) map[uint32]int {
	if encoded == "" {
		return nil
	}
	out := map[uint32]int{}
	for _, entry := range strings.Split(encoded, ";") {
		idStr, pctStr, found := strings.Cut(entry, ":")
		if !found {
			continue
		}
		id, err := strconv.ParseUint(idStr, 10, 32)
		if err != nil {
			continue
		}
		pct, err := strconv.Atoi(pctStr)
		if err != nil {
			continue
		}
		out[uint32(id)] = pct
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

// ListenForStatus blocks forever applying every status push the plugin
// sends to 127.0.0.1:companionPort. Run it in a goroutine.
//
// Keeps retrying the bind instead of giving up on the first failure: right
// after "Update & Restart" the new process starts while the old one may
// still hold the port for a moment, and a single failed attempt used to
// leave the freshly updated app permanently without plugin status.
func (c *PluginClient) ListenForStatus() {
	addr := net.UDPAddr{Port: companionPort, IP: net.ParseIP("127.0.0.1")}
	var conn *net.UDPConn
	for attempt := 0; ; attempt++ {
		var err error
		conn, err = net.ListenUDP("udp", &addr)
		if err == nil {
			break
		}
		if attempt == 0 {
			log.Printf("failed to listen for plugin status on :%d (retrying): %v", companionPort, err)
		}
		time.Sleep(500 * time.Millisecond)
	}
	defer conn.Close()

	buf := make([]byte, 4096)
	for {
		n, _, err := conn.ReadFromUDP(buf)
		if err != nil {
			continue
		}
		c.applyStatusMessage(string(buf[:n]))
	}
}
