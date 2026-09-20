package main

import (
	"crypto/rand"
	"encoding/base64"
	"fmt"
	"net"
	"sync"
	"time"
)

// sessionSaltSize matches SessionCrypto::kSaltSize (plugin/include/net/
// session_crypto.h) - the two are never actually compared byte-for-byte
// by anything, but keeping the constant name/value pair documented in
// both places avoids the two silently drifting apart.
const sessionSaltSize = 16

// Excludes visually-confusing characters (0/O, 1/I), matching the plan's
// "short, human-typeable code, like SmartCopilot's Connection-ID" (section 7).
const sessionCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
const sessionCodeLength = 6

// How long a client can go without a keepalive/relay/join before it's
// considered gone and evicted from its session.
//
// Generous on purpose: the plugin's flight loop (where it polls for our
// responses and sends keepalives) does not run at all while X-Plane is on
// a loading screen (new flight/aircraft/scenery load), which can easily
// take 30-60+ seconds. create_session's reply can sit unread in the
// client's socket buffer that whole time, so the gap between "session
// created" and the client's first keepalive can be much longer than the
// steady-state 10s keepalive interval (RendezvousClient::MaybeSendKeepalive)
// suggests. A too-short timeout here evicts a session before the plugin
// ever gets a chance to keep it alive.
const clientTimeout = 120 * time.Second

// Anti-guessing throttle for join_session: a session code is only 6
// characters from a 33-character alphabet (~1.3 billion combinations, see
// randomSessionCode) - fine against an occasional mistyped code, but not
// against a script trying many codes per second against a server exposed
// on the open internet, where a successful guess means joining someone
// else's Shared Cockpit session and taking over their aircraft. Capping
// how many join_session requests one source IP can make per window
// doesn't stop a distributed attack (and a spoofed UDP source IP could
// evade it entirely), but it turns "guess thousands of codes per second"
// into an impractically slow attack from any single address, at
// essentially no cost to real users - a reconnecting plugin retries at
// most a couple of times per window (see plugin_main.cpp's
// kReconnectRetryInterval).
const (
	joinAttemptWindow        = 10 * time.Second
	maxJoinAttemptsPerWindow = 8
)

// joinAttemptCounter tracks one source IP's join_session requests within
// the current joinAttemptWindow.
type joinAttemptCounter struct {
	count       int
	windowStart time.Time
}

// ClientState is one connected UDP endpoint's membership in a session.
type ClientState struct {
	SessionCode string
	MemberID    int
	Addr        *net.UDPAddr
	LastSeen    time.Time
}

// Session is a group of clients that see each other's peer_joined/relay
// traffic. Not capped at 2 members (docs/plan.md section 5's "open
// participant count" requirement applies here too): create_session starts
// one, join_session can be called by any number of further clients.
type Session struct {
	Code    string
	Members map[int]*ClientState
	nextID  int
	// Salt is minted once, at creation, and handed out unchanged in every
	// session_created for this session (both the creator's and every
	// later joiner's) - see protocol.go's ServerMessage.Salt comment.
	Salt []byte
	// ClientVersion is the creator's ProtocolVersion (protocol.go) - every
	// joiner must match it exactly, see handleJoinSession.
	ClientVersion int
}

func randomSalt() ([]byte, error) {
	salt := make([]byte, sessionSaltSize)
	if _, err := rand.Read(salt); err != nil {
		return nil, err
	}
	return salt, nil
}

// Sender abstracts "send this message to this address" so Server's logic
// can be exercised in tests without a real UDP socket - see session_test.go.
type Sender interface {
	SendTo(addr *net.UDPAddr, msg ServerMessage)
}

// Server holds all rendezvous state. Safe for concurrent use; HandleMessage
// and SweepStaleClients are the two entry points (called from main's UDP
// read loop and a periodic ticker, respectively).
type Server struct {
	sender Sender

	mu               sync.Mutex
	sessions         map[string]*Session
	clientsByAddr    map[string]*ClientState
	joinAttemptsByIP map[string]*joinAttemptCounter
}

func NewServer(sender Sender) *Server {
	return &Server{
		sender:           sender,
		sessions:         make(map[string]*Session),
		clientsByAddr:    make(map[string]*ClientState),
		joinAttemptsByIP: make(map[string]*joinAttemptCounter),
	}
}

