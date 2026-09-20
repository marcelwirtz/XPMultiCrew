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

// ProtocolVersion is this build's own wire-protocol version, sent as
// ClientVersion on every create_session/join_session. Session-Auth/
// Verschlüsselung (docs/plan.md Phase 4) is a hard, incompatible cutover
// by design (the user's own call, not a gradual migration): a joiner
// whose ClientVersion doesn't match the session creator's is rejected
// with a clear error (see handleJoinSession) instead of the two sides
// silently failing to understand each other's encrypted-vs-plaintext (or
// differently-keyed) payloads. Bump this only for a genuinely incompatible
// wire change - see aircraft_state.h's kAircraftStateMinProtocolVersion
// for the same "only bump for a real break" discipline on a different
// protocol in this project.
const ProtocolVersion = 2

// ClientMessage is anything a client sends to the server.
type ClientMessage struct {
	Type MessageType `json:"type"`
	Code string      `json:"code,omitempty"` // join_session
	// Sent with every message (not just create/join_session) for
	// simplicity - the server only actually inspects it for those two,
	// see handleCreateSession/handleJoinSession.
	ClientVersion int `json:"client_version"`
	// Role is "" (the default - a normal Formation participant) or
	// "spectator" - see docs/plan.md's spectator-mode section. The
	// server's only job for this is remembering it per ClientState
	// (session.go) and passing it along in peer_joined below; "never
	// broadcast your own position" is entirely a client-side (plugin)
	// behavior - this server never inspected AircraftStatePacket payloads
	// to begin with (see this file's own top comment), so there's nothing
	// for it to enforce either way.
	Role    string `json:"role,omitempty"`    // create_session, join_session
	Payload string `json:"payload,omitempty"` // relay, base64
}

// ServerMessage is anything the server sends to a client.
type ServerMessage struct {
	Type MessageType `json:"type"`

	// session_created (sent both on create and on join, so both flows
	// converge on "you are now member YourID of session Code"). Salt is
	// this session's server-minted random value (base64), the same one
	// on both the creator's and every joiner's session_created - both
	// sides derive their shared SessionCrypto key from it plus the
	// session code they already both know (see net/session_crypto.h).
	// The server itself never sees or needs the derived key - it only
	// hands out the salt and otherwise keeps relaying opaque bytes.
	Code   string `json:"code,omitempty"`
	YourID int    `json:"your_id,omitempty"`
	Salt   string `json:"salt,omitempty"`

	// peer_joined / peer_left
	PeerID   int    `json:"peer_id,omitempty"`
	PeerAddr string `json:"peer_addr,omitempty"` // "ip:port" as seen by the server
	// PeerIsSpectator (peer_joined only) - so a receiving client/companion
	// UI can eventually tell "this peer is watching, not flying" apart
	// from an ordinary pilot, without needing a separate lookup. No
	// `omitempty`: `false` (the common case) must still be sent, not
	// silently dropped and mistaken for "not included in this response".
	PeerIsSpectator bool `json:"peer_is_spectator"`

	// relay
	FromPeerID int    `json:"from_peer_id,omitempty"`
	Payload    string `json:"payload,omitempty"`

	// error
	Message string `json:"message,omitempty"`
}
