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

} // namespace

int main() {
    TestHoldsLastKnownPoseWithOnlyOneSample();
    TestExtrapolatesLinearMotion();
    TestHeadingWrapsAcrossNorth();
    TestDropsOutOfOrderAndDuplicatePackets();
    TestStopsExtrapolatingAfterTimeout();
    std::printf("All remote_aircraft tests passed.\n");
    return 0;
}
