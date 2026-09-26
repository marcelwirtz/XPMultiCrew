#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flytogether {

struct Peer {
    std::string host;
    uint16_t port = 0;
};

// Loads a simple "host:port" per line peer list (blank lines and #comments
// ignored) for LAN testing. This is a deliberately manual stand-in for
// session discovery - the rendezvous/relay server (docs/plan.md section 7)
// replaces it in Phase 2. Returns an empty list if the file cannot be
// opened.
std::vector<Peer> LoadPeerList(const std::string& path);

// Optional "PORT <n>" line in the same file: the local UDP port Formation
// listens on for LAN peers (default kFormationUdpPort, 49002 - which
// ForeFlight and some EFB tools also use). Returns `fallback` if the file
// has no valid PORT line or can't be opened.
uint16_t LoadPeerListListenPort(const std::string& path, uint16_t fallback);

// Resolves the peer list file path: $XPMULTICREW_PEERS_FILE if set,
// otherwise `default_path`.
std::string ResolvePeerListPath(const std::string& default_path);

} // namespace flytogether
