#include "formation/rendezvous_config.h"

#include "formation/rendezvous_protocol.h"

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

RendezvousConfig LoadRendezvousConfig(const std::string& path) {
    RendezvousConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }

    bool have_server = false;
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

        if (keyword == "SERVER") {
            std::string host;
            uint16_t port = 0;
            if (SplitHostPort(rest, host, port)) {
                config.server_host = host;
                config.server_port = port;
                have_server = true;
            }
        } else if (keyword == "CREATE") {
            config.create_session = true;
        } else if (keyword == "JOIN") {
            config.join_code = rest;
        }
    }

    config.enabled = have_server && (config.create_session || !config.join_code.empty());
    return config;
}

std::string ResolveRendezvousConfigPath(const std::string& default_path) {
    if (const char* env = std::getenv("XPMULTICREW_RENDEZVOUS_FILE")) {
        return env;
    }
    return default_path;
}

} // namespace flytogether
