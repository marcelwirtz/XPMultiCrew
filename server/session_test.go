package main

import (
	"encoding/base64"
	"net"
	"testing"
	"time"
)

type sentMsg struct {
	addr string
	msg  ServerMessage
}

type mockSender struct {
	sent []sentMsg
}

func (m *mockSender) SendTo(addr *net.UDPAddr, msg ServerMessage) {
	m.sent = append(m.sent, sentMsg{addr: addr.String(), msg: msg})
}

func (m *mockSender) messagesTo(addr *net.UDPAddr) []ServerMessage {
	var out []ServerMessage
	for _, s := range m.sent {
		if s.addr == addr.String() {
			out = append(out, s.msg)
		}
	}
	return out
}

func addrFor(port int) *net.UDPAddr {
	return &net.UDPAddr{IP: net.ParseIP("203.0.113.1"), Port: port}
}

func TestCreateSessionAssignsIdOne(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(10001)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})

	msgs := sender.messagesTo(a)
	if len(msgs) != 1 || msgs[0].Type != MsgSessionCreated || msgs[0].YourID != 1 {
		t.Fatalf("expected one session_created with your_id=1, got %+v", msgs)
	}
	if msgs[0].Code == "" {
		t.Fatal("expected a non-empty session code")
	}
}

func TestJoinSessionNotifiesBothSides(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(10001)
	b := addrFor(10002)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code

	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code})

	bMsgs := sender.messagesTo(b)
	foundPeerJoined, foundWelcome := false, false
	for _, m := range bMsgs {
		if m.Type == MsgPeerJoined && m.PeerID == 1 {
			foundPeerJoined = true
		}
		if m.Type == MsgSessionCreated && m.YourID == 2 && m.Code == code {
			foundWelcome = true
		}
	}
	if !foundPeerJoined || !foundWelcome {
		t.Fatalf("b did not get expected messages: %+v", bMsgs)
	}

	aMsgs := sender.messagesTo(a)
	foundPeerJoinedForA := false
	for _, m := range aMsgs {
		if m.Type == MsgPeerJoined && m.PeerID == 2 && m.PeerAddr == b.String() {
			foundPeerJoinedForA = true
		}
	}
	if !foundPeerJoinedForA {
		t.Fatalf("a was not notified of b joining: %+v", aMsgs)
	}
}

func TestThreeWayMeshDiscovery(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a, b, c := addrFor(1), addrFor(2), addrFor(3)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code})
	server.HandleMessage(c, ClientMessage{Type: MsgJoinSession, Code: code})

	cMsgs := sender.messagesTo(c)
	peers := map[int]bool{}
	for _, m := range cMsgs {
		if m.Type == MsgPeerJoined {
			peers[m.PeerID] = true
		}
	}
	if !peers[1] || !peers[2] {
		t.Fatalf("c should know about peers 1 and 2 (not hardcoded to 2 participants), got %+v", cMsgs)
	}
}

func TestRelayFansOutToOthersOnly(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a, b, c := addrFor(1), addrFor(2), addrFor(3)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code})
	server.HandleMessage(c, ClientMessage{Type: MsgJoinSession, Code: code})

	sender.sent = nil // discard join-phase messages, focus on relay
	server.HandleMessage(a, ClientMessage{Type: MsgRelay, Payload: "aGVsbG8="})

	if msgs := sender.messagesTo(a); len(msgs) != 0 {
		t.Fatalf("sender should not receive its own relay, got %+v", msgs)
	}
	bMsgs := sender.messagesTo(b)
	if len(bMsgs) != 1 || bMsgs[0].Type != MsgRelay || bMsgs[0].FromPeerID != 1 || bMsgs[0].Payload != "aGVsbG8=" {
		t.Fatalf("b did not get the relayed payload: %+v", bMsgs)
	}
	cMsgs := sender.messagesTo(c)
	if len(cMsgs) != 1 || cMsgs[0].FromPeerID != 1 {
		t.Fatalf("c did not get the relayed payload: %+v", cMsgs)
	}
}

func TestJoinUnknownSessionReturnsError(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	server.HandleMessage(a, ClientMessage{Type: MsgJoinSession, Code: "NOPE99"})

	msgs := sender.messagesTo(a)
	if len(msgs) != 1 || msgs[0].Type != MsgError {
		t.Fatalf("expected one error message, got %+v", msgs)
	}
}

