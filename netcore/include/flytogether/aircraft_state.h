#pragma once

#include <cstdint>

// Phase 1 wire format for Formation mode (docs/plan.md section 5): one
// aircraft's position, attitude and a few animation-relevant surfaces/
// lights, broadcast to every configured peer. Deliberately not tied to a
// fixed number of participants - a `sender_id` identifies the aircraft, so
// any number of peers can be tracked (see plugin/include/formation).
//
// This supersedes the Phase 0 spike protocol (position_packet.h), which is
// left untouched as a standalone, already-verified artifact.

namespace flytogether {

constexpr uint32_t kAircraftStateMagic = 0x46545331; // "FTS1"
constexpr uint32_t kAircraftStateProtocolVersion = 1;
constexpr uint16_t kFormationUdpPort = 49002;

namespace LightBits {
constexpr uint8_t kBeacon = 1 << 0;
constexpr uint8_t kStrobe = 1 << 1;
constexpr uint8_t kNav = 1 << 2;
constexpr uint8_t kLanding = 1 << 3;
} // namespace LightBits

#pragma pack(push, 1)
struct AircraftStatePacket {
    uint32_t magic = kAircraftStateMagic;
    uint32_t protocol_version = kAircraftStateProtocolVersion;
    uint32_t sender_id = 0; // random, chosen once per plugin session
    uint32_t sequence = 0;  // monotonically increasing per sender, wraps

    double latitude = 0.0;
    double longitude = 0.0;
    double elevation_m = 0.0; // MSL meters

    float heading_deg = 0.0f; // true heading
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;

    float gear_ratio = 0.0f;       // 0 = up, 1 = down
    float flap_ratio = 0.0f;       // 0..1
    float speedbrake_ratio = 0.0f; // 0..1
    float engine_ratio = 0.0f;     // throttle/N1 proxy, 0..1, for animation

    uint8_t light_bits = 0; // bitmask of LightBits::k*

    char icao_type[8] = {}; // e.g. "C172", null-padded, not null-terminated
                             // if it fills all 8 bytes
};
#pragma pack(pop)

} // namespace flytogether
