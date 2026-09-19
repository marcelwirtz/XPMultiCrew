#include "formation/peer_list.h"

#include "flytogether/string_utils.h"

#include <cstdlib>
#include <fstream>

namespace flytogether {

std::vector<Peer> LoadPeerList(const std::string& path) {
    std::vector<Peer> peers;
    std::ifstream file(path);
    if (!file.is_open()) {
        return peers;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = TrimConfigLine(line);
        if (line.empty()) {
            continue;
        }

        const auto colon_pos = line.rfind(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        Peer peer;
        peer.host = line.substr(0, colon_pos);
        try {
            const int port = std::stoi(line.substr(colon_pos + 1));
            if (port <= 0 || port > 0xFFFF) {
                continue;
            }
            peer.port = static_cast<uint16_t>(port);
        } catch (...) {
            continue;
        }
        if (!peer.host.empty() && peer.port != 0) {
            peers.push_back(std::move(peer));
        }
    }

    return peers;
}

std::string ResolvePeerListPath(const std::string& default_path) {
    if (const char* env = std::getenv("XPMULTICREW_PEERS_FILE")) {
        return env;
    }
    return default_path;
}

} // namespace flytogether