// allowJoinAttempt reports whether ip is still under the join_session rate
// limit, incrementing its counter (creating/resetting it if its window has
// elapsed) as a side effect. Caller must hold s.mu.
func (s *Server) allowJoinAttempt(ip string) bool {
	now := time.Now()
	c, ok := s.joinAttemptsByIP[ip]
	if !ok || now.Sub(c.windowStart) >= joinAttemptWindow {
		s.joinAttemptsByIP[ip] = &joinAttemptCounter{count: 1, windowStart: now}
		return true
	}
	c.count++
	return c.count <= maxJoinAttemptsPerWindow
}

func randomSessionCode() (string, error) {
	raw := make([]byte, sessionCodeLength)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	code := make([]byte, sessionCodeLength)
	for i, v := range raw {
		code[i] = sessionCodeAlphabet[int(v)%len(sessionCodeAlphabet)]
	}
	return string(code), nil
}

// HandleMessage processes one incoming client message from addr.
func (s *Server) HandleMessage(addr *net.UDPAddr, msg ClientMessage) {
	switch msg.Type {
	case MsgCreateSession:
		s.handleCreateSession(addr, msg.ClientVersion)
	case MsgJoinSession:
		s.handleJoinSession(addr, msg.Code, msg.ClientVersion)
	case MsgRelay:
		s.handleRelay(addr, msg.Payload)
	case MsgKeepalive:
		s.handleKeepalive(addr)
	case MsgLeaveSession:
		s.handleLeaveSession(addr)
	default:
		s.sender.SendTo(addr, ServerMessage{Type: MsgError, Message: "unknown message type"})
	}
}

func (s *Server) handleCreateSession(addr *net.UDPAddr, clientVersion int) {
	s.mu.Lock()
	defer s.mu.Unlock()

	var code string
	for {
		c, err := randomSessionCode()
		if err != nil {
			s.sender.SendTo(addr, ServerMessage{Type: MsgError, Message: "internal error generating session code"})
			return
		}
		if _, exists := s.sessions[c]; !exists {
			code = c
			break
		}
	}
	salt, err := randomSalt()
	if err != nil {
		s.sender.SendTo(addr, ServerMessage{Type: MsgError, Message: "internal error generating session salt"})
		return
	}

	member := &ClientState{SessionCode: code, MemberID: 1, Addr: addr, LastSeen: time.Now()}
	session := &Session{
		Code: code, Members: map[int]*ClientState{1: member}, nextID: 2,
		Salt: salt, ClientVersion: clientVersion,
	}
	s.sessions[code] = session
	s.clientsByAddr[addr.String()] = member

	s.sender.SendTo(addr, ServerMessage{
		Type: MsgSessionCreated, Code: code, YourID: 1,
		Salt: base64.StdEncoding.EncodeToString(salt),
	})
}

func (s *Server) handleJoinSession(addr *net.UDPAddr, code string, clientVersion int) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if !s.allowJoinAttempt(addr.IP.String()) {
		s.sender.SendTo(addr, ServerMessage{Type: MsgError, Message: "too many join attempts, please wait and try again"})
		return
	}

	session, ok := s.sessions[code]
	if !ok {
		s.sender.SendTo(addr, ServerMessage{Type: MsgError, Message: fmt.Sprintf("no such session: %s", code)})
		return
	}
	if clientVersion != session.ClientVersion {
		// A clear, actionable rejection instead of letting the two sides
		// silently fail to understand each other's payloads (differently
		// keyed/shaped envelopes) once they start exchanging position/
		// dataref traffic - see protocol.go's ProtocolVersion comment.
		s.sender.SendTo(addr, ServerMessage{
			Type:    MsgError,
			Message: "incompatible client version - update both sides to the same XPMultiCrew build",
		})
		return
	}

	newID := session.nextID
	session.nextID++
	member := &ClientState{SessionCode: code, MemberID: newID, Addr: addr, LastSeen: time.Now()}

	// Tell the joiner about everyone already in the session (their address,
	// so the joiner's client can start UDP hole-punching towards them)...
	for _, existing := range session.Members {
		s.sender.SendTo(addr, ServerMessage{Type: MsgPeerJoined, PeerID: existing.MemberID, PeerAddr: existing.Addr.String()})
	}
	// ...and tell everyone already there about the joiner, same reason.
	for _, existing := range session.Members {
		s.sender.SendTo(existing.Addr, ServerMessage{Type: MsgPeerJoined, PeerID: newID, PeerAddr: addr.String()})
	}

	session.Members[newID] = member
	s.clientsByAddr[addr.String()] = member

	s.sender.SendTo(addr, ServerMessage{
		Type: MsgSessionCreated, Code: code, YourID: newID,
		Salt: base64.StdEncoding.EncodeToString(session.Salt),
	})
}

