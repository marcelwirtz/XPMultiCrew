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

// Client -> server
struct RendezvousClientMessage {
    std::string type;    // "create_session" | "join_session" | "relay" | "keepalive" | "leave_session"
    std::string code;    // join_session
    std::string payload; // relay, base64
};

// Server -> client
struct RendezvousServerMessage {
    std::string type; // "session_created" | "peer_joined" | "peer_left" | "relay" | "error" | "keepalive_ack"
    std::string code;
    int your_id = 0;
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
