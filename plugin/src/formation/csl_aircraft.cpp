#include "formation/csl_aircraft.h"

namespace flytogether {

namespace {
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
    SetLocation(pose_.latitude, pose_.longitude, pose_.elevation_m / XPMP2::M_per_FT);
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
