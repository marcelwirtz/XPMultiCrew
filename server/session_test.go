package main

import (
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
