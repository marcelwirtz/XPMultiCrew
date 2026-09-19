#pragma once

#include <cstdint>
#include <string>

namespace flytogether {

// File-based rendezvous setup, the same deliberately-manual stand-in for a
// real UI that peer_list.h is for LAN peers (a real "enter session code"
// dialog is Phase 4 work). See docs/plan.md section 7.
//
// Expected file contents, one directive per line (# comments, blank lines
// ignored):
//   SERVER <host>:<port>
//   CREATE                 # start a new session, log the code to share
// or
//   SERVER <host>:<port>
//   JOIN <code>             # join a session someone else created
struct RendezvousConfig {
    bool enabled = false; // true only if SERVER + (CREATE or JOIN) were both given
    std::string server_host;
    uint16_t server_port = 0;
    bool create_session = false;
    std::string join_code;
};

// Returns a disabled config (enabled=false) if the file doesn't exist or
// doesn't specify enough to act on.
RendezvousConfig LoadRendezvousConfig(const std::string& path);

// Resolves the config file path: $XPMULTICREW_RENDEZVOUS_FILE if set,
// otherwise `default_path`.
std::string ResolveRendezvousConfigPath(const std::string& default_path);

} // namespace flytogether
