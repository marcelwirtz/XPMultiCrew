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

} // namespace

int main() {
    TestIdentityAtZeroAngles();
    TestIsAlwaysUnitLength();
    std::printf("All quaternion tests passed.\n");
    return 0;
}
