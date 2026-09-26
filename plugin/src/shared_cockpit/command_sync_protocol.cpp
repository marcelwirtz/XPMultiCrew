#include "shared_cockpit/command_sync_protocol.h"

#include <cstring>

namespace flytogether {

std::vector<uint8_t> EncodeCommandMessage(const CommandMessage& msg) {
    const std::string name = msg.name.substr(0, kMaxCommandNameLen);
    std::vector<uint8_t> out(4 + 4 + 4 + 1 + 1 + name.size());
    size_t o = 0;
    auto put32 = [&](uint32_t v) {
        std::memcpy(out.data() + o, &v, 4);
        o += 4;
    };
    put32(kCommandSyncMagic);
    put32(msg.sender_id);
    put32(msg.sequence);
    out[o++] = static_cast<uint8_t>(msg.phase);
    out[o++] = static_cast<uint8_t>(name.size());
    std::memcpy(out.data() + o, name.data(), name.size());
    return out;
}

std::optional<CommandMessage> DecodeCommandMessage(const uint8_t* data, size_t len) {
    if (len < 14) {
        return std::nullopt;
    }
    uint32_t magic = 0;
    CommandMessage msg;
    std::memcpy(&magic, data, 4);
    std::memcpy(&msg.sender_id, data + 4, 4);
    std::memcpy(&msg.sequence, data + 8, 4);
    const uint8_t phase = data[12];
    const size_t name_len = data[13];
    if (magic != kCommandSyncMagic || (phase != 1 && phase != 2) || name_len == 0 || 14 + name_len > len) {
        return std::nullopt;
    }
    msg.phase = static_cast<CommandPhase>(phase);
    msg.name.assign(reinterpret_cast<const char*>(data + 14), name_len);
    for (char c : msg.name) {
        if (c <= ' ' || c > '~') {
            return std::nullopt; // command paths are plain printable ASCII
        }
    }
    return msg;
}

bool CommandDedup::Accept(uint32_t sender_id, uint32_t sequence) {
    Seen& s = seen_[sender_id];
    for (size_t i = 0; i < kWindow; ++i) {
        if (s.used[i] && s.ring[i] == sequence) {
            return false;
        }
    }
    s.ring[s.next] = sequence;
    s.used[s.next] = true;
    s.next = (s.next + 1) % kWindow;
    return true;
}

} // namespace flytogether
