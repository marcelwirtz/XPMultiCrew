#pragma once

// C++ side of the rendezvous protocol implemented by server/protocol.go.
// Deliberately not a general JSON library: the message set is small and
// fixed-shape, and we only ever decode output produced by our own Go
// server (compact `encoding/json` output, one flat object per line), so a
// simple key-lookup decoder is enough and easy to keep in sync by hand
// with server/protocol.go.

#include <cstdint>
#include <string>

namespace flytogether {

// This build's own wire-protocol version, sent as client_version on every
// message - see server/protocol.go's ProtocolVersion comment for the hard-
// cutover reasoning (session-auth/encryption, docs/plan.md Phase 4). Must
// match server/protocol.go's ProtocolVersion exactly; the two are kept in
// sync by hand like every other part of this hand-rolled protocol.
// 3 since v0.3.0 - see server/protocol.go's ProtocolVersion comment.
constexpr int kRendezvousProtocolVersion = 3;

// Client -> server
struct RendezvousClientMessage {
    std::string type;             // "create_session" | "join_session" | "relay" | "keepalive" | "leave_session"
    std::string code;             // join_session
    int client_version = kRendezvousProtocolVersion;
    // create_session/join_session only - encoded as "role":"spectator"
    // when true, omitted (defaulting to an ordinary participant) when
    // false - see server/protocol.go's ClientMessage.Role comment.
    bool is_spectator = false;
    std::string payload; // relay, base64
};

// Server -> client
struct RendezvousServerMessage {
    std::string type; // "session_created" | "peer_joined" | "peer_left" | "relay" | "error" | "keepalive_ack"
    std::string code;
    int your_id = 0;
    std::string salt; // session_created, base64 - see net/session_crypto.h
    int peer_id = 0;
    std::string peer_addr; // "ip:port"
    int from_peer_id = 0;
    std::string payload; // relay, base64
    std::string message; // error
};

std::string EncodeClientMessage(const RendezvousClientMessage& msg);

// Returns false if `json` doesn't look like one of our messages at all
// (e.g. empty/garbage). Missing fields are left at their zero value.
bool DecodeServerMessage(const std::string& json, RendezvousServerMessage& out);

// Splits "ip:port" (as sent in peer_addr) into host and port. Returns
// false if the format is unexpected.
bool SplitHostPort(const std::string& hostPort, std::string& outHost, uint16_t& outPort);

} // namespace flytogether