func (s *Server) handleRelay(addr *net.UDPAddr, payload string) {
	s.mu.Lock()
	defer s.mu.Unlock()

	sender, ok := s.clientsByAddr[addr.String()]
	if !ok {
		s.sender.SendTo(addr, ServerMessage{Type: MsgError, Message: "not in a session"})
		return
	}
	sender.LastSeen = time.Now()

	session, ok := s.sessions[sender.SessionCode]
	if !ok {
		return
	}
	for id, member := range session.Members {
		if id == sender.MemberID {
			continue
		}
		s.sender.SendTo(member.Addr, ServerMessage{Type: MsgRelay, FromPeerID: sender.MemberID, Payload: payload})
	}
}

func (s *Server) handleKeepalive(addr *net.UDPAddr) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if member, ok := s.clientsByAddr[addr.String()]; ok {
		member.LastSeen = time.Now()
		// Acked even with no session activity to reply to (no peer yet, no
		// relay traffic) - the client's own disconnect-detection needs
		// this positive signal to tell "quiet because alone" apart from
		// "quiet because unreachable". See protocol.go's MsgKeepaliveAck.
		s.sender.SendTo(addr, ServerMessage{Type: MsgKeepaliveAck})
	}
}

// handleLeaveSession removes a client immediately, instead of leaving
// remaining members to find out up to clientTimeout (120s) later via
// SweepStaleClients - a plugin quitting/disabling, or switching to a
// different session, sends this so peers see "X left" right away rather
// than X's aircraft just freezing in place for two minutes. Best-effort:
// if this datagram never arrives (e.g. the process was killed, not
// quit cleanly), SweepStaleClients is still the fallback.
func (s *Server) handleLeaveSession(addr *net.UDPAddr) {
	s.mu.Lock()
	defer s.mu.Unlock()

	member, ok := s.clientsByAddr[addr.String()]
	if !ok {
		return
	}
	delete(s.clientsByAddr, addr.String())
	s.removeMemberFromSession(member)
}

// removeMemberFromSession notifies a member's remaining session peers that
// it's gone and cleans up an empty session. Caller must hold s.mu.
func (s *Server) removeMemberFromSession(member *ClientState) {
	session, ok := s.sessions[member.SessionCode]
	if !ok {
		return
	}
	delete(session.Members, member.MemberID)
	for _, remaining := range session.Members {
		s.sender.SendTo(remaining.Addr, ServerMessage{Type: MsgPeerLeft, PeerID: member.MemberID})
	}
	if len(session.Members) == 0 {
		delete(s.sessions, member.SessionCode)
	}
}

// SweepStaleClients removes clients not heard from within clientTimeout,
// notifies the remaining members of their session, and deletes sessions
// that become empty. Call this periodically (see main.go). This is the
// fallback for clients that go away without sending leave_session first
// (crash, killed process, lost connectivity).
//
// Also prunes expired join_session rate-limit counters (see
// allowJoinAttempt) - without this, a sustained scan from many distinct
// source IPs would otherwise grow joinAttemptsByIP without bound for as
// long as the server runs.
func (s *Server) SweepStaleClients(now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()

	for addrKey, member := range s.clientsByAddr {
		if now.Sub(member.LastSeen) <= clientTimeout {
			continue
		}
		delete(s.clientsByAddr, addrKey)
		s.removeMemberFromSession(member)
	}

	for ip, c := range s.joinAttemptsByIP {
		if now.Sub(c.windowStart) > joinAttemptWindow {
			delete(s.joinAttemptsByIP, ip)
		}
	}
}
