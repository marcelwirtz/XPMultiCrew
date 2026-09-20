package main

import (
	"net"
	"strings"
	"testing"
	"time"
)

// recvLine binds a raw UDP socket standing in for the plugin's control
// listener (mirrors tests/control_listener_test.cpp's approach on the C++
// side) and returns the first line it receives, or fails the test after a
// short timeout.
func recvLine(t *testing.T) (*net.UDPConn, func() string) {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{Port: pluginPort, IP: net.ParseIP("127.0.0.1")})
	if err != nil {
		t.Fatalf("failed to bind plugin port %d (something else already listening?): %v", pluginPort, err)
	}
	t.Cleanup(func() { _ = conn.Close() })

	return conn, func() string {
		buf := make([]byte, 256)
		_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		n, err := conn.Read(buf)
		if err != nil {
			t.Fatalf("did not receive a UDP packet in time: %v", err)
		}
		return strings.TrimSpace(string(buf[:n]))
	}
}

func TestStartSharedCockpitMasterOmitsCode(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.StartSharedCockpit("MASTER", "rendezvous.example.com:45000", ""); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "START_SHARED_COCKPIT MASTER rendezvous.example.com:45000" {
		t.Fatalf("unexpected command line: %q", got)
	}
}

func TestStartSharedCockpitClientSendsCode(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.StartSharedCockpit("CLIENT", "rendezvous.example.com:45000", "ABC123"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "START_SHARED_COCKPIT CLIENT rendezvous.example.com:45000 ABC123" {
		t.Fatalf("unexpected command line: %q", got)
	}
}

func TestStartSharedCockpitClientRequiresCode(t *testing.T) {
	app := NewApp()
	if err := app.StartSharedCockpit("CLIENT", "rendezvous.example.com:45000", ""); err == nil {
		t.Fatal("expected an error when joining as CLIENT without a session code")
	}
}

func TestStartSharedCockpitRequiresServer(t *testing.T) {
	app := NewApp()
	if err := app.StartSharedCockpit("MASTER", "", ""); err == nil {
		t.Fatal("expected an error when server address is blank")
	}
}

func TestDisconnectFormationSendsCommand(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.DisconnectFormation(); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "DISCONNECT_FORMATION" {
		t.Fatalf("unexpected command line: %q", got)
	}
}

func TestDisconnectSharedCockpitSendsCommand(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.DisconnectSharedCockpit(); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "DISCONNECT_SHARED_COCKPIT" {
		t.Fatalf("unexpected command line: %q", got)
	}
}

func TestRequestOwnershipSendsCommand(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.RequestOwnership("engine"); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "REQUEST_OWNERSHIP engine" {
		t.Fatalf("unexpected command line: %q", got)
	}
}
