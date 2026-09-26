#include "formation/csl_aircraft.h"

#include <cmath>

namespace flytogether {

namespace {
// Used when the sender didn't report its reference-point height
// (AircraftStatePacket::ref_height_agl_m unknown: a protocol_version 1
// peer, or one that hasn't been on the ground yet this flight). Roughly
// what the light twins in this project's bundled Shared Cockpit profiles
// measure (BE58/BE9L ~2.4-2.5m) - the same figure the generic model's
// VERT_OFFSET used to hard-code before the height was sent explicitly.
constexpr double kDefaultRefHeightAglM = 2.5;
constexpr double kMaxPlausibleRefHeightAglM = 15.0;

double RefHeightAglM(float reported) {
    if (!std::isfinite(reported) || reported < 0.0f || reported > kMaxPlausibleRefHeightAglM) {
        return kDefaultRefHeightAglM;
    }
    return reported;
}

// XPMP2 mode S ids must be in [0x01, 0xFFFFFF]; our sender_id is a full
// uint32 chosen at random, so fold it into that range.
XPMPPlaneID ToModeS(uint32_t senderId) {
    return static_cast<XPMPPlaneID>((senderId % 0xFFFFFEu) + 1);
}
} // namespace

RemoteAircraftXPMP::RemoteAircraftXPMP(const std::string& icaoType, uint32_t senderId)
    : XPMP2::Aircraft(icaoType.empty() ? "GENR" : icaoType, "", "", ToModeS(senderId))
{
    // Without this, a peer's aircraft has zero protection against
    // appearing to sink into or float above terrain/scenery when the two
    // sides' local elevation data disagrees (different scenery packs,
    // network-position jitter, etc.) - XPMP2's own ClampToGround() probes
    // X-Plane's actual rendered scenery collision mesh via
    // XPLMProbeTerrainXYZ, so unlike a flat terrain-heightmap read it
    // already accounts for objects like helipads/ship decks. It's just
    // off by default (XPMP2::Aircraft::bClampToGround).
    bClampToGround = true;
}

void RemoteAircraftXPMP::SetPose(const AircraftPose& pose) {
    pose_ = pose;
}

void RemoteAircraftXPMP::UpdatePosition(float /*elapsedSinceLastCall*/, int /*flCounter*/) {
    // XPMP2 wants the altitude of the bottom of the gear (it then lifts the
    // model by the CSL's own VERT_OFFSET), but elevation_m is X-Plane's
    // aircraft reference point, which sits metres above that - so subtract
    // the sender's measured gap. Makes every CSL package sit on the ground
    // correctly instead of only a model with a hand-tuned VERT_OFFSET.
    const double gear_altitude_m = pose_.elevation_m - RefHeightAglM(pose_.ref_height_agl_m);
    SetLocation(pose_.latitude, pose_.longitude, gear_altitude_m / XPMP2::M_per_FT);
    drawInfo.heading = pose_.heading_deg;
    drawInfo.pitch = pose_.pitch_deg;
    drawInfo.roll = pose_.roll_deg;

    SetGearRatio(pose_.gear_ratio);
    SetFlapRatio(pose_.flap_ratio);
    SetSpeedbrakeRatio(pose_.speedbrake_ratio);
    SetThrustRatio(pose_.engine_ratio);

    SetLightsBeacon((pose_.light_bits & LightBits::kBeacon) != 0);
    SetLightsStrobe((pose_.light_bits & LightBits::kStrobe) != 0);
    SetLightsNav((pose_.light_bits & LightBits::kNav) != 0);
    SetLightsLanding((pose_.light_bits & LightBits::kLanding) != 0);
}

} // namespace flytogether
