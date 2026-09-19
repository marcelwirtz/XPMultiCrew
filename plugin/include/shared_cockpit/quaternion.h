#pragma once

// Euler (psi/theta/phi, degrees) -> X-Plane's local-OpenGL-to-aircraft
// rotation quaternion, needed to hand physics control back cleanly after
// Shared Cockpit's override_planepath is released (docs/plan.md section 6;
// formula and axis/quaternion-component conventions are X-Plane's own, see
// https://developer.x-plane.com/article/movingtheplane/
// "Orienting the Aircraft in Space" / "Transitioning From Disabled to
// Enabled Flight Model").
//
// Pure math, no XPLM dependency - see tests/quaternion_test.cpp.

namespace flytogether {

struct Quaternion {
    float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;
};

Quaternion EulerDegToQuaternion(float psi_deg, float theta_deg, float phi_deg);

} // namespace flytogether
