package main

// Wire protocol for the rendezvous/relay server (docs/plan.md section 7).
// One UDP port, JSON-framed messages. The server is deliberately generic:
// it never looks inside `Payload` (base64 of whatever the plugin's netcore
// sends) - it only manages session membership, peer discovery for
// client-side UDP hole-punching, and an always-available relay fallback.

type MessageType string

const (
	// Client -> server
	MsgCreateSession MessageType = "create_session"
	MsgJoinSession   MessageType = "join_session"
	MsgRelay         MessageType = "relay"
	MsgKeepalive     MessageType = "keepalive"
	MsgLeaveSession  MessageType = "leave_session"

	// Server -> client
	MsgSessionCreated MessageType = "session_created"
	MsgPeerJoined     MessageType = "peer_joined"
	MsgPeerLeft       MessageType = "peer_left"
	MsgError          MessageType = "error"
	// Sent in reply to every keepalive, purely so the client has a
	// positive "the server is still there" signal even while alone in a
	// quiet session (no peer, no relay traffic) - see
	// RendezvousClient::PollIncoming's on_disconnected handling, which
	// would otherwise have no way to tell "quiet because nobody's here
	// yet" apart from "quiet because the server/network is unreachable".
	MsgKeepaliveAck MessageType = "keepalive_ack"
)

// ClientMessage is anything a client sends to the server.
type ClientMessage struct {
	Type    MessageType `json:"type"`
	Code    string      `json:"code,omitempty"`    // join_session
	Payload string      `json:"payload,omitempty"` // relay, base64
}

// ServerMessage is anything the server sends to a client.
type ServerMessage struct {
	Type MessageType `json:"type"`

	// session_created (sent both on create and on join, so both flows
	// converge on "you are now member YourID of session Code")
	Code   string `json:"code,omitempty"`
	YourID int    `json:"your_id,omitempty"`

	// peer_joined / peer_left
	PeerID   int    `json:"peer_id,omitempty"`
	PeerAddr string `json:"peer_addr,omitempty"` // "ip:port" as seen by the server

	// relay
	FromPeerID int    `json:"from_peer_id,omitempty"`
	Payload    string `json:"payload,omitempty"`

	// error
	Message string `json:"message,omitempty"`
}
