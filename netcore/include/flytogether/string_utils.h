#pragma once

#include <cctype>
#include <string>

namespace flytogether {

// Strips a trailing '#'-comment (if any), then leading/trailing whitespace.
// Shared by every line-oriented config file format in this project
// (formation/peer_list.h, formation/rendezvous_config.h,
// shared_cockpit/shared_cockpit_config.h) so the trimming rules can't
// silently drift between them.
inline std::string TrimConfigLine(std::string s) {
    const auto hash_pos = s.find('#');
    if (hash_pos != std::string::npos) {
        s = s.substr(0, hash_pos);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    return s.substr(start);
}

} // namespace flytogether
