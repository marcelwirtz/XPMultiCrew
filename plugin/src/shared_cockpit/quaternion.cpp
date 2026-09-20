#include "shared_cockpit/quaternion.h"

#include <algorithm>
#include <cmath>

namespace flytogether {

Quaternion EulerDegToQuaternion(float psi_deg, float theta_deg, float phi_deg) {
    constexpr double kPi = 3.14159265358979323846;
    // Half-angle in radians: X-Plane's formula uses pi/360 (i.e. half of
    // pi/180) per developer.x-plane.com/article/movingtheplane/.
    const double psi = kPi / 360.0 * psi_deg;
    const double theta = kPi / 360.0 * theta_deg;
    const double phi = kPi / 360.0 * phi_deg;

    const double cpsi = std::cos(psi), spsi = std::sin(psi);
    const double ctheta = std::cos(theta), stheta = std::sin(theta);
    const double cphi = std::cos(phi), sphi = std::sin(phi);

    Quaternion q;
    q.q0 = static_cast<float>(cpsi * ctheta * cphi + spsi * stheta * sphi);
    q.q1 = static_cast<float>(cpsi * ctheta * sphi - spsi * stheta * cphi);
    q.q2 = static_cast<float>(cpsi * stheta * cphi + spsi * ctheta * sphi);
    q.q3 = static_cast<float>(-cpsi * stheta * sphi + spsi * ctheta * cphi);
    return q;
}

EulerAnglesDeg QuaternionToEulerDeg(const Quaternion& q) {
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    const double q0 = q.q0, q1 = q.q1, q2 = q.q2, q3 = q.q3;

    // Standard ZYX (yaw/heading * pitch * roll, intrinsic) quaternion->Euler
    // extraction - verified against EulerDegToQuaternion above by expanding
    // its Hamilton product qz(heading) (x) qy(pitch) (x) qx(roll) by hand,
    // not guessed; see tests/quaternion_test.cpp's round-trip test.
    EulerAnglesDeg out;
    out.heading_deg = static_cast<float>(std::atan2(2.0 * (q0 * q3 + q1 * q2), 1.0 - 2.0 * (q2 * q2 + q3 * q3)) *
                                          kRadToDeg);
    const double sin_pitch = std::clamp(2.0 * (q0 * q2 - q3 * q1), -1.0, 1.0);
    out.pitch_deg = static_cast<float>(std::asin(sin_pitch) * kRadToDeg);
    out.roll_deg = static_cast<float>(std::atan2(2.0 * (q0 * q1 + q2 * q3), 1.0 - 2.0 * (q1 * q1 + q2 * q2)) *
                                       kRadToDeg);
    return out;
}

Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t) {
    double bx = b.q1, by = b.q2, bz = b.q3, bw = b.q0;
    double dot = double(a.q0) * bw + double(a.q1) * bx + double(a.q2) * by + double(a.q3) * bz;

    // Take the shorter path around the 4D unit sphere.
    if (dot < 0.0) {
        bw = -bw;
        bx = -bx;
        by = -by;
        bz = -bz;
        dot = -dot;
    }

    double sa, sb;
    if (dot > 0.9995) {
        // Nearly identical - general SLERP's sin(theta) denominator would
        // be ~0 here, so fall back to a linear blend (renormalized below).
        sa = 1.0 - t;
        sb = t;
    } else {
        const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
        const double sin_theta = std::sin(theta);
        sa = std::sin((1.0 - t) * theta) / sin_theta;
        sb = std::sin(t * theta) / sin_theta;
    }

    Quaternion out;
    out.q0 = static_cast<float>(sa * a.q0 + sb * bw);
    out.q1 = static_cast<float>(sa * a.q1 + sb * bx);
    out.q2 = static_cast<float>(sa * a.q2 + sb * by);
    out.q3 = static_cast<float>(sa * a.q3 + sb * bz);

    const double norm = std::sqrt(double(out.q0) * out.q0 + double(out.q1) * out.q1 + double(out.q2) * out.q2 +
                                   double(out.q3) * out.q3);
    if (norm > 1e-9) {
        out.q0 = static_cast<float>(out.q0 / norm);
        out.q1 = static_cast<float>(out.q1 / norm);
        out.q2 = static_cast<float>(out.q2 / norm);
        out.q3 = static_cast<float>(out.q3 / norm);
    }
    return out;
}

} // namespace flytogether
