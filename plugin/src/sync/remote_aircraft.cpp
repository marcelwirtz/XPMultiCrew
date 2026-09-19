#include "sync/remote_aircraft.h"

#include <algorithm>
#include <cmath>

namespace flytogether {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6378137.0;

// Never extrapolate further into the future than this past the last packet
// - avoids remote aircraft flying off into space if the network connection
// drops instead of just holding position (see IsStale() for eviction).
constexpr double kMaxExtrapolationS = 1.5;

// Wraparound-safe "is `a` newer than `b`" for uint32 sequence numbers.
bool IsNewer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

double WrapDegrees180(double deg) {
    deg = std::fmod(deg + 180.0, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg - 180.0;
}

double WrapDegrees360(double deg) {
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg;
}

} // namespace

void RemoteAircraft::OnPacketReceived(const AircraftStatePacket& packet, double receive_time_s) {
    if (has_latest_ && !IsNewer(packet.sequence, latest_.sequence)) {
        return; // stale, out-of-order or duplicate relative to what we have
    }

    previous_ = latest_;
    previous_receive_time_s_ = latest_receive_time_s_;
    has_previous_ = has_latest_;

    latest_ = packet;
    latest_receive_time_s_ = receive_time_s;
    has_latest_ = true;
}

bool RemoteAircraft::IsStale(double now_s, double timeout_s) const {
    return !has_latest_ || (now_s - latest_receive_time_s_) > timeout_s;
}

AircraftPose RemoteAircraft::ComputePose(double now_s) const {
    AircraftPose pose;
    if (!has_latest_) {
        return pose;
    }

    pose.gear_ratio = latest_.gear_ratio;
    pose.flap_ratio = latest_.flap_ratio;
    pose.speedbrake_ratio = latest_.speedbrake_ratio;
    pose.engine_ratio = latest_.engine_ratio;
    pose.light_bits = latest_.light_bits;

    const double sample_dt = latest_receive_time_s_ - previous_receive_time_s_;

    if (!has_previous_ || sample_dt <= 1e-6) {
        pose.latitude = latest_.latitude;
        pose.longitude = latest_.longitude;
        pose.elevation_m = latest_.elevation_m;
        pose.heading_deg = latest_.heading_deg;
        pose.pitch_deg = latest_.pitch_deg;
        pose.roll_deg = latest_.roll_deg;
        return pose;
    }

    double extrapolate_s = now_s - latest_receive_time_s_;
    extrapolate_s = std::clamp(extrapolate_s, -sample_dt, kMaxExtrapolationS);

    // Velocity estimated from the last two samples in a local flat-earth
    // meters frame centered on the latest sample. Fine for the tens-of-km
    // separations formation flying involves; not meant for round-the-world
    // accuracy.
    const double dlat_deg = latest_.latitude - previous_.latitude;
    const double dlon_deg = latest_.longitude - previous_.longitude;
    const double north_vel_mps = (dlat_deg * kPi / 180.0 * kEarthRadiusM) / sample_dt;
    const double east_vel_mps =
        (dlon_deg * kPi / 180.0 * kEarthRadiusM * std::cos(latest_.latitude * kPi / 180.0)) /
        sample_dt;
    const double vert_vel_mps = (latest_.elevation_m - previous_.elevation_m) / sample_dt;
    const double heading_rate_dps =
        WrapDegrees180(latest_.heading_deg - previous_.heading_deg) / sample_dt;

    const double north_m = north_vel_mps * extrapolate_s;
    const double east_m = east_vel_mps * extrapolate_s;

    pose.latitude = latest_.latitude + (north_m / kEarthRadiusM) * (180.0 / kPi);
    pose.longitude = latest_.longitude +
        (east_m / (kEarthRadiusM * std::cos(latest_.latitude * kPi / 180.0))) * (180.0 / kPi);
    pose.elevation_m = latest_.elevation_m + vert_vel_mps * extrapolate_s;
    pose.heading_deg =
        static_cast<float>(WrapDegrees360(latest_.heading_deg + heading_rate_dps * extrapolate_s));

    // Pitch/roll: hold at the latest value rather than extrapolating a rate
    // - short-lived attitude changes don't need dead reckoning as much as
    // position does for a first Formation-mode MVP.
    pose.pitch_deg = latest_.pitch_deg;
    pose.roll_deg = latest_.roll_deg;

    return pose;
}

} // namespace flytogether
