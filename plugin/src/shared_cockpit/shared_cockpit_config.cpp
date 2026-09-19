#include "shared_cockpit/shared_cockpit_config.h"

#include "formation/rendezvous_protocol.h" // SplitHostPort

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace flytogether {

namespace {

std::string Trim(std::string s) {
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

} // namespace

SharedCockpitConfig LoadSharedCockpitConfig(const std::string& path) {
    SharedCockpitConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        const auto space_pos = line.find(' ');
        const std::string keyword = space_pos == std::string::npos ? line : line.substr(0, space_pos);
        const std::string rest =
            space_pos == std::string::npos ? std::string() : Trim(line.substr(space_pos + 1));

        if (keyword == "ROLE") {
            if (rest == "MASTER") {
                config.role = SharedCockpitRole::kMaster;
            } else if (rest == "CLIENT") {
                config.role = SharedCockpitRole::kClient;
            }
        } else if (keyword == "PEER") {
            std::string host;
            uint16_t port = 0;
            if (SplitHostPort(rest, host, port)) {
                config.peers.push_back(Peer{host, port});
            }
        } else if (keyword == "DATAREF") {
            if (!rest.empty()) {
                config.datarefs.push_back(rest);
            }
        }
    }

    config.enabled = config.role != SharedCockpitRole::kNone;
    return config;
}

std::string ResolveSharedCockpitConfigPath(const std::string& default_path, const std::string& icao_type,
                                            const std::string& plugin_resources_path) {
    if (const char* env = std::getenv("XPMULTICREW_SHARED_COCKPIT_FILE")) {
        return env;
    }
    if (!icao_type.empty()) {
        // A user's own override/customization, next to X-Plane, always
        // wins over the plugin's bundled default if present.
        const std::string user_profile_path =
            std::string(kSharedCockpitProfilesDir) + "/" + icao_type + ".txt";
        if (std::ifstream probe(user_profile_path); probe.is_open()) {
            return user_profile_path;
        }
        // The profile bundled with (and auto-installed alongside) the
        // plugin itself - see kSharedCockpitProfilesSubdir's comment.
        if (!plugin_resources_path.empty()) {
            const std::string bundled_path = plugin_resources_path + "/" +
                                              std::string(kSharedCockpitProfilesSubdir) + "/" + icao_type +
                                              ".txt";
            if (std::ifstream probe(bundled_path); probe.is_open()) {
                return bundled_path;
            }
        }
    }
    return default_path;
}

} // namespace flytogether