func TestStaleClientIsEvictedAndOthersNotified(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a, b := addrFor(1), addrFor(2)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code})

	server.mu.Lock()
	server.clientsByAddr[b.String()].LastSeen = time.Now().Add(-1 * time.Hour)
	server.mu.Unlock()

	sender.sent = nil
	server.SweepStaleClients(time.Now())

	aMsgs := sender.messagesTo(a)
	found := false
	for _, m := range aMsgs {
		if m.Type == MsgPeerLeft && m.PeerID == 2 {
			found = true
		}
	}
	if !found {
		t.Fatalf("a should have been told b left: %+v", aMsgs)
	}

	server.mu.Lock()
	_, stillThere := server.sessions[code].Members[2]
	server.mu.Unlock()
	if stillThere {
		t.Fatal("b should have been removed from the session")
	}
}

func TestSessionDeletedWhenEmpty(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code

	server.mu.Lock()
	server.clientsByAddr[a.String()].LastSeen = time.Now().Add(-1 * time.Hour)
	server.mu.Unlock()

	server.SweepStaleClients(time.Now())

	server.mu.Lock()
	_, exists := server.sessions[code]
	server.mu.Unlock()
	if exists {
		t.Fatal("empty session should have been deleted")
	}
}

func TestLeaveSessionNotifiesRemainingMembersImmediately(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a, b := addrFor(1), addrFor(2)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code})

	sender.sent = nil
	server.HandleMessage(b, ClientMessage{Type: MsgLeaveSession})

	aMsgs := sender.messagesTo(a)
	found := false
	for _, m := range aMsgs {
		if m.Type == MsgPeerLeft && m.PeerID == 2 {
			found = true
		}
	}
	if !found {
		t.Fatalf("a should have been told b left immediately, got: %+v", aMsgs)
	}

	server.mu.Lock()
	_, stillThere := server.sessions[code].Members[2]
	_, stillTracked := server.clientsByAddr[b.String()]
	server.mu.Unlock()
	if stillThere {
		t.Fatal("b should have been removed from the session")
	}
	if stillTracked {
		t.Fatal("b should have been removed from clientsByAddr")
	}
}

func TestLeaveSessionDeletesEmptySession(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code

	server.HandleMessage(a, ClientMessage{Type: MsgLeaveSession})

	server.mu.Lock()
	_, exists := server.sessions[code]
	server.mu.Unlock()
	if exists {
		t.Fatal("session should have been deleted once its last member left")
	}
}

func TestLeaveSessionFromUnknownClientIsHarmless(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	// Not in any session - must not panic or send anything back (a plugin
	// might send this defensively on shutdown even if it never actually
	// joined a session).
	server.HandleMessage(a, ClientMessage{Type: MsgLeaveSession})

	if msgs := sender.messagesTo(a); len(msgs) != 0 {
		t.Fatalf("expected no messages, got: %+v", msgs)
	}
}

func TestRelayFromUnknownClientReturnsError(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	server.HandleMessage(a, ClientMessage{Type: MsgRelay, Payload: "eA=="})

	msgs := sender.messagesTo(a)
	if len(msgs) != 1 || msgs[0].Type != MsgError {
		t.Fatalf("expected an error for relay from a client not in any session, got %+v", msgs)
	}
}

func TestKeepaliveIsAckedForAKnownClient(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(10001)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	server.HandleMessage(a, ClientMessage{Type: MsgKeepalive})

	msgs := sender.messagesTo(a)
	if len(msgs) != 2 || msgs[1].Type != MsgKeepaliveAck {
		t.Fatalf("expected session_created then keepalive_ack, got %+v", msgs)
	}
}

func TestJoinSessionIsRateLimitedPerSourceIP(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1) // creates the session so there's a valid code to (not) find
	attacker := addrFor(9999)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code
	sender.sent = nil

	// The first maxJoinAttemptsPerWindow attempts (even wrong-code ones)
	// go through normally and get the usual "no such session" error, not
	// the rate-limit error - guessing a handful of codes must not be
	// distinguishable from being throttled.
	for i := 0; i < maxJoinAttemptsPerWindow; i++ {
		server.HandleMessage(attacker, ClientMessage{Type: MsgJoinSession, Code: "WRONG1"})
	}
	msgs := sender.messagesTo(attacker)
	if len(msgs) != maxJoinAttemptsPerWindow {
		t.Fatalf("expected %d responses before throttling, got %d: %+v",
			maxJoinAttemptsPerWindow, len(msgs), msgs)
	}
	for _, m := range msgs {
		if m.Type != MsgError || m.Message == "" {
			t.Fatalf("expected ordinary 'no such session' errors before throttling, got %+v", m)
		}
	}

	// The next attempt from the same source IP - even with the *correct*
	// code this time - is throttled instead of actually joining. This is
	// the core property: once over the limit, guessing correctly doesn't
	// help within the same window.
	sender.sent = nil
	server.HandleMessage(attacker, ClientMessage{Type: MsgJoinSession, Code: code})
	throttled := sender.messagesTo(attacker)
	if len(throttled) != 1 || throttled[0].Type != MsgError {
		t.Fatalf("expected a single throttling error, got %+v", throttled)
	}

	server.mu.Lock()
	_, joined := server.sessions[code].Members[2]
	server.mu.Unlock()
	if joined {
		t.Fatal("a throttled join_session must not actually join the session, even with the right code")
	}

	// A different source IP is unaffected by the first one being throttled
	// (addrFor always returns the same IP, so build one with a different
	// IP directly).
	other := &net.UDPAddr{IP: net.ParseIP("198.51.100.7"), Port: 1}
	sender.sent = nil
	server.HandleMessage(other, ClientMessage{Type: MsgJoinSession, Code: code})
	otherMsgs := sender.messagesTo(other)
	otherJoined := false
	for _, m := range otherMsgs {
		if m.Type == MsgSessionCreated {
			otherJoined = true
		}
	}
	if !otherJoined {
		t.Fatalf("a different source IP should not be affected by another IP's throttling, got %+v", otherMsgs)
	}
}

