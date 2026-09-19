#include "formation/rendezvous_protocol.h"

#include <cctype>
#include <cstdlib>
#include <optional>

namespace flytogether {

namespace {

std::optional<std::string> ExtractString(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\":\"";
    const auto pos = json.find(pattern);
    if (pos == std::string::npos) return std::nullopt;
    const auto start = pos + pattern.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos) return std::nullopt;
    return json.substr(start, end - start);
}

std::optional<int> ExtractInt(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\":";
    const auto pos = json.find(pattern);
    if (pos == std::string::npos) return std::nullopt;
    auto start = pos + pattern.size();
    auto end = start;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) {
        ++end;
    }
    if (end == start) return std::nullopt;
    return std::atoi(json.substr(start, end - start).c_str());
}

} // namespace

std::string EncodeClientMessage(const RendezvousClientMessage& msg) {
    // Fixed-vocabulary protocol: `type` is always one of our literal
    // strings, `code` only ever contains our alnum session-code alphabet
    // (or a user-typed code, uppercased/trimmed by the config loader), and
    // `payload` is base64 - none of these ever need JSON string escaping.
    std::string out = "{\"type\":\"" + msg.type + "\"";
    if (!msg.code.empty()) {
        out += ",\"code\":\"" + msg.code + "\"";
    }
    if (!msg.payload.empty()) {
        out += ",\"payload\":\"" + msg.payload + "\"";
    }
    out += "}";
    return out;
}

bool DecodeServerMessage(const std::string& json, RendezvousServerMessage& out) {
    const auto type = ExtractString(json, "type");
    if (!type) {
        return false;
    }
    out.type = *type;
    if (const auto v = ExtractString(json, "code")) out.code = *v;
    if (const auto v = ExtractInt(json, "your_id")) out.your_id = *v;
    if (const auto v = ExtractInt(json, "peer_id")) out.peer_id = *v;
    if (const auto v = ExtractString(json, "peer_addr")) out.peer_addr = *v;
    if (const auto v = ExtractInt(json, "from_peer_id")) out.from_peer_id = *v;
    if (const auto v = ExtractString(json, "payload")) out.payload = *v;
    if (const auto v = ExtractString(json, "message")) out.message = *v;
    return true;
}

bool SplitHostPort(const std::string& hostPort, std::string& outHost, uint16_t& outPort) {
    if (hostPort.empty()) return false;

    std::string hostPart;
    std::string portPart;
    if (hostPort.front() == '[') {
        // Bracketed IPv6 form: "[::1]:1234"
        const auto close = hostPort.find(']');
        if (close == std::string::npos || hostPort.size() <= close + 2 || hostPort[close + 1] != ':') {
            return false;
        }
        hostPart = hostPort.substr(1, close - 1);
        portPart = hostPort.substr(close + 2);
    } else {
        const auto colon = hostPort.rfind(':');
        if (colon == std::string::npos) return false;
        hostPart = hostPort.substr(0, colon);
        portPart = hostPort.substr(colon + 1);
    }

    if (hostPart.empty() || portPart.empty()) return false;
    const int port = std::atoi(portPart.c_str());
    if (port <= 0 || port > 0xFFFF) return false;

    outHost = hostPart;
    outPort = static_cast<uint16_t>(port);
    return true;
}

} // namespace flytogether
