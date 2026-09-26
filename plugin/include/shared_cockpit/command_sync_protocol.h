#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flytogether {

// Wire format for Shared Cockpit command mirroring (see command_sync.h):
// one message per button press/release. Travels sealed over the Shared
// Cockpit session like the dataref sync.
constexpr uint32_t kCommandSyncMagic = 0x46544331; // "FTC1"

enum class CommandPhase : uint8_t { kBegin = 1, kEnd = 2 };

struct CommandMessage {
    uint32_t sender_id = 0;
    uint32_t sequence = 0; // per sender - relay and direct deliver every message twice, see CommandDedup
    CommandPhase phase = CommandPhase::kBegin;
    std::string name;
};

constexpr size_t kMaxCommandNameLen = 200;

std::vector<uint8_t> EncodeCommandMessage(const CommandMessage& msg);
std::optional<CommandMessage> DecodeCommandMessage(const uint8_t* data, size_t len);

// Remembers the last kWindow sequence numbers per sender so a message that
// arrives both directly and via the relay is executed once. A dataref value
// arriving twice is harmless; a button pressed twice isn't.
class CommandDedup {
public:
    static constexpr size_t kWindow = 256;
    // True the first time (sender, sequence) is seen.
    bool Accept(uint32_t sender_id, uint32_t sequence);

private:
    struct Seen {
        std::array<uint32_t, kWindow> ring{};
        std::array<bool, kWindow> used{};
        size_t next = 0;
    };
    std::unordered_map<uint32_t, Seen> seen_;
};

} // namespace flytogether