func TestCreateSessionReturnsANonEmptySalt(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession, ClientVersion: ProtocolVersion})

	msg := sender.messagesTo(a)[0]
	if msg.Salt == "" {
		t.Fatal("expected a non-empty salt on session_created")
	}
	decoded, err := base64.StdEncoding.DecodeString(msg.Salt)
	if err != nil || len(decoded) != sessionSaltSize {
		t.Fatalf("salt should be %d base64-decoded bytes, got %q (err=%v)", sessionSaltSize, msg.Salt, err)
	}
}

func TestJoinerGetsTheSameSaltAsTheCreator(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a, b := addrFor(1), addrFor(2)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession, ClientVersion: ProtocolVersion})
	created := sender.messagesTo(a)[0]

	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: created.Code, ClientVersion: ProtocolVersion})
	bMsgs := sender.messagesTo(b)
	var joined ServerMessage
	for _, m := range bMsgs {
		if m.Type == MsgSessionCreated {
			joined = m
		}
	}
	if joined.Salt == "" {
		t.Fatal("expected a non-empty salt on the joiner's session_created too")
	}
	if joined.Salt != created.Salt {
		t.Fatalf("joiner's salt (%q) should match the creator's (%q) - both derive the same key from it",
			joined.Salt, created.Salt)
	}
}

func TestJoinWithMismatchedClientVersionIsRejected(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a, b := addrFor(1), addrFor(2)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession, ClientVersion: ProtocolVersion})
	code := sender.messagesTo(a)[0].Code

	sender.sent = nil
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code, ClientVersion: ProtocolVersion + 1})

	bMsgs := sender.messagesTo(b)
	if len(bMsgs) != 1 || bMsgs[0].Type != MsgError {
		t.Fatalf("expected a single rejection error for a version mismatch, got %+v", bMsgs)
	}

	// The mismatched joiner must not actually end up in the session.
	server.mu.Lock()
	_, joined := server.sessions[code].Members[2]
	server.mu.Unlock()
	if joined {
		t.Fatal("a version-mismatched joiner must not be added to the session")
	}

	// The existing member must not have been told about a joiner that was
	// actually rejected.
	aMsgs := sender.messagesTo(a)
	if len(aMsgs) != 0 {
		t.Fatalf("creator should not be notified of a rejected join attempt, got %+v", aMsgs)
	}
}

func TestSpectatorRoleIsRelayedToOtherMembersViaPeerJoined(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	pilot, spectator := addrFor(1), addrFor(2)

	server.HandleMessage(pilot, ClientMessage{Type: MsgCreateSession, ClientVersion: ProtocolVersion})
	code := sender.messagesTo(pilot)[0].Code

	sender.sent = nil
	server.HandleMessage(spectator, ClientMessage{
		Type: MsgJoinSession, Code: code, ClientVersion: ProtocolVersion, Role: "spectator",
	})

	// The pilot is told the joiner is a spectator...
	pilotMsgs := sender.messagesTo(pilot)
	foundSpectatorNotice := false
	for _, m := range pilotMsgs {
		if m.Type == MsgPeerJoined && m.PeerID == 2 && m.PeerIsSpectator {
			foundSpectatorNotice = true
		}
	}
	if !foundSpectatorNotice {
		t.Fatalf("expected the pilot to be told peer 2 is a spectator, got %+v", pilotMsgs)
	}

	// ...and the spectator itself is told the existing member is an
	// ordinary pilot (PeerIsSpectator: false, not just absent).
	spectatorMsgs := sender.messagesTo(spectator)
	foundPilotNotice := false
	for _, m := range spectatorMsgs {
		if m.Type == MsgPeerJoined && m.PeerID == 1 && !m.PeerIsSpectator {
			foundPilotNotice = true
		}
	}
	if !foundPilotNotice {
		t.Fatalf("expected the spectator to be told peer 1 is NOT a spectator, got %+v", spectatorMsgs)
	}

	server.mu.Lock()
	isSpectator := server.sessions[code].Members[2].IsSpectator
	server.mu.Unlock()
	if !isSpectator {
		t.Fatal("expected the joiner's own ClientState.IsSpectator to be true")
	}
}

