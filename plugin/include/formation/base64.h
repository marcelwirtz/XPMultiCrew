#pragma once

// Minimal base64 codec (standard alphabet, '=' padding) - only needed to
// wrap/unwrap the plugin's binary AircraftStatePacket inside a JSON
// "payload" string field for the rendezvous server's relay path
// (server/protocol.go), since the server itself is payload-agnostic and
// only ever sees base64 text, never our binary struct.

#include <cstdint>
#include <string>
#include <vector>

namespace flytogether {

inline std::string Base64Encode(const void* data, size_t len) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t n = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
    }

    const size_t remaining = len - i;
    if (remaining == 1) {
        const uint32_t n = uint32_t(bytes[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const uint32_t n = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

inline int Base64DecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::vector<uint8_t> Base64Decode(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve((text.size() / 4) * 3);

    int vals[4];
    size_t count = 0;
    for (char c : text) {
        if (c == '=' || c == '\0') break;
        const int v = Base64DecodeChar(c);
        if (v < 0) continue; // skip whitespace/unexpected chars
        vals[count++] = v;
        if (count == 4) {
            out.push_back(static_cast<uint8_t>((vals[0] << 2) | (vals[1] >> 4)));
            out.push_back(static_cast<uint8_t>((vals[1] << 4) | (vals[2] >> 2)));
            out.push_back(static_cast<uint8_t>((vals[2] << 6) | vals[3]));
            count = 0;
        }
    }
    if (count == 2) {
        out.push_back(static_cast<uint8_t>((vals[0] << 2) | (vals[1] >> 4)));
    } else if (count == 3) {
        out.push_back(static_cast<uint8_t>((vals[0] << 2) | (vals[1] >> 4)));
        out.push_back(static_cast<uint8_t>((vals[1] << 4) | (vals[2] >> 2)));
    }
    return out;
}

} // namespace flytogether
