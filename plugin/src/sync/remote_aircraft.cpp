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

// If the rendered pose ever ends up more than this far from the freshly
// dead-reckoned target (a real reposition, or catching back up after a
// stale/dropped-connection gap), snap instead of blending - matches
// JoinFS-XP's own tuning for the same "when is this not just normal
// catch-up" threshold (JoinFS-XP.cpp's AdvancePosition, 50.0).
constexpr double kHardResetDistanceM = 50.0;

// Guards a single very-late ComputePose() call (e.g. after the sim was
// paused/stalled) from integrating one huge catch-up step.
constexpr double kMaxRenderDtS = 0.5;

// PD catch-up gain for the rendered-pose blend - the formula (base 0.8,
// scaling 0.03/m, clamped to 2.5, derivative 0.25) is from JoinFS's own
// docs/positioning-improvements.md Improvement 3 (github.com/tuduce/JoinFS
// - a design note that project wrote but never implemented itself);
// adapted to be explicitly dt-scaled (an Euler-integrated PD controller)
// rather than JoinFS's fixed per-tick gain, since ComputePose() is called
// at a variable frame rate, not a fixed poll tick - see
// remote_aircraft.h's class comment.
double PositionGain(double error_m) { return std::clamp(0.8 + error_m * 0.03, 0.8, 2.5); }
constexpr double kPosDerivative = 0.25;

// Exponential (frame-rate-independent) blend factor for orientation SLERP:
// converges roughly like JoinFS's own fixed per-tick t~0.5 at a ~80ms
// cadence, but scales smoothly to any actual frame time instead of being
// tuned to one specific tick rate.
constexpr double kOrientationBlendRate = 9.0; // 1/s

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

