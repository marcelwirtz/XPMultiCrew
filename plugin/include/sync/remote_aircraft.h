#pragma once

#include "flytogether/aircraft_state.h"
#include "shared_cockpit/quaternion.h"

namespace flytogether {

// The current best-guess pose for a remote aircraft: either its last known
// state directly, or a dead-reckoned extrapolation between updates.
struct AircraftPose {
    double latitude = 0.0;
    double longitude = 0.0;
    double elevation_m = 0.0;
    float heading_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    float gear_ratio = 0.0f;
    float flap_ratio = 0.0f;
    float speedbrake_ratio = 0.0f;
    float engine_ratio = 0.0f;
    uint8_t light_bits = 0;
};

// Tracks one remote aircraft's network-reported state and extrapolates a
// smooth pose between updates ("dead reckoning") from the last two received
// samples, so drawing doesn't have to wait for (or visibly snap on) every
// incoming packet. See docs/plan.md section 5.
//
// ComputePose() does two things, not one: it dead-reckons a *target* pose
// from the last two network samples (exactly as before), then blends an
// internally-tracked *rendered* pose toward that target - a proportional/
// derivative catch-up for position (damped so it converges without
// oscillating) and a quaternion SLERP for orientation (avoids the Euler-
// angle gimbal-lock pops a raw pitch/roll/heading blend would produce in a
// steep bank or pitch). Without this second step, every new packet redraws
// the aircraft at a freshly-recomputed target position - fine as a target,
// but visibly "pops" frame to frame instead of gliding smoothly, which is
// what the rendered-pose blend fixes. A single hard-reset distance
// threshold still applies for large errors (a real reposition, or catching
// up after a stale/dropped-connection gap) - see the .cpp.
//
// This design and its tuning are informed by JoinFS's X-Plane-side
// AdvancePosition (github.com/tuduce/JoinFS, JoinFS-XP/JoinFS-XP.cpp) and
// that project's own (never-implemented-by-JoinFS-itself) improvement
// notes in docs/positioning-improvements.md - adapted here to X-Plane's
// override_planepath model, which needs an absolute position handed to it
// every frame (unlike SimConnect's continuously-integrated object-velocity
// model JoinFS's own gain formula was tuned against), so the blend below is
// an explicit, dt-scaled PD integrator rather than a literal port.
//
// Pure math, no XPLM/network dependency - see tests/remote_aircraft_test.cpp.
class RemoteAircraft {
public:
    // `receive_time_s` and the `now_s` passed to ComputePose()/IsStale()
    // must share one clock (in the plugin, XPLMGetElapsedTime()). Packets
    // that are not newer than the last accepted one (by sequence number,
    // wraparound-safe) are ignored.
    void OnPacketReceived(const AircraftStatePacket& packet, double receive_time_s);

    // Call every frame (both current callers already do). Marked `const`
    // even though it advances the rendered-pose blend described above -
    // that blend is a rendering-side cache of "what smoothed pose did we
    // last show", not part of this object's externally-observable network
    // state, so it lives in the `mutable` members below. This keeps the
    // public signature - and every existing call site, including const
    // contexts like FormationSync::ForEachRemoteAircraft - unchanged.
    AircraftPose ComputePose(double now_s) const;

    bool HasData() const { return has_latest_; }
    const AircraftStatePacket& latest() const { return latest_; }
    bool IsStale(double now_s, double timeout_s = 8.0) const;

private:
    AircraftPose ComputeTargetPose(double now_s, double* out_vel_north_mps, double* out_vel_east_mps,
                                    double* out_vel_vert_mps) const;

    AircraftStatePacket previous_{};
    AircraftStatePacket latest_{};
    double previous_receive_time_s_ = 0.0;
    double latest_receive_time_s_ = 0.0;
    bool has_previous_ = false;
    bool has_latest_ = false;

    // Rendered-pose blend state - see the class comment above.
    mutable bool has_rendered_ = false;
    mutable double rendered_lat_ = 0.0;
    mutable double rendered_lon_ = 0.0;
    mutable double rendered_elevation_m_ = 0.0;
    mutable double rendered_vel_north_mps_ = 0.0;
    mutable double rendered_vel_east_mps_ = 0.0;
    mutable double rendered_vel_vert_mps_ = 0.0;
    mutable Quaternion rendered_orientation_{};
    mutable double last_render_time_s_ = 0.0;
};

} // namespace flytogether
