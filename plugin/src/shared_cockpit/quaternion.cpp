#include "shared_cockpit/quaternion.h"

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

} // namespace flytogether
