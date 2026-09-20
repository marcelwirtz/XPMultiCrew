#pragma once

#include <cstddef>
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

// The version THIS build writes into every outgoing packet. Bump it
// whenever a future change adds a new optional trailing field, and gate
// that field's *read* on `packet.protocol_version >= <the version it was
// introduced in>` - never on exact equality, so a receiver correctly
// treats "newer than me" the same as "current" (it just won't read fields
// it doesn't know about yet) instead of rejecting the packet outright.
constexpr uint32_t kAircraftStateProtocolVersion = 1;

// The oldest protocol_version this build still understands enough of to
// accept at all - deliberately a *separate*, frozen constant from
// kAircraftStateProtocolVersion above (mirrors JoinFS's own
// `Sim.VERSION` - what it writes, bumped often - vs. its separate,
// rarely-touched minimum-accepted-dataVersion floor). A receiver rejects
// only `protocol_version < kAircraftStateMinProtocolVersion`; anything at
// or above this floor is accepted, regardless of whether it's older, the
// same, or newer than this build's own kAircraftStateProtocolVersion. If
// `kAircraftStateProtocolVersion` were (mis)used for this check instead,
// an updated receiver would reject a not-yet-updated peer's older-but-
// still-valid packets - exactly backwards from the compatibility this is
// meant to provide. Only raise this floor for a genuinely breaking change
// (never for a purely-additive tail field).
constexpr uint32_t kAircraftStateMinProtocolVersion = 1;

constexpr uint16_t kFormationUdpPort = 49002;

// The smallest AircraftStatePacket that has ever been valid on the wire -
// today, that's simply every field below, since there's no earlier/smaller
// version to be compatible with yet. A receiver checks an incoming
// packet's byte count against THIS, not `sizeof(AircraftStatePacket)`
// directly, so that once a future version appends a field (growing
// `sizeof(AircraftStatePacket)`), an older peer's smaller packet is still
// accepted instead of being silently rejected outright. When that day
// comes: leave this constant's value exactly as it is now (don't
// redefine it as `sizeof(AircraftStatePacket)`, which would just track
// the growing struct and defeat the whole point) - only the *reads* of
// any newer trailing field need to additionally check that the received
// byte count actually covers that field's offset (and, ideally, that the
// sender's `protocol_version` says it's present), the same "tail-append,
// version-gated reads" discipline JoinFS's own wire protocol documents.
constexpr size_t kAircraftStateMinSize = 77; // = sizeof(AircraftStatePacket) as of protocol_version 1 -
                                              // a literal, not `sizeof(AircraftStatePacket)`, precisely so
                                              // it can't silently track the struct's size once it grows;
                                              // verified against the struct below by a static_assert.

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

// If this ever fires, `kAircraftStateMinSize` above needs a fresh,
// separately-tracked value for the new baseline the next time a field is
// appended - see its comment. It must never simply become
// `sizeof(AircraftStatePacket)`.
static_assert(sizeof(AircraftStatePacket) == kAircraftStateMinSize,
              "AircraftStatePacket's size changed - see kAircraftStateMinSize's comment");

} // namespace flytogether