func TestDefaultRoleIsNotASpectator(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession, ClientVersion: ProtocolVersion})
	code := sender.messagesTo(a)[0].Code

	server.mu.Lock()
	isSpectator := server.sessions[code].Members[1].IsSpectator
	server.mu.Unlock()
	if isSpectator {
		t.Fatal("expected a plain create_session (no Role) to default to a non-spectator")
	}
}

func TestKeepaliveFromUnknownClientIsHarmless(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(1)

	// Not in any session (e.g. a keepalive that arrives just after the
	// server already evicted this client) - must not panic or send
	// anything back, same as leave_session's equivalent case.
	server.HandleMessage(a, ClientMessage{Type: MsgKeepalive})

	if msgs := sender.messagesTo(a); len(msgs) != 0 {
		t.Fatalf("expected no messages, got: %+v", msgs)
	}
}

// The plugin's auto-reconnect re-sends join_session from the same socket
// after its own 40s silence timeout, while the server (120s timeout) still
// has it as a member. That must resume the existing membership, not add a
// second one for the same address.
func TestRejoinFromSameAddressResumesMembership(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(10001)
	b := addrFor(10002)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	code := sender.messagesTo(a)[0].Code
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code})

	sender.sent = nil
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: code})

	if n := len(server.sessions[code].Members); n != 2 {
		t.Fatalf("expected 2 members after rejoin, got %d", n)
	}
	if msgs := sender.messagesTo(a); len(msgs) != 0 {
		t.Fatalf("a should not be notified of b's resume, got %+v", msgs)
	}
	bMsgs := sender.messagesTo(b)
	gotPeerA, gotWelcome := false, false
	for _, m := range bMsgs {
		if m.Type == MsgPeerJoined && m.PeerID == 2 {
			t.Fatalf("b was told about itself: %+v", bMsgs)
		}
		if m.Type == MsgPeerJoined && m.PeerID == 1 {
			gotPeerA = true
		}
		if m.Type == MsgSessionCreated && m.YourID == 2 {
			gotWelcome = true
		}
	}
	if !gotPeerA || !gotWelcome {
		t.Fatalf("b did not get peer list + session_created with its old id: %+v", bMsgs)
	}
}

func TestCreateOrJoinOtherSessionLeavesOldOne(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	a := addrFor(10001)
	b := addrFor(10002)
	c := addrFor(10003)

	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	codeA := sender.messagesTo(a)[0].Code
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: codeA})
	server.HandleMessage(c, ClientMessage{Type: MsgCreateSession})
	codeC := sender.messagesTo(c)[0].Code

	sender.sent = nil
	server.HandleMessage(b, ClientMessage{Type: MsgJoinSession, Code: codeC})

	if n := len(server.sessions[codeA].Members); n != 1 {
		t.Fatalf("b should have left session A, it has %d members", n)
	}
	gotLeft := false
	for _, m := range sender.messagesTo(a) {
		if m.Type == MsgPeerLeft && m.PeerID == 2 {
			gotLeft = true
		}
	}
	if !gotLeft {
		t.Fatal("a was not told that b left")
	}

	// Creating a new session leaves the old one too - and the now-empty
	// session A goes away entirely.
	server.HandleMessage(a, ClientMessage{Type: MsgCreateSession})
	if _, ok := server.sessions[codeA]; ok {
		t.Fatal("empty session A should have been deleted")
	}
}

func TestCreateSessionIsRateLimitedPerIP(t *testing.T) {
	sender := &mockSender{}
	server := NewServer(sender)
	for i := 0; i < maxCreateAttemptsPerWindow+1; i++ {
		server.HandleMessage(addrFor(20000+i), ClientMessage{Type: MsgCreateSession})
	}
	last := sender.messagesTo(addrFor(20000 + maxCreateAttemptsPerWindow))
	if len(last) != 1 || last[0].Type != MsgError {
		t.Fatalf("expected the create over the limit to be refused, got %+v", last)
	}
	if n := len(server.sessions); n != maxCreateAttemptsPerWindow {
		t.Fatalf("expected %d sessions, got %d", maxCreateAttemptsPerWindow, n)
	}
}
