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

func TestCreateSessionSendsSpectatorToken(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.CreateSession("rendezvous.example.com:45000", true); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "CREATE_SESSION rendezvous.example.com:45000 SPECTATOR" {
		t.Fatalf("unexpected command line: %q", got)
	}
}

func TestCreateSessionOmitsSpectatorTokenByDefault(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.CreateSession("rendezvous.example.com:45000", false); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "CREATE_SESSION rendezvous.example.com:45000" {
		t.Fatalf("unexpected command line: %q", got)
	}
}

func TestJoinSessionSendsSpectatorToken(t *testing.T) {
	_, recv := recvLine(t)
	app := NewApp()

	if err := app.JoinSession("rendezvous.example.com:45000", "ABC123", true); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := recv(); got != "JOIN_SESSION rendezvous.example.com:45000 ABC123 SPECTATOR" {
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

func TestIsIdleStatus(t *testing.T) {
	idle := []string{"not connected", "not started", "unknown (plugin not seen yet)"}
	for _, s := range idle {
		if !isIdleStatus(s) {
			t.Errorf("expected %q to be idle", s)
		}
	}
	active := []string{"connecting to rendezvous.example.com:45000...", "connected, code 'ABC123', peer 1",
		"connection lost, reconnecting...", "waiting for co-pilot, code 'ABC123'", "running as MASTER"}
	for _, s := range active {
		if isIdleStatus(s) {
			t.Errorf("expected %q to NOT be idle", s)
		}
	}
}

func TestMaybePersistFormationSavesOnceActiveWithACode(t *testing.T) {
	useTempConfigDir(t)
	app := NewApp()

	// Idle or codeless: nothing persisted yet.
	app.maybePersistFormation("not connected", "")
	app.maybePersistFormation("connecting to rendezvous.example.com:45000...", "")
	if loadConfig().LastFormation != nil {
		t.Fatalf("expected nothing persisted yet, got %+v", loadConfig().LastFormation)
	}

	// CreateSession/JoinSession records the server; an active status with
	// a code then persists both together.
	_ = app.CreateSession("rendezvous.example.com:45000", false)
	app.maybePersistFormation("connected, code 'ABC123', peer 1", "ABC123")

	got := loadConfig().LastFormation
	want := &PersistedSession{Server: "rendezvous.example.com:45000", Code: "ABC123"}
	if got == nil || *got != *want {
		t.Fatalf("unexpected persisted formation session: got %+v, want %+v", got, want)
	}
}

func TestMaybePersistSharedCockpitRecordsRoleEvenForMaster(t *testing.T) {
	useTempConfigDir(t)
	app := NewApp()

	_ = app.StartSharedCockpit("MASTER", "rendezvous.example.com:45000", "")
	app.maybePersistSharedCockpit("waiting for co-pilot, code 'XYZ789'", "XYZ789")

	got := loadConfig().LastSharedCockpit
	want := &PersistedSession{Server: "rendezvous.example.com:45000", Code: "XYZ789", Role: "MASTER"}
	if got == nil || *got != *want {
		t.Fatalf("unexpected persisted shared cockpit session: got %+v, want %+v", got, want)
	}
}

func TestMaybeAutoRejoinFiresOnceAndSendsRememberedFormationAndClientSessions(t *testing.T) {
	useTempConfigDir(t)
	conn, recv := recvLine(t)

	_ = persistLastFormation(&PersistedSession{Server: "rendezvous.example.com:45000", Code: "ABC123"})
	_ = persistLastSharedCockpit(&PersistedSession{Server: "rendezvous.example.com:45000", Code: "XYZ789", Role: "CLIENT"})

	app := NewApp()
	app.maybeAutoRejoin()

	lines := map[string]bool{}
	lines[recv()] = true
	lines[recv()] = true
	if !lines["JOIN_SESSION rendezvous.example.com:45000 ABC123"] {
		t.Errorf("expected a JOIN_SESSION for the remembered formation session, got %+v", lines)
	}
	if !lines["START_SHARED_COCKPIT CLIENT rendezvous.example.com:45000 XYZ789"] {
		t.Errorf("expected a START_SHARED_COCKPIT CLIENT for the remembered session, got %+v", lines)
	}

	// A second call (e.g. SIM_READY toggling again) must NOT resend -
	// only the very first SIM_READY per app launch triggers this.
	app.maybeAutoRejoin()
	buf := make([]byte, 256)
	_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := conn.Read(buf); err == nil {
		t.Fatal("expected no further commands from a second maybeAutoRejoin call")
	}
}

func TestMaybeAutoRejoinCarriesTheSpectatorFlagThrough(t *testing.T) {
	useTempConfigDir(t)
	_, recv := recvLine(t)

	_ = persistLastFormation(&PersistedSession{Server: "rendezvous.example.com:45000", Code: "ABC123", IsSpectator: true})

	app := NewApp()
	app.maybeAutoRejoin()

	if got := recv(); got != "JOIN_SESSION rendezvous.example.com:45000 ABC123 SPECTATOR" {
		t.Fatalf("expected the remembered spectator flag to be sent, got %q", got)
	}
}

func TestMaybeAutoRejoinSkipsAPersistedMasterSession(t *testing.T) {
	useTempConfigDir(t)
	conn, _ := recvLine(t)

	_ = persistLastSharedCockpit(&PersistedSession{Server: "rendezvous.example.com:45000", Code: "XYZ789", Role: "MASTER"})

	app := NewApp()
	app.maybeAutoRejoin()

	buf := make([]byte, 256)
	_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := conn.Read(buf); err == nil {
		t.Fatal("expected no command to be sent for a persisted MASTER session")
	}
}

func TestMaybeAutoRejoinIsANoOpWithNothingPersisted(t *testing.T) {
	useTempConfigDir(t)
	conn, _ := recvLine(t)

	app := NewApp()
	app.maybeAutoRejoin()

	buf := make([]byte, 256)
	_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := conn.Read(buf); err == nil {
		t.Fatal("expected no command to be sent when nothing was persisted")
	}
}

func TestDisconnectFormationClearsPersistedSession(t *testing.T) {
	useTempConfigDir(t)
	recvLine(t)

	_ = persistLastFormation(&PersistedSession{Server: "rendezvous.example.com:45000", Code: "ABC123"})
	app := NewApp()

	if err := app.DisconnectFormation(); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := loadConfig().LastFormation; got != nil {
		t.Fatalf("expected the persisted formation session to be cleared, got %+v", got)
	}
}

func TestDisconnectSharedCockpitClearsPersistedSession(t *testing.T) {
	useTempConfigDir(t)
	recvLine(t)

	_ = persistLastSharedCockpit(&PersistedSession{Server: "rendezvous.example.com:45000", Code: "XYZ789", Role: "CLIENT"})
	app := NewApp()

	if err := app.DisconnectSharedCockpit(); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got := loadConfig().LastSharedCockpit; got != nil {
		t.Fatalf("expected the persisted shared cockpit session to be cleared, got %+v", got)
	}
}
