// Pure-math test for the Euler->quaternion conversion used when Shared
// Cockpit hands physics control back after overriding it (see
// shared_cockpit/quaternion.h). No XPLM dependency.

#include "shared_cockpit/quaternion.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace flytogether;

namespace {

bool NearlyEqual(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

void TestIdentityAtZeroAngles() {
    const Quaternion q = EulerDegToQuaternion(0.0f, 0.0f, 0.0f);
    assert(NearlyEqual(q.q0, 1.0, 1e-6));
    assert(NearlyEqual(q.q1, 0.0, 1e-6));
    assert(NearlyEqual(q.q2, 0.0, 1e-6));
    assert(NearlyEqual(q.q3, 0.0, 1e-6));
    std::printf("TestIdentityAtZeroAngles: OK\n");
}

void TestIsAlwaysUnitLength() {
    const float angles[] = {0.0f, 15.0f, 45.0f, 90.0f, 179.0f, -37.0f, 200.0f, 359.0f};
    for (float psi : angles) {
        for (float theta : angles) {
            for (float phi : angles) {
                const Quaternion q = EulerDegToQuaternion(psi, theta, phi);
                const double norm_sq =
                    double(q.q0) * q.q0 + double(q.q1) * q.q1 + double(q.q2) * q.q2 + double(q.q3) * q.q3;
                assert(NearlyEqual(norm_sq, 1.0, 1e-4));
            }
        }
    }
    std::printf("TestIsAlwaysUnitLength: OK\n");
}

// QuaternionToEulerDeg is the inverse of EulerDegToQuaternion. Verified by
// round-tripping through the quaternion (forward -> inverse -> forward)
// rather than comparing angles directly, since angles alone are ambiguous
// (wrap-around, and genuine gimbal-lock at pitch = +/-90 where many
// heading/roll splits produce the same rotation) - the *quaternion* a
// given rotation produces is unambiguous up to overall sign (q and -q are
// the same rotation - quaternions doubly-cover SO(3); e.g. a 271 degree
// heading and its wrapped-equivalent -89 degrees differ by a full 360
// degrees, which is only a 180 degree shift in the half-angle these
// formulas actually use, flipping every component's sign at once - this
// is expected and is exactly why Slerp() explicitly checks the dot
// product's sign before blending), so equal-or-negated quaternions in are
// proof the inverse recovered an equivalent rotation.
void TestEulerQuaternionRoundTrip() {
    const float headings[] = {0.0f, 15.0f, 90.0f, 179.0f, -37.0f, 271.0f};
    const float pitches[] = {0.0f, 10.0f, -25.0f, 45.0f, -60.0f};
    const float rolls[] = {0.0f, 5.0f, -45.0f, 90.0f, 178.0f, -170.0f};
    for (float h : headings) {
        for (float p : pitches) {
            for (float r : rolls) {
                const Quaternion q1 = EulerDegToQuaternion(h, p, r);
                const EulerAnglesDeg back = QuaternionToEulerDeg(q1);
                const Quaternion q2 = EulerDegToQuaternion(back.heading_deg, back.pitch_deg, back.roll_deg);
                const bool same_sign = NearlyEqual(q1.q0, q2.q0, 1e-3) && NearlyEqual(q1.q1, q2.q1, 1e-3) &&
                                        NearlyEqual(q1.q2, q2.q2, 1e-3) && NearlyEqual(q1.q3, q2.q3, 1e-3);
                const bool flipped_sign = NearlyEqual(q1.q0, -q2.q0, 1e-3) && NearlyEqual(q1.q1, -q2.q1, 1e-3) &&
                                           NearlyEqual(q1.q2, -q2.q2, 1e-3) && NearlyEqual(q1.q3, -q2.q3, 1e-3);
                assert(same_sign || flipped_sign);
            }
        }
    }
    std::printf("TestEulerQuaternionRoundTrip: OK\n");
}

void TestSlerpEndpointsAndMidpoint() {
    const Quaternion a = EulerDegToQuaternion(0.0f, 0.0f, 0.0f);
    const Quaternion b = EulerDegToQuaternion(90.0f, 0.0f, 0.0f);

    const Quaternion at0 = Slerp(a, b, 0.0f);
    assert(NearlyEqual(at0.q0, a.q0, 1e-5));
    assert(NearlyEqual(at0.q1, a.q1, 1e-5));
    assert(NearlyEqual(at0.q2, a.q2, 1e-5));
    assert(NearlyEqual(at0.q3, a.q3, 1e-5));

    const Quaternion at1 = Slerp(a, b, 1.0f);
    assert(NearlyEqual(at1.q0, b.q0, 1e-5));
    assert(NearlyEqual(at1.q3, b.q3, 1e-5));

    // Halfway between a 0 and 90 degree heading should be ~45 degrees.
    const Quaternion mid = Slerp(a, b, 0.5f);
    const EulerAnglesDeg mid_angles = QuaternionToEulerDeg(mid);
    assert(NearlyEqual(mid_angles.heading_deg, 45.0, 1e-2));
    std::printf("TestSlerpEndpointsAndMidpoint: OK\n");
}

void TestSlerpTakesShorterPath() {
    // 10 degrees and 350 degrees are 20 degrees apart through north, not
    // 340 degrees apart the "long way" - the blend must go through 0/360,
    // matching RemoteAircraft's existing heading-wrap expectations.
    const Quaternion a = EulerDegToQuaternion(350.0f, 0.0f, 0.0f);
    const Quaternion b = EulerDegToQuaternion(10.0f, 0.0f, 0.0f);
    const Quaternion mid = Slerp(a, b, 0.5f);
    const EulerAnglesDeg mid_angles = QuaternionToEulerDeg(mid);
    // Expect ~0 (i.e. ~360), not ~180.
    const float wrapped = mid_angles.heading_deg < 0.0f ? mid_angles.heading_deg + 360.0f : mid_angles.heading_deg;
    assert(wrapped < 10.0f || wrapped > 350.0f);
    std::printf("TestSlerpTakesShorterPath: OK\n");
}

} // namespace

int main() {
    TestIdentityAtZeroAngles();
    TestIsAlwaysUnitLength();
    TestEulerQuaternionRoundTrip();
    TestSlerpEndpointsAndMidpoint();
    TestSlerpTakesShorterPath();
    std::printf("All quaternion tests passed.\n");
    return 0;
}
