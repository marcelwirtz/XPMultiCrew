#pragma once

#include "flytogether/aircraft_state.h"

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
// Pure math, no XPLM/network dependency - see tests/remote_aircraft_test.cpp.
class RemoteAircraft {
public:
    // `receive_time_s` and the `now_s` passed to ComputePose()/IsStale()
    // must share one clock (in the plugin, XPLMGetElapsedTime()). Packets
    // that are not newer than the last accepted one (by sequence number,
    // wraparound-safe) are ignored.
    void OnPacketReceived(const AircraftStatePacket& packet, double receive_time_s);

    AircraftPose ComputePose(double now_s) const;

    bool HasData() const { return has_latest_; }
    const AircraftStatePacket& latest() const { return latest_; }
    bool IsStale(double now_s, double timeout_s = 8.0) const;

private:
    AircraftStatePacket previous_{};
    AircraftStatePacket latest_{};
    double previous_receive_time_s_ = 0.0;
    double latest_receive_time_s_ = 0.0;
    bool has_previous_ = false;
    bool has_latest_ = false;
};

} // namespace flytogether
