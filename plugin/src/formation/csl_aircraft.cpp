#include "formation/csl_aircraft.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
    : XPMP2::Aircraft(icaoType.empty() ? "GENR" : icaoType, "", "", ToModeS(senderId)),
      requested_icao_(icaoType)
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

void RemoteAircraftXPMP::SetPose(const AircraftPose& pose, const AircraftStatePacket& latest) {
    pose_ = pose;
    latest_ = latest;

    const std::string callsign(latest.callsign, strnlen(latest.callsign, sizeof(latest.callsign)));
    if (callsign != applied_callsign_) {
        applied_callsign_ = callsign;
        // Label next to the aircraft (and on X-Plane's map): the callsign
        // if the peer set one, else its type - better than XPMP2's default
        // of nothing useful.
        label = callsign.empty() ? requested_icao_ : callsign;
        std::snprintf(acInfoTexts.tailNum, sizeof(acInfoTexts.tailNum), "%s", callsign.c_str());
        std::snprintf(acInfoTexts.flightNum, sizeof(acInfoTexts.flightNum), "%s", callsign.c_str());
    }
}

void RemoteAircraftXPMP::UpdateIcaoType(const std::string& icaoType) {
    if (icaoType == requested_icao_) {
        return;
    }
    requested_icao_ = icaoType;
    ChangeModel(icaoType.empty() ? "GENR" : icaoType, "", "");
    if (applied_callsign_.empty()) {
        label = requested_icao_;
    }
}

void RemoteAircraftXPMP::UpdatePosition(float elapsedSinceLastCall, int /*flCounter*/) {
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
    SetLightsTaxi((pose_.light_bits & LightBits::kTaxi) != 0);

    SetOnGrnd((latest_.flags & StateFlags::kOnGround) != 0);
    SetReversDeployRatio(latest_.reverser_ratio);
    SetThrustReversRatio(latest_.reverser_ratio);
    SetNoseWheelAngle(latest_.nose_wheel_deg);
    SetYokePitchRatio(latest_.yoke_pitch_ratio);
    SetYokeRollRatio(latest_.yoke_roll_ratio);
    SetYokeHeadingRatio(latest_.yoke_heading_ratio);
    SetSlatRatio(latest_.slat_ratio);

    // Rotation speeds are sent; the angles CSL models actually animate
    // with are integrated here, per drawn frame.
    const float dt = std::clamp(elapsedSinceLastCall, 0.0f, 0.5f);
    SetTireRotRad(latest_.tire_rot_rad_s);
    tire_angle_deg_ = std::fmod(tire_angle_deg_ + latest_.tire_rot_rad_s * 57.29578f * dt, 360.0f);
    SetTireRotAngle(tire_angle_deg_);
    SetPropRotRpm(latest_.prop_rpm);
    SetEngineRotRpm(latest_.prop_rpm);
    prop_angle_deg_ = std::fmod(prop_angle_deg_ + latest_.prop_rpm * 6.0f * dt, 360.0f); // rpm*360/60
    SetPropRotAngle(prop_angle_deg_);
    SetEngineRotAngle(prop_angle_deg_);
}

} // namespace flytogether
