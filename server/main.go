// Command xpmulticrew-rendezvous is the rendezvous/relay server from
// docs/plan.md section 7: session/room management, peer address discovery
// for client-side UDP hole-punching, and an always-on relay fallback for
// when hole-punching doesn't work (e.g. behind CGNAT).
package main

import (
	"encoding/json"
	"flag"
	"log"
	"net"
	"time"
)

type udpSender struct {
	conn *net.UDPConn
}

func (u *udpSender) SendTo(addr *net.UDPAddr, msg ServerMessage) {
	data, err := json.Marshal(msg)
	if err != nil {
		log.Printf("marshal error: %v", err)
		return
	}
	if _, err := u.conn.WriteToUDP(data, addr); err != nil {
		log.Printf("write error to %s: %v", addr, err)
	}
}

func main() {
	port := flag.Int("port", 45000, "UDP port to listen on")
	flag.Parse()

	addr := &net.UDPAddr{Port: *port, IP: net.IPv4zero}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		log.Fatalf("failed to listen on UDP port %d: %v", *port, err)
	}
	defer conn.Close()
	log.Printf("XPMultiCrew rendezvous server listening on UDP :%d", *port)

	server := NewServer(&udpSender{conn: conn})

	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for now := range ticker.C {
			server.SweepStaleClients(now)
		}
	}()

	// Anyone can send garbage to a public UDP port; logging every datagram
	// would let them fill the disk/journal. At most a few lines a minute,
	// plus a count of what was suppressed.
	logLimiter := newLogLimiter(10, time.Minute)
	buf := make([]byte, 4096)
	for {
		n, clientAddr, err := conn.ReadFromUDP(buf)
		if err != nil {
			logLimiter.Printf("read error: %v", err)
			continue
		}
		var msg ClientMessage
		if err := json.Unmarshal(buf[:n], &msg); err != nil {
			logLimiter.Printf("bad message from %s: %v", clientAddr, err)
			continue
		}
		server.HandleMessage(clientAddr, msg)
	}
}
