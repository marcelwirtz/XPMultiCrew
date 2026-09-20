#include "shared_cockpit/shared_cockpit_config.h"

#include "flytogether/string_utils.h"

#include <cstdlib>
#include <fstream>

namespace flytogether {

SharedCockpitConfig LoadSharedCockpitConfig(const std::string& path) {
    SharedCockpitConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = TrimConfigLine(line);
        if (line.empty()) {
            continue;
        }

        const auto space_pos = line.find(' ');
        const std::string keyword = space_pos == std::string::npos ? line : line.substr(0, space_pos);
        const std::string rest =
            space_pos == std::string::npos ? std::string() : TrimConfigLine(line.substr(space_pos + 1));

        if (keyword == "DATAREF") {
            if (!rest.empty()) {
                config.datarefs.push_back(rest);
            }
        }
        // ROLE/PEER are no longer recognized here - Shared Cockpit only
        // ever starts through the companion app (rendezvous-based peer
        // discovery), never from a manually-typed peer in this file - see
        // this header's comment.
    }

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
