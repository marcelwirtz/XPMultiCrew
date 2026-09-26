// Pure-math regression test for the Phase 1 dead-reckoning logic. No XPLM
// SDK or networking involved - build and run with:
//   g++ -std=c++17 -I plugin/include -I netcore/include \
//       tests/remote_aircraft_test.cpp plugin/src/sync/remote_aircraft.cpp \
//       -o /tmp/remote_aircraft_test && /tmp/remote_aircraft_test
// or via CTest (see top-level CMakeLists.txt).

#include "sync/remote_aircraft.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using flytogether::AircraftStatePacket;
using flytogether::RemoteAircraft;

namespace {

bool NearlyEqual(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

void TestHoldsLastKnownPoseWithOnlyOneSample() {
    RemoteAircraft aircraft;
    AircraftStatePacket packet;
    packet.sequence = 1;
    packet.latitude = 47.0;
    packet.longitude = 8.0;
    packet.elevation_m = 500.0;
    packet.heading_deg = 90.0f;

    aircraft.OnPacketReceived(packet, /*receive_time_s=*/10.0);

    const auto pose = aircraft.ComputePose(/*now_s=*/12.0);
    assert(NearlyEqual(pose.latitude, 47.0, 1e-9));
    assert(NearlyEqual(pose.longitude, 8.0, 1e-9));
    assert(NearlyEqual(pose.heading_deg, 90.0, 1e-6));
    std::printf("TestHoldsLastKnownPoseWithOnlyOneSample: OK\n");
}

void TestExtrapolatesLinearMotion() {
    RemoteAircraft aircraft;

    AircraftStatePacket a;
    a.sequence = 1;
    a.latitude = 47.0;
    a.longitude = 8.0;
    a.elevation_m = 1000.0;
    a.heading_deg = 0.0f;
    aircraft.OnPacketReceived(a, 0.0);

    AircraftStatePacket b = a;
    b.sequence = 2;
    b.latitude = 47.001; // ~111 m north over 1 second
    b.elevation_m = 1010.0;
    aircraft.OnPacketReceived(b, 1.0);

    const auto pose = aircraft.ComputePose(1.5); // 0.5s past the latest sample
    assert(pose.latitude > b.latitude);
    assert(NearlyEqual(pose.latitude, 47.0015, 1e-3));
    assert(NearlyEqual(pose.elevation_m, 1015.0, 1.0));
    std::printf("TestExtrapolatesLinearMotion: OK\n");
}

void TestHeadingWrapsAcrossNorth() {
    RemoteAircraft aircraft;

    AircraftStatePacket a;
    a.sequence = 1;
    a.heading_deg = 350.0f;
    aircraft.OnPacketReceived(a, 0.0);

    AircraftStatePacket b = a;
    b.sequence = 2;
    b.heading_deg = 10.0f; // turned 20 degrees through north, not -340
    aircraft.OnPacketReceived(b, 1.0);

    const auto pose = aircraft.ComputePose(1.5);
    // Must keep turning through north (towards ~20), not snap back below 10.
    assert(pose.heading_deg > 10.0f && pose.heading_deg < 30.0f);
    std::printf("TestHeadingWrapsAcrossNorth: OK\n");
}

void TestDropsOutOfOrderAndDuplicatePackets() {
    RemoteAircraft aircraft;

    AircraftStatePacket a;
    a.sequence = 5;
    a.latitude = 47.0;
    aircraft.OnPacketReceived(a, 0.0);

    AircraftStatePacket stale = a;
    stale.sequence = 3;
    stale.latitude = 99.0; // must be ignored: older sequence number
    aircraft.OnPacketReceived(stale, 1.0);

    const auto pose = aircraft.ComputePose(1.0);
    assert(NearlyEqual(pose.latitude, 47.0, 1e-9));
    std::printf("TestDropsOutOfOrderAndDuplicatePackets: OK\n");
}

void TestStopsExtrapolatingAfterTimeout() {
    RemoteAircraft aircraft;

    AircraftStatePacket a;
    a.sequence = 1;
    a.latitude = 47.0;
    aircraft.OnPacketReceived(a, 0.0);

    AircraftStatePacket b = a;
    b.sequence = 2;
    b.latitude = 47.01;
    aircraft.OnPacketReceived(b, 1.0);

    // Far beyond the internal extrapolation cap - must not fly off forever.
    const auto pose = aircraft.ComputePose(30.0);
    const double expected = 47.01 + (47.01 - 47.0) * 1.5; // capped at +1.5s
    assert(NearlyEqual(pose.latitude, expected, 1e-2));
    assert(aircraft.IsStale(30.0));
    std::printf("TestStopsExtrapolatingAfterTimeout: OK\n");
}

// The rendered-pose catch-up blend (see remote_aircraft.h's class comment)
// only ever engages from the *second* ComputePose() call onward - the
// first call always snaps exactly to the dead-reckoned target, which is
// why every test above (each calling ComputePose() exactly once) still
// holds unchanged. These tests call it repeatedly to exercise the blend
// itself.

void TestTracksConstantVelocityAcrossMultipleFrames() {
    RemoteAircraft aircraft;
    AircraftStatePacket a;
    a.sequence = 1;
    a.latitude = 47.0;
    a.longitude = 8.0;
    a.elevation_m = 1000.0;
    aircraft.OnPacketReceived(a, 0.0);

    AircraftStatePacket b = a;
    b.sequence = 2;
    b.latitude = 47.0001; // ~11 m/s north
    aircraft.OnPacketReceived(b, 1.0);

    const auto snapped = aircraft.ComputePose(1.0); // first call: snaps, also seeds rendered velocity
    assert(NearlyEqual(snapped.latitude, b.latitude, 1e-9));

    // Once the rendered velocity has been seeded to match the target's
    // own (steady) velocity, repeated calls should track the constant-
    // velocity target closely rather than drifting or oscillating away
    // from it, even though each step re-runs the PD blend.
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kEarthRadiusM = 6378137.0;
    const double target_vel_north_mps = (b.latitude - a.latitude) * kPi / 180.0 * kEarthRadiusM; // / 1.0s
    for (int i = 1; i <= 20; ++i) {
        const double t = 1.0 + i * 0.05;
        const auto pose = aircraft.ComputePose(t);
        const double expected_lat =
            b.latitude + (target_vel_north_mps * (t - 1.0) / kEarthRadiusM) * (180.0 / kPi);
        assert(NearlyEqual(pose.latitude, expected_lat, 5e-6)); // ~0.5 m tolerance
    }
    std::printf("TestTracksConstantVelocityAcrossMultipleFrames: OK\n");
}

void TestBlendsTowardTargetInsteadOfSnapping() {
    RemoteAircraft aircraft;
    AircraftStatePacket a;
    a.sequence = 1;
    a.latitude = 47.0;
    a.longitude = 8.0;
    aircraft.OnPacketReceived(a, 0.0);

    AircraftStatePacket b = a;
    b.sequence = 2;
    aircraft.OnPacketReceived(b, 1.0);
    const auto snapped = aircraft.ComputePose(1.0); // establishes the rendered baseline at b, zero velocity

    // A third packet jumps the target ~20 m north - a real discontinuity,
    // but comfortably under the 50 m hard-reset threshold.
    AircraftStatePacket c = b;
    c.sequence = 3;
    c.latitude = 47.00018;
    aircraft.OnPacketReceived(c, 1.05);

    const auto soon_after = aircraft.ComputePose(1.08); // 30 ms later
    // Moved toward the new target, but a 30 ms step of a damped blend
    // can't have already caught all the way up - proves this is a catch-
    // up, not an instant snap to the new target.
    assert(soon_after.latitude > snapped.latitude);
    assert(soon_after.latitude < c.latitude - 1e-6);
    std::printf("TestBlendsTowardTargetInsteadOfSnapping: OK\n");
}

void TestHardResetSnapsForLargeJump() {
    RemoteAircraft aircraft;
    AircraftStatePacket a;
    a.sequence = 1;
    a.latitude = 47.0;
    a.longitude = 8.0;
    aircraft.OnPacketReceived(a, 0.0);

    AircraftStatePacket b = a;
    b.sequence = 2;
    aircraft.OnPacketReceived(b, 1.0);
    aircraft.ComputePose(1.0); // establishes the rendered baseline

    // ~1 km jump - a real reposition, not ordinary catch-up.
    AircraftStatePacket c = b;
    c.sequence = 3;
    c.latitude = 47.009;
    aircraft.OnPacketReceived(c, 1.05);

    // Evaluated at exactly c's receive time (no extrapolation past it, so
    // the target is exactly c regardless of the huge implied velocity a
    // ~1 km jump over 0.05 s works out to) - isolates the assertion to
    // "did the hard reset snap" rather than any extrapolation beyond c.
    const auto pose = aircraft.ComputePose(1.05);
    assert(NearlyEqual(pose.latitude, c.latitude, 1e-9)); // snapped exactly, not blending toward it
    std::printf("TestHardResetSnapsForLargeJump: OK\n");
}

} // namespace

// protocol_version 2's ref_height_agl_m reaches the pose; a version 1
// sender's packet (shorter, so the field keeps its default) reads as
// unknown rather than as whatever bytes happen to be there.
void TestRefHeightOnlyFromVersion2Senders() {
    AircraftStatePacket v2;
    v2.sequence = 1;
    v2.ref_height_agl_m = 2.3f;
    RemoteAircraft a;
    a.OnPacketReceived(v2, 1.0);
    assert(NearlyEqual(a.ComputePose(1.0).ref_height_agl_m, 2.3, 1e-6));

    AircraftStatePacket v1 = v2;
    v1.protocol_version = 1;
    RemoteAircraft b;
    b.OnPacketReceived(v1, 1.0);
    assert(b.ComputePose(1.0).ref_height_agl_m < 0.0f);
    std::printf("TestRefHeightOnlyFromVersion2Senders: OK\n");
}

int main() {
    TestHoldsLastKnownPoseWithOnlyOneSample();
    TestExtrapolatesLinearMotion();
    TestHeadingWrapsAcrossNorth();
    TestDropsOutOfOrderAndDuplicatePackets();
    TestStopsExtrapolatingAfterTimeout();
    TestTracksConstantVelocityAcrossMultipleFrames();
    TestBlendsTowardTargetInsteadOfSnapping();
    TestHardResetSnapsForLargeJump();
    TestRefHeightOnlyFromVersion2Senders();
    std::printf("All remote_aircraft tests passed.\n");
    return 0;
}
