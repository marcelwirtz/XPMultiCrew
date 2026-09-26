#pragma once

#include <cmath>
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
constexpr uint32_t kAircraftStateProtocolVersion = 2;

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
constexpr uint8_t kTaxi = 1 << 4; // since protocol_version 2 - older senders just never set it
} // namespace LightBits

namespace StateFlags {
constexpr uint8_t kOnGround = 1 << 0;
} // namespace StateFlags

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

    // --- protocol_version 2 ---

    // How far the reference point `elevation_m` describes sits above the
    // bottom of the landing gear, in meters - measured while on the ground
    // (sim/flightmodel/position/y_agl) and held from then on. Lets the
    // receiver hand XPMP2 the gear-contact altitude it expects, so any CSL
    // model's own VERT_OFFSET places it correctly on the ground. Negative
    // = unknown (not measured yet, or a protocol_version 1 sender whose
    // shorter packet never reached this field - it keeps this default).
    float ref_height_agl_m = -1.0f;

    // Shown as the aircraft's label in the sim and in the companion's peer
    // list. [A-Za-z0-9-] only, null-padded like icao_type; empty = none set.
    char callsign[8] = {};

    // Velocity in X-Plane's local OpenGL frame (m/s) and body angular
    // rates P/Q/R (deg/s). Shared Cockpit uses them so a client taking over
    // control (or losing the master) keeps flying at the master's speed
    // instead of stopping dead, and so its instruments see real motion
    // while following.
    float velocity_x_mps = 0.0f;
    float velocity_y_mps = 0.0f;
    float velocity_z_mps = 0.0f;
    float roll_rate_dps = 0.0f;
    float pitch_rate_dps = 0.0f;
    float yaw_rate_dps = 0.0f;

    // Extra CSL animation inputs (XPMP2 dataref names in brackets).
    float reverser_ratio = 0.0f;      // 0..1 [thrust reverser deploy]
    float prop_rpm = 0.0f;            // [prop + engine rotation]
    float tire_rot_rad_s = 0.0f;      // [tire rotation]
    float nose_wheel_deg = 0.0f;      // [nose wheel steering]
    float yoke_pitch_ratio = 0.0f;    // -1..1
    float yoke_roll_ratio = 0.0f;     // -1..1
    float yoke_heading_ratio = 0.0f;  // -1..1 (rudder)
    float slat_ratio = 0.0f;          // 0..1

    uint8_t flags = 0; // bitmask of StateFlags::k*
};
#pragma pack(pop)

// Size as of protocol_version 2. kAircraftStateMinSize above stays at the
// version 1 baseline on purpose - see its comment.
constexpr size_t kAircraftStateV2Size = 146;
static_assert(sizeof(AircraftStatePacket) == kAircraftStateV2Size,
              "AircraftStatePacket's size changed - add a new kAircraftStateV<N>Size, don't touch "
              "kAircraftStateMinSize (see its comment)");
static_assert(offsetof(AircraftStatePacket, ref_height_agl_m) == kAircraftStateMinSize,
              "protocol_version 2 fields must start right after the version 1 baseline");

// Rejects poses no real aircraft can have (NaN/Inf, out-of-range lat/lon,
// absurd altitude) before they reach XPMP2 or - in Shared Cockpit - the
// client's own physics override. Encryption authenticates who sent a
// packet, not that the sender's sim produced sane numbers.
inline bool IsPlausibleAircraftState(const AircraftStatePacket& p) {
    const bool finite = std::isfinite(p.latitude) && std::isfinite(p.longitude) &&
                        std::isfinite(p.elevation_m) && std::isfinite(p.heading_deg) &&
                        std::isfinite(p.pitch_deg) && std::isfinite(p.roll_deg) &&
                        std::isfinite(p.ref_height_agl_m) && std::isfinite(p.velocity_x_mps) &&
                        std::isfinite(p.velocity_y_mps) && std::isfinite(p.velocity_z_mps) &&
                        std::isfinite(p.roll_rate_dps) && std::isfinite(p.pitch_rate_dps) &&
                        std::isfinite(p.yaw_rate_dps) && std::isfinite(p.reverser_ratio) &&
                        std::isfinite(p.prop_rpm) && std::isfinite(p.tire_rot_rad_s) &&
                        std::isfinite(p.nose_wheel_deg) && std::isfinite(p.yoke_pitch_ratio) &&
                        std::isfinite(p.yoke_roll_ratio) && std::isfinite(p.yoke_heading_ratio) &&
                        std::isfinite(p.slat_ratio);
    return finite && std::fabs(p.latitude) <= 90.0 && std::fabs(p.longitude) <= 180.0 &&
           p.elevation_m > -1000.0 && p.elevation_m < 30000.0;
}

// Cuts icao_type off at the first character that isn't [A-Za-z0-9] - it
// ends up in XPMP2 model matching and in the companion app's line-based
// status text (PEERS id:icao:callsign;...), where a stray newline, ';' or
// ':' from a peer would break or inject lines. Callsigns may also contain
// '-' (registrations like D-EABC).
inline void SanitizeFixedString(char (&text)[8], bool allowDash) {
    bool cut = false;
    for (char& c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        (allowDash && c == '-');
        if (cut || !ok) {
            cut = true;
            c = '\0';
        }
    }
}
inline void SanitizeIcaoType(char (&icao)[8]) { SanitizeFixedString(icao, false); }
inline void SanitizeCallsign(char (&callsign)[8]) { SanitizeFixedString(callsign, true); }

} // namespace flytogether
