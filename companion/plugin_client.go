package main

import (
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
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
	ID   uint32 `json:"id"`
	ICAO string `json:"icao"`
}

// PluginClient sends commands to the X-Plane plugin's control listener and
// keeps track of the latest status it has pushed back.
type PluginClient struct {
	mu                  sync.Mutex
	formationStatus     string
	formationCode       string
	sharedCockpitStatus string
	sharedCockpitCode   string
	formationPeers      []FormationPeer
	simReady            bool
	runningVersion      string
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
		case "PEERS":
			c.formationPeers = parseFormationPeers(value)
		case "SIM_READY":
			c.simReady = value == "1"
		case "PLUGIN_VERSION":
			c.runningVersion = value
		}
	}
}

// parseFormationPeers decodes control_listener.h's PEERS wire format:
// "<sender_id>:<icao>;<sender_id>:<icao>;..." (empty string means no
// peers). Malformed entries are skipped rather than failing the whole
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
		peers = append(peers, FormationPeer{ID: uint32(id), ICAO: icao})
	}
	return peers
}

// ListenForStatus blocks forever applying every status push the plugin
// sends to 127.0.0.1:companionPort. Run it in a goroutine.
func (c *PluginClient) ListenForStatus() {
	addr := net.UDPAddr{Port: companionPort, IP: net.ParseIP("127.0.0.1")}
	conn, err := net.ListenUDP("udp", &addr)
	if err != nil {
		log.Printf("failed to listen for plugin status on :%d: %v", companionPort, err)
		return
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
