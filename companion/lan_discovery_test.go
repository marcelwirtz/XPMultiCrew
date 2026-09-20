package main

import (
	"net"
	"testing"

	"github.com/hashicorp/mdns"
)

func entry(name string, ip string, port int) *mdns.ServiceEntry {
	var addr net.IP
	if ip != "" {
		addr = net.ParseIP(ip)
	}
	return &mdns.ServiceEntry{
		Name:   name + lanNameSuffix,
		AddrV4: addr,
		Port:   port,
	}
}

func TestServiceEntriesToLanPeersDecodesNameHostPort(t *testing.T) {
	peers := serviceEntriesToLanPeers([]*mdns.ServiceEntry{entry("copilot-pc", "192.168.1.50", 49002)}, "")
	if len(peers) != 1 {
		t.Fatalf("expected 1 peer, got %d: %+v", len(peers), peers)
	}
	if peers[0] != (LanPeer{Name: "copilot-pc", Host: "192.168.1.50", Port: 49002}) {
		t.Fatalf("unexpected peer: %+v", peers[0])
	}
}

func TestServiceEntriesToLanPeersFiltersOutOwnInstance(t *testing.T) {
	peers := serviceEntriesToLanPeers([]*mdns.ServiceEntry{
		entry("my-pc", "192.168.1.10", 49002),
		entry("copilot-pc", "192.168.1.50", 49002),
	}, "my-pc")
	if len(peers) != 1 || peers[0].Name != "copilot-pc" {
		t.Fatalf("expected only copilot-pc to remain, got %+v", peers)
	}
}

func TestServiceEntriesToLanPeersSkipsEntriesWithoutIPv4(t *testing.T) {
	peers := serviceEntriesToLanPeers([]*mdns.ServiceEntry{entry("ipv6-only", "", 49002)}, "")
	if len(peers) != 0 {
		t.Fatalf("expected no peers for an IPv4-less entry, got %+v", peers)
	}
}

func TestServiceEntriesToLanPeersDedupesByHostPort(t *testing.T) {
	peers := serviceEntriesToLanPeers([]*mdns.ServiceEntry{
		entry("copilot-pc", "192.168.1.50", 49002),
		entry("copilot-pc", "192.168.1.50", 49002),
	}, "")
	if len(peers) != 1 {
		t.Fatalf("expected duplicate entries to collapse to 1, got %d: %+v", len(peers), peers)
	}
}

func TestServiceEntriesToLanPeersNeverReturnsNil(t *testing.T) {
	peers := serviceEntriesToLanPeers(nil, "")
	if peers == nil {
		t.Fatal("expected an empty slice, got nil")
	}
	if len(peers) != 0 {
		t.Fatalf("expected no peers, got %+v", peers)
	}
}

func TestServiceEntriesToLanPeersIgnoresNilEntries(t *testing.T) {
	peers := serviceEntriesToLanPeers([]*mdns.ServiceEntry{nil, entry("copilot-pc", "192.168.1.50", 49002)}, "")
	if len(peers) != 1 || peers[0].Name != "copilot-pc" {
		t.Fatalf("expected the nil entry to be skipped, got %+v", peers)
	}
}
