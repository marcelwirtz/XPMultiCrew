#include "sync/touchdown_detector.h"

#include <algorithm>
#include <cmath>

namespace flytogether {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6371008.8;

double WrapDeg(double deg) {
    return std::remainder(deg, 360.0); // -180..180
}
} // namespace

double DistanceMeters(double lat1, double lon1, double lat2, double lon2) {
    const double r = kPi / 180.0;
    const double dlat = (lat2 - lat1) * r;
    const double dlon = (lon2 - lon1) * r;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(lat1 * r) * std::cos(lat2 * r) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(a)));
}

void TouchdownDetector::Reset() {
    state_ = State::kUnknown;
    has_prev_ = false;
    g_window_.clear();
}

double TouchdownDetector::SmoothedG() const {
    if (g_window_.empty()) return 1.0;
    double sum = 0.0;
    for (const auto& [t, g] : g_window_) sum += g;
    return sum / static_cast<double>(g_window_.size());
}

void TouchdownDetector::StartFrom(const TouchdownSample& s) {
    // After a reset nothing is known about the approach, so an aircraft
    // placed in the air has to fly the arming time before a touchdown counts.
    state_ = s.on_ground ? State::kGround : State::kAirborne;
    airborne_since_s_ = s.time_s;
    max_agl_m_ = s.agl_m;
    armed_ = false;
    has_flare_point_ = false;
}

std::optional<LandingResult> TouchdownDetector::Finish(bool touch_and_go) {
    LandingResult out = pending_;
    out.touch_and_go = touch_and_go;
    return out;
}

std::optional<LandingResult> TouchdownDetector::Update(const TouchdownSample& s) {
    if (has_prev_) {
        const double dt = s.time_s - prev_.time_s;
        if (dt < 0.0 || dt > kMaxGapS ||
            DistanceMeters(prev_.latitude, prev_.longitude, s.latitude, s.longitude) > kJumpM) {
            state_ = State::kUnknown;
        }
    }
    if (state_ == State::kUnknown) {
        g_window_.clear();
    }
    g_window_.emplace_back(s.time_s, s.g_normal);
    while (!g_window_.empty() && g_window_.front().first < s.time_s - kGSmoothS) {
        g_window_.pop_front();
    }
    const TouchdownSample prev = prev_;
    const bool had_prev = has_prev_ && state_ != State::kUnknown;
    prev_ = s;
    has_prev_ = true;

    std::optional<LandingResult> result;
    switch (state_) {
        case State::kUnknown:
            StartFrom(s);
            break;

        case State::kGround:
            if (!s.on_ground) {
                state_ = State::kAirborne;
                airborne_since_s_ = s.time_s;
                max_agl_m_ = s.agl_m;
                armed_ = false;
                has_flare_point_ = false;
            }
            break;

        case State::kAirborne:
            if (!s.on_ground) {
                max_agl_m_ = std::max(max_agl_m_, s.agl_m);
                if (!armed_ && s.time_s - airborne_since_s_ >= kArmAirborneS && max_agl_m_ >= kArmAglM) {
                    armed_ = true;
                }
                // The last time it came down through 50 ft counts (a
                // go-around and second approach overwrites the first).
                if (had_prev && prev.agl_m >= kFlareGateM && s.agl_m < kFlareGateM) {
                    has_flare_point_ = true;
                    flare_lat_ = s.latitude;
                    flare_lon_ = s.longitude;
                } else if (s.agl_m >= kFlareGateM) {
                    has_flare_point_ = false;
                }
                break;
            }
            if (!armed_) {
                state_ = State::kGround; // a hop during the takeoff roll, not a landing
                break;
            }
            {
                // The touchdown frame's own vertical speed is already cut
                // by the gear compressing - the frame before is the honest one.
                const TouchdownSample& air = had_prev && !prev.on_ground ? prev : s;
                pending_ = LandingResult{};
                pending_.latitude = s.latitude;
                pending_.longitude = s.longitude;
                pending_.heading_true_deg = s.heading_true_deg;
                pending_.vs_fpm = std::min(air.vs_fpm, s.vs_fpm);
                pending_.peak_g = SmoothedG();
                pending_.groundspeed_kt = air.groundspeed_kt;
                pending_.drift_deg = air.groundspeed_kt > 5.0 ? WrapDeg(air.track_true_deg - air.heading_true_deg) : 0.0;
                pending_.flare_distance_m =
                    has_flare_point_ ? DistanceMeters(flare_lat_, flare_lon_, s.latitude, s.longitude) : -1.0;
                touchdown_s_ = s.time_s;
                peak_g_until_s_ = s.time_s + kPeakGWindowS;
                bounce_airborne_ = false;
                ground_since_s_ = s.time_s;
                state_ = State::kRollout;
            }
            break;

        case State::kRollout:
            if (s.time_s <= peak_g_until_s_) {
                pending_.peak_g = std::max(pending_.peak_g, SmoothedG());
            }
            if (!s.on_ground) {
                if (!bounce_airborne_) {
                    bounce_airborne_ = true;
                    bounce_since_s_ = s.time_s;
                }
                if (s.time_s - bounce_since_s_ >= kTouchAndGoS) {
                    result = Finish(/*touch_and_go=*/true);
                    // Still flying: the next touchdown counts right away.
                    state_ = State::kAirborne;
                    airborne_since_s_ = bounce_since_s_;
                    max_agl_m_ = s.agl_m;
                    armed_ = true;
                    has_flare_point_ = false;
                }
                break;
            }
            if (bounce_airborne_) {
                bounce_airborne_ = false;
                if (s.time_s - bounce_since_s_ >= kBounceMinAirS) {
                    ++pending_.bounces;
                    // A hard second contact counts toward the G rating too.
                    peak_g_until_s_ = s.time_s + kPeakGWindowS;
                }
                ground_since_s_ = s.time_s;
            }
            if (s.time_s - ground_since_s_ >= kSettleS && s.time_s >= peak_g_until_s_) {
                result = Finish(/*touch_and_go=*/false);
                state_ = State::kGround;
            }
            break;
    }
    return result;
}

} // namespace flytogether
