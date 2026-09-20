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

// Inverse of EulerDegToQuaternion: recovers heading (psi), pitch (theta),
// roll (phi) in degrees from a unit quaternion in the same convention.
// Ambiguous (infinitely many equivalent psi/phi splits) exactly at the
// pitch = +/-90 deg gimbal-lock singularity, same as any Euler
// representation - not a new limitation introduced here, just inherent to
// converting back out of quaternion form.
struct EulerAnglesDeg {
    float heading_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
};
EulerAnglesDeg QuaternionToEulerDeg(const Quaternion& q);

// Spherical linear interpolation from `a` to `b`, t in [0, 1] (0 = a, 1 =
// b). Used to blend a remote aircraft's rendered orientation toward its
// dead-reckoned target every frame without the gimbal-lock pops a raw
// per-axis Euler blend produces in a steep bank or pitch - see
// sync/remote_aircraft.h's class comment. Takes the shorter of the two
// paths around the rotation (negates `b` first if the quaternions are more
// than 90 degrees apart as 4D vectors) and falls back to linear
// interpolation (then re-normalizes) when `a`/`b` are nearly identical, to
// avoid a division by ~0 in the general SLERP formula.
Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t);

} // namespace flytogether