// Dead-reckons the *target* pose from the last two network samples -
// unchanged math from before the rendered-pose blend was added. Also
// hands back the estimated north/east/vertical velocity (0 if there's
// only one sample to go on), so ComputePose() can seed the rendered
// pose's own velocity state on the first call or a hard reset instead of
// starting from a motionless guess.
AircraftPose RemoteAircraft::ComputeTargetPose(double now_s, double* out_vel_north_mps, double* out_vel_east_mps,
                                                double* out_vel_vert_mps) const {
    AircraftPose pose;
    *out_vel_north_mps = 0.0;
    *out_vel_east_mps = 0.0;
    *out_vel_vert_mps = 0.0;
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

    *out_vel_north_mps = north_vel_mps;
    *out_vel_east_mps = east_vel_mps;
    *out_vel_vert_mps = vert_vel_mps;

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

AircraftPose RemoteAircraft::ComputePose(double now_s) const {
    double target_vel_north_mps = 0.0, target_vel_east_mps = 0.0, target_vel_vert_mps = 0.0;
    const AircraftPose target = ComputeTargetPose(now_s, &target_vel_north_mps, &target_vel_east_mps,
                                                    &target_vel_vert_mps);
    if (!has_latest_) {
        return target;
    }

    const Quaternion target_orientation =
        EulerDegToQuaternion(target.heading_deg, target.pitch_deg, target.roll_deg);

    // First-ever call, or the rendered pose has drifted far enough from
    // the target to be a real reposition/reconnect rather than ordinary
    // catch-up: snap instead of blending. See kHardResetDistanceM's
    // comment for why 50m.
    bool snap = !has_rendered_;
    double north_err_m = 0.0, east_err_m = 0.0, vert_err_m = 0.0;
    if (has_rendered_) {
        const double dlat_deg = target.latitude - rendered_lat_;
        const double dlon_deg = target.longitude - rendered_lon_;
        north_err_m = dlat_deg * kPi / 180.0 * kEarthRadiusM;
        east_err_m = dlon_deg * kPi / 180.0 * kEarthRadiusM * std::cos(rendered_lat_ * kPi / 180.0);
        vert_err_m = target.elevation_m - rendered_elevation_m_;
        const double distance_m =
            std::sqrt(north_err_m * north_err_m + east_err_m * east_err_m + vert_err_m * vert_err_m);
        snap = distance_m > kHardResetDistanceM;
    }

    if (snap) {
        rendered_lat_ = target.latitude;
        rendered_lon_ = target.longitude;
        rendered_elevation_m_ = target.elevation_m;
        rendered_vel_north_mps_ = target_vel_north_mps;
        rendered_vel_east_mps_ = target_vel_east_mps;
        rendered_vel_vert_mps_ = target_vel_vert_mps;
        rendered_orientation_ = target_orientation;
        last_render_time_s_ = now_s;
        has_rendered_ = true;
        return target;
    }

    // `dt_raw`: real elapsed time, only guarded against going negative
    // (a clock that went backwards) - used for the orientation SLERP,
    // which is unconditionally stable at any magnitude (as dt grows, the
    // blend factor asymptotically approaches 1, i.e. "fully caught up",
    // which is exactly correct - there's no integration-stability reason
    // to understate a real gap). `dt`: the same, but additionally capped
    // at kMaxRenderDtS - needed for the *position* PD integrator below,
    // which explicitly Euler-integrates a velocity every call and would
    // take one destabilizingly large step after a real stall/pause
    // (e.g. the sim paused, or this wasn't polled for a while) without
    // this cap. Conflating the two used to make orientation catch-up
    // artificially slow after any gap over kMaxRenderDtS - see
    // tests/shared_cockpit_sync_test.cpp's IngestRelayedPacket case,
    // which caught this.
    const double dt_raw = std::max(0.0, now_s - last_render_time_s_);
    const double dt = std::clamp(dt_raw, 0.0, kMaxRenderDtS);
    last_render_time_s_ = now_s;
    if (dt_raw <= 0.0) {
        // Called twice for the same instant (or a clock that went
        // backwards) - nothing to integrate, return what we already had.
        AircraftPose pose = target;
        pose.latitude = rendered_lat_;
        pose.longitude = rendered_lon_;
        pose.elevation_m = rendered_elevation_m_;
        const EulerAnglesDeg held = QuaternionToEulerDeg(rendered_orientation_);
        pose.heading_deg = static_cast<float>(WrapDegrees360(held.heading_deg));
        pose.pitch_deg = held.pitch_deg;
        pose.roll_deg = held.roll_deg;
        return pose;
    }

    const double distance_m =
        std::sqrt(north_err_m * north_err_m + east_err_m * east_err_m + vert_err_m * vert_err_m);
    const double gain = PositionGain(distance_m);
    // Derivative term damps relative to the *target's* velocity, not
    // toward zero - it's braking overshoot in the closing rate, not
    // bleeding off speed a steadily-moving target has already earned.
    // Subtracting a flat fraction of absolute velocity (matching JoinFS's
    // own per-tick formula literally) would decay the rendered pose's
    // velocity every step even at zero error, so it would keep falling
    // further behind a target moving at any nonzero constant velocity -
    // caught by TestTracksConstantVelocityAcrossMultipleFrames below.
    rendered_vel_north_mps_ +=
        (north_err_m * gain + (target_vel_north_mps - rendered_vel_north_mps_) * kPosDerivative) * dt;
    rendered_vel_east_mps_ +=
        (east_err_m * gain + (target_vel_east_mps - rendered_vel_east_mps_) * kPosDerivative) * dt;
    rendered_vel_vert_mps_ +=
        (vert_err_m * gain + (target_vel_vert_mps - rendered_vel_vert_mps_) * kPosDerivative) * dt;

    const double d_north_m = rendered_vel_north_mps_ * dt;
    const double d_east_m = rendered_vel_east_mps_ * dt;
    rendered_lat_ += (d_north_m / kEarthRadiusM) * (180.0 / kPi);
    rendered_lon_ += (d_east_m / (kEarthRadiusM * std::cos(rendered_lat_ * kPi / 180.0))) * (180.0 / kPi);
    rendered_elevation_m_ += rendered_vel_vert_mps_ * dt;

    const double orientation_alpha = 1.0 - std::exp(-kOrientationBlendRate * dt_raw);
    rendered_orientation_ =
        Slerp(rendered_orientation_, target_orientation, static_cast<float>(orientation_alpha));
    const EulerAnglesDeg rendered_euler = QuaternionToEulerDeg(rendered_orientation_);

    AircraftPose pose = target; // gear/flap/speedbrake/engine/light bits: held, not blended
    pose.latitude = rendered_lat_;
    pose.longitude = rendered_lon_;
    pose.elevation_m = rendered_elevation_m_;
    pose.heading_deg = static_cast<float>(WrapDegrees360(rendered_euler.heading_deg));
    pose.pitch_deg = rendered_euler.pitch_deg;
    pose.roll_deg = rendered_euler.roll_deg;
    return pose;
}

} // namespace flytogether
