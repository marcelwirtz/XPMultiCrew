package main

import (
	"fmt"
	"net"
	"os"
	"strings"
	"time"

	"github.com/hashicorp/mdns"
)

// LAN-Auto-Discovery (docs/plan.md's Phase 4 #6): every running companion
// instance advertises itself over mDNS so others on the same LAN segment
// show up in a "Nearby on LAN" list, without either side needing the
// rendezvous server. Advertising is unconditional (starts at app launch,
// see App.startup) and carries no session state - "an XPMultiCrew instance
// is reachable here" is all mDNS ever says. The actual encrypted link is
// only ever established once a human picks a discovered peer AND types in
// the same manually-shared session code on both sides (see
// LAN_CONNECT_FORMATION in control_listener.h and net/session_crypto.h's
// code-only constructor) - the code itself is never put on the wire here,
// since mDNS traffic is unauthenticated multicast, visible to the whole
// LAN segment.

// lanServiceName is this app's mDNS service type.
const lanServiceName = "_xpmulticrew-formation._udp"

// formationUdpPort mirrors flytogether::kFormationUdpPort
// (netcore/include/flytogether/aircraft_state.h) - the plugin's fixed
// direct-UDP Formation port every discovered peer is reachable on. Nothing
// dynamic to advertise for it (yet), but every peer still needs to be told
// it via mDNS's SRV record rather than hardcoding it a second time on the
// receiving end, so a future per-instance port doesn't need a wire-format
// change.
const formationUdpPort = 49002

// lanInstanceNameSuffix is appended to the mDNS instance name, matching how
// hashicorp/mdns's PTR target is built
// ("<instance>.<service>.<domain>.") - see serviceEntriesToLanPeers.
const lanNameSuffix = "." + lanServiceName + ".local."

// LanPeer is one entry in the "Nearby on LAN" list - see
// discoverLanPeers/serviceEntriesToLanPeers. Host:Port is ready to hand
// straight to LanConnectFormation.
type LanPeer struct {
	Name string `json:"name"` // human-readable instance name (the advertiser's hostname, normally)
	Host string `json:"host"`
	Port int    `json:"port"`
}

// lanAdvertiser owns the background mDNS responder started by
// startLanAdvertise.
type lanAdvertiser struct {
	server       *mdns.Server
	instanceName string
}

// startLanAdvertise makes this machine's XPMultiCrew instance discoverable
// on the LAN. Safe to call once at startup (see App.startup); returns an
// error if mDNS couldn't bind (e.g. multicast disabled/blocked) - callers
// should log and otherwise carry on, exactly like ControlListener.Start's
// "UDP port busy" failure mode elsewhere in this app: LAN discovery not
// working shouldn't take down the rest of the app.
func startLanAdvertise() (*lanAdvertiser, error) {
	name := lanInstanceName()
	service, err := mdns.NewMDNSService(name, lanServiceName, "", "", formationUdpPort, localLanIPv4Addresses(), nil)
	if err != nil {
		return nil, err
	}
	server, err := mdns.NewServer(&mdns.Config{Zone: service})
	if err != nil {
		return nil, err
	}
	return &lanAdvertiser{server: server, instanceName: name}, nil
}

func (a *lanAdvertiser) stop() {
	if a == nil || a.server == nil {
		return
	}
	_ = a.server.Shutdown()
}

// discoverLanPeers browses the LAN for other XPMultiCrew instances for up
// to `timeout`, filtering out this machine's own advertisement (mDNS
// queries see it too, same as anyone else's) - see
// serviceEntriesToLanPeers for the pure decoding logic this wraps.
func discoverLanPeers(timeout time.Duration, ownInstanceName string) ([]LanPeer, error) {
	entriesCh := make(chan *mdns.ServiceEntry, 16)
	var entries []*mdns.ServiceEntry
	done := make(chan struct{})
	go func() {
		for e := range entriesCh {
			entries = append(entries, e)
		}
		close(done)
	}()

	params := mdns.DefaultParams(lanServiceName)
	params.Timeout = timeout
	params.Entries = entriesCh
	err := mdns.Query(params)
	close(entriesCh)
	<-done
	if err != nil {
		return nil, err
	}
	return serviceEntriesToLanPeers(entries, ownInstanceName), nil
}

// serviceEntriesToLanPeers is discoverLanPeers' pure decoding step (no
// networking) - see lan_discovery_test.go. Skips entries with no usable
// IPv4 address (e.g. an IPv6-only responder - LAN_CONNECT_FORMATION's
// <host:port> is IPv4-only for now) and dedupes by host:port (a single mDNS
// query can legitimately deliver the same entry more than once across
// separate answer records).
func serviceEntriesToLanPeers(entries []*mdns.ServiceEntry, ownInstanceName string) []LanPeer {
	peers := []LanPeer{}
	seen := map[string]bool{}
	for _, e := range entries {
		if e == nil || e.AddrV4 == nil {
			continue
		}
		name := strings.TrimSuffix(e.Name, lanNameSuffix)
		if ownInstanceName != "" && name == ownInstanceName {
			continue
		}
		key := fmt.Sprintf("%s:%d", e.AddrV4.String(), e.Port)
		if seen[key] {
			continue
		}
		seen[key] = true
		peers = append(peers, LanPeer{Name: name, Host: e.AddrV4.String(), Port: e.Port})
	}
	return peers
}

// lanInstanceName picks this machine's mDNS instance name - the hostname
// when available (recognizable in a list of several peers), a fixed
// fallback otherwise. Not a secret - see this file's package comment.
func lanInstanceName() string {
	host, err := os.Hostname()
	if err != nil || host == "" {
		return "XPMultiCrew"
	}
	return host
}

// localLanIPv4Addresses collects this machine's own non-loopback IPv4
// addresses directly from its network interfaces, rather than letting
// mdns.NewMDNSService fall back to net.LookupIP(hostname): on a lot of
// Linux setups /etc/hosts maps the hostname straight to 127.0.1.1, which
// would advertise a useless loopback address to every other peer on the
// LAN instead of a real, reachable one.
func localLanIPv4Addresses() []net.IP {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var ips []net.IP
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			ip4 := ip.To4()
			if ip4 == nil || ip4.IsLoopback() {
				continue
			}
			ips = append(ips, ip4)
		}
	}
	return ips
}
