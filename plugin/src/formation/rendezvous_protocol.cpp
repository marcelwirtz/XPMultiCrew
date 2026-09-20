#include "formation/rendezvous_protocol.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace flytogether {

namespace {

// Escapes '"', '\' and control characters for embedding `s` inside a JSON
// string literal. Needed for `code`, which - unlike `type`/`payload` - can
// contain arbitrary user-typed text (the companion app doesn't itself
// restrict what a user pastes into the "join session" field before it
// reaches here): without this, a stray '"' would silently truncate the
// message this client sends, or worse, inject extra JSON fields.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extracts the string value of "key":"..." out of `json`, honoring '\"'
// and '\\' escapes while scanning for the closing quote (unlike a plain
// json.find('"', start), which would stop at the first escaped quote and
// silently truncate the value). This is the server-message counterpart to
// JsonEscape() above: server/protocol.go's encoding/json escapes the same
// way, e.g. a "message" error string that happens to contain a '"'.
std::optional<std::string> ExtractString(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\":\"";
    const auto pos = json.find(pattern);
    if (pos == std::string::npos) return std::nullopt;

    std::string out;
    size_t i = pos + pattern.size();
    for (; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            ++i;
            switch (json[i]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                // \uXXXX and anything else: our own vocabulary (error
                // messages, addresses, session codes) never needs full
                // Unicode escape decoding, so just drop the backslash and
                // keep scanning from the following character.
                default: out += json[i]; break;
            }
        } else {
            out += json[i];
        }
    }
    if (i >= json.size()) return std::nullopt; // unterminated string
    return out;
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
    // `type` is always one of our own literal strings and `payload` is
    // base64 - neither ever contains a '"' or '\', so escaping them is a
    // no-op. `code` is different: it reaches here straight from whatever
    // the companion app's "join session" field sent over the control
    // listener's plain-text protocol (control/control_listener.cpp just
    // reads a whitespace-delimited token, no character-set validation) -
    // JsonEscape() keeps a stray '"' in there from truncating/corrupting
    // this message instead of just trusting the input never contains one.
    std::string out = "{\"type\":\"" + JsonEscape(msg.type) + "\"";
    if (!msg.code.empty()) {
        out += ",\"code\":\"" + JsonEscape(msg.code) + "\"";
    }
    // Always included (not just on create/join_session) - see
    // RendezvousClientMessage::client_version's comment.
    out += ",\"client_version\":" + std::to_string(msg.client_version);
    if (msg.is_spectator) {
        out += ",\"role\":\"spectator\"";
    }
    if (!msg.payload.empty()) {
        out += ",\"payload\":\"" + JsonEscape(msg.payload) + "\"";
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
    if (const auto v = ExtractString(json, "salt")) out.salt = *v;
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
