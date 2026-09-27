#pragma once

#include <deque>
#include <optional>
#include <utility>

namespace flytogether {

// Landing rating ("Butter-Board"): watches the own aircraft frame by frame
// and reports each landing once it has settled - the sink rate at the
// moment the gear touched, the peak G right after, drift, bounces and how
// far the aircraft floated from 50 ft AGL to touchdown. Runway-relative
// numbers (distance past the threshold, centerline offset) are worked out
// by the companion app, which has the runways from apt.dat.
//
// Pure, no XPLM - plugin_main.cpp feeds it the datarefs.

struct TouchdownSample {
    double time_s = 0.0;  // sim elapsed time; must not run while paused
    bool on_ground = false;
    double vs_fpm = 0.0;  // vertical speed, positive up
    double g_normal = 1.0;
    double agl_m = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    double heading_true_deg = 0.0;
    double groundspeed_kt = 0.0;
    double track_true_deg = 0.0; // direction of movement over the ground
};

struct LandingResult {
    // Where and how the gear first touched.
    double latitude = 0.0;
    double longitude = 0.0;
    double heading_true_deg = 0.0;
    double vs_fpm = 0.0;     // negative = descending
    double peak_g = 1.0;     // highest load factor (0.2 s average) in the second after touchdown (and any bounce)
    double groundspeed_kt = 0.0;
    double drift_deg = 0.0;  // track minus heading, positive = moving right of the nose
    int bounces = 0;
    double flare_distance_m = -1.0; // 50 ft AGL -> touchdown, -1 if the aircraft never came down through 50 ft
    bool touch_and_go = false;      // lifted off again instead of rolling out
};

class TouchdownDetector {
public:
    // Call every frame while the sim runs. Returns a result once per
    // landing, a few seconds after touchdown (after the bounces are over).
    std::optional<LandingResult> Update(const TouchdownSample& sample);

    // Forget everything (aircraft reload, reposition).
    void Reset();

    // Tuning, public for the tests.
    static constexpr double kArmAirborneS = 3.0;   // continuously airborne this long ...
    static constexpr double kArmAglM = 10.0;       // ... and this high before a touchdown counts
    static constexpr double kFlareGateM = 15.24;   // 50 ft
    static constexpr double kPeakGWindowS = 1.0;
    // X-Plane's g_nrml spikes for a single frame when the gear touches
    // (2.5+ G on a 150 fpm landing) - the peak is taken of the average
    // over this window instead.
    static constexpr double kGSmoothS = 0.2;
    static constexpr double kBounceMinAirS = 0.2;  // shorter hops are gear-contact flicker
    static constexpr double kSettleS = 2.5;        // on the ground this long = landing over
    static constexpr double kTouchAndGoS = 5.0;    // airborne again this long = touch-and-go
    static constexpr double kJumpM = 1000.0;       // position jump between frames = reposition
    static constexpr double kMaxGapS = 2.0;        // bigger time gap = reposition/reload

private:
    enum class State { kUnknown, kGround, kAirborne, kRollout };

    std::optional<LandingResult> Finish(bool touch_and_go);
    void StartFrom(const TouchdownSample& sample);

    double SmoothedG() const;

    State state_ = State::kUnknown;
    std::deque<std::pair<double, double>> g_window_; // (time, g)
    TouchdownSample prev_{};
    bool has_prev_ = false;

    // Airborne
    double airborne_since_s_ = 0.0;
    double max_agl_m_ = 0.0;
    bool armed_ = false;
    bool has_flare_point_ = false;
    double flare_lat_ = 0.0;
    double flare_lon_ = 0.0;

    // Rollout
    LandingResult pending_{};
    double touchdown_s_ = 0.0;
    double peak_g_until_s_ = 0.0;
    bool bounce_airborne_ = false;
    double bounce_since_s_ = 0.0;   // start of the current airborne phase / ground contact
    double ground_since_s_ = 0.0;
};

// Great-circle distance in metres.
double DistanceMeters(double lat1, double lon1, double lat2, double lon2);

} // namespace flytogether
