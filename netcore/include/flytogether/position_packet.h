#pragma once

#include <cstdint>

// Shared wire format for the Phase 0 spike: prove that the plugin can read a
// dataref and get it to a second, separate process over a real UDP socket
// (loopback only, no NAT traversal / encryption / compression yet - that's
// Phase 1+, see docs/plan.md sections 5 and 7).

namespace flytogether {

constexpr uint32_t kPositionPacketMagic = 0x46545030; // "FTP0"
constexpr uint16_t kSpikeUdpPort = 49001;

#pragma pack(push, 1)
struct PositionPacket {
    uint32_t magic = kPositionPacketMagic;
    uint32_t sequence = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double elevation = 0.0;
};
#pragma pack(pop)

} // namespace flytogether
