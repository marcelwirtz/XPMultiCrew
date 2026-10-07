#include "shared_cockpit/shared_cockpit_config.h"

#include "flytogether/string_utils.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace flytogether {

bool ParseDatarefCategoryName(const std::string& name, DatarefCategory& out) {
    if (name == "engine") {
        out = DatarefCategory::kEngine;
    } else if (name == "avionics") {
        out = DatarefCategory::kAvionics;
    } else if (name == "systems") {
        out = DatarefCategory::kSystems;
    } else if (name == "flight") {
        out = DatarefCategory::kFlight;
    } else {
        return false; // unrecognized - leave `out` untouched, caller keeps the default
    }
    return true;
}

std::string DatarefCategoryName(DatarefCategory category) {
    switch (category) {
        case DatarefCategory::kEngine:
            return "engine";
        case DatarefCategory::kAvionics:
            return "avionics";
        case DatarefCategory::kSystems:
            return "systems";
        case DatarefCategory::kFlight:
            return "flight";
    }
    return "systems";
}

bool IsFlightControlDataref(const std::string& name) {
    const std::string base = name.substr(0, name.find('['));
    static const char* const kControls[] = {
        "sim/joystick/yoke_pitch_ratio",
        "sim/joystick/yoke_roll_ratio",
        "sim/joystick/yoke_heading_ratio",
        "sim/cockpit2/controls/yoke_pitch_ratio",
        "sim/cockpit2/controls/yoke_roll_ratio",
        "sim/cockpit2/controls/yoke_heading_ratio",
        "sim/cockpit2/controls/left_brake_ratio",
        "sim/cockpit2/controls/right_brake_ratio",
        "sim/cockpit2/engine/actuators/throttle_ratio",
        "sim/cockpit2/engine/actuators/throttle_ratio_all",
        "sim/cockpit2/engine/actuators/throttle_beta_rev_ratio",
        "sim/cockpit2/engine/actuators/throttle_beta_rev_ratio_all",
        "sim/cockpit2/engine/actuators/throttle_jet_rev_ratio",
        "sim/cockpit2/engine/actuators/throttle_jet_rev_ratio_all",
        "sim/cockpit2/engine/actuators/mixture_ratio",
        "sim/cockpit2/engine/actuators/mixture_ratio_all",
        "sim/cockpit2/engine/actuators/prop_ratio",
        "sim/cockpit2/engine/actuators/prop_ratio_all",
        "sim/flightmodel/engine/ENGN_thro",
        "sim/flightmodel/engine/ENGN_thro_use",
        "sim/flightmodel/engine/ENGN_mixt",
        "sim/flightmodel/engine/ENGN_prop",
    };
    for (const char* control : kControls) {
        if (base == control) {
            return true;
        }
    }
    return false;
}

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
                // First token is the dataref path; any further
                // whitespace-separated tokens are optional modifiers
                // (STREAM, or CATEGORY followed by its own value), in
                // either order - see this file's DATAREF grammar comment.
                std::istringstream tokens(rest);
                DatarefSyncSpec spec;
                tokens >> spec.name;
                std::string token;
                while (tokens >> token) {
                    if (token == "STREAM") {
                        spec.stream = true;
                    } else if (token == "OUTPUT") {
                        spec.output = true;
                    } else if (token == "CATEGORY") {
                        std::string category_name;
                        tokens >> category_name;
                        DatarefCategory parsed = spec.category;
                        // "flight" is the MASTER role, not a dataref bucket - see kFlight.
                        if (ParseDatarefCategoryName(category_name, parsed) && parsed != DatarefCategory::kFlight) {
                            spec.category = parsed;
                        }
                    }
                    // Unrecognized tokens are ignored, same "forward
                    // compatible, don't hard-fail on the unknown" spirit
                    // as everywhere else this file parses.
                }
                if (!spec.name.empty() && !IsFlightControlDataref(spec.name)) {
                    config.datarefs.push_back(spec);
                }
            }
        }
        if (keyword == "COMMAND" && !rest.empty()) {
            std::istringstream tokens(rest);
            CommandSyncSpec spec;
            tokens >> spec.name;
            std::string token;
            while (tokens >> token) {
                if (token == "CATEGORY") {
                    std::string category_name;
                    tokens >> category_name;
                    DatarefCategory parsed = spec.category;
                    if (ParseDatarefCategoryName(category_name, parsed) && parsed != DatarefCategory::kFlight) {
                        spec.category = parsed;
                    }
                }
            }
            if (!spec.name.empty()) {
                config.commands.push_back(spec);
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
