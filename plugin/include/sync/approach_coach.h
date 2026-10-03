#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flytogether {

// Approach Coach: a stabilized-approach monitor for the own aircraft.
//
// - 1000 ft AGL gate (only when established on an ILS - on a visual
//   circuit the aircraft is still on downwind/base there): a callout
//   whether the approach is stable.
// - 500 ft AGL gate (every approach): stable, or "UNSTABLE - GO AROUND"
//   with what's off.
// - Below 500 ft down to 50 ft: warnings when sink rate, bank, speed or
//   glideslope stay out of limits for a moment.
// - After the landing (or a go-around) a summary the companion attaches
//   to the Butter-Board landing.
//
// Only descending through a gate counts, and only after having been
// above kArmFt - a takeoff or low-level cruise doesn't trigger anything.
// Pure, no XPLM - plugin_main.cpp feeds it the datarefs.

struct ApproachSample {
    double time_s = 0.0; // sim running time; must not run while paused
    bool on_ground = false;
    double agl_ft = 0.0;
    double vs_fpm = 0.0; // positive up
    double ias_kt = 0.0;
    double bank_deg = 0.0;
    bool gear_retractable = false;
    double gear_ratio = 1.0; // 0 = up, 1 = down
    double vref_kt = 0.0;    // <= 0 = unknown (no speed checks)
    bool on_ils = false;     // localizer and glideslope both received
    double loc_dots = 0.0;
    double gs_dots = 0.0;    // positive = aircraft below the glideslope (fly up)
};

namespace ApproachDeviation {
constexpr uint32_t kSpeedHigh = 1 << 0;
constexpr uint32_t kSpeedLow = 1 << 1;
constexpr uint32_t kSinkRate = 1 << 2;
constexpr uint32_t kGearUp = 1 << 3;
constexpr uint32_t kBank = 1 << 4;
constexpr uint32_t kGlideslope = 1 << 5;
constexpr uint32_t kLocalizer = 1 << 6;
} // namespace ApproachDeviation

struct ApproachCallout {
    std::string text;
    double seconds = 4.0;
    bool alert = false; // unstable/warning (drawn in red) vs. informational
};

struct ApproachSummary {
    bool stable_at_500 = false;
    uint32_t deviations_at_500 = 0; // ApproachDeviation bits
    uint32_t warnings_below_500 = 0; // ApproachDeviation bits that triggered a warning below 500 ft
    double max_sink_below_500_fpm = 0.0; // positive
    bool go_around = false;
};

class ApproachCoach {
public:
    // Call every frame while the sim runs. Callouts to show go into
    // `callouts`; returns a summary once per approach that passed the
    // 500 ft gate (at touchdown or go-around).
    std::optional<ApproachSummary> Update(const ApproachSample& s, std::vector<ApproachCallout>& callouts);

    void Reset();

    // "speed +25 kt, sink 1300 fpm" - what the bits mean for this sample.
    static std::string Describe(uint32_t deviations, const ApproachSample& s);

    // Vref from the .acf's Vso (landing-configuration stall speed, given at
    // max weight like a POH's white arc): 1.3 x Vso. Aircraft over
    // kHeavyKg scale it with sqrt(weight / max weight) like an airliner's
    // weight-based Vref (a light A330 flies ~140 kt, not 1.3 x 125); light
    // aircraft keep the fixed POH speed their pilots actually fly.
    // Implausible Vso (unset in the .acf) = 0, which turns speed checks off.
    static double EstimateVrefKt(double vso_kt, double weight_kg, double max_weight_kg);
    static constexpr double kHeavyKg = 5700.0;

    // Tuning, public for the tests.
    static constexpr double kArmFt = 700.0;   // a 1000 ft AGL circuit arms it
    static constexpr double kGate1000Ft = 1000.0;
    static constexpr double kGate500Ft = 500.0;
    static constexpr double kWarnFloorFt = 50.0;  // no warnings in the flare
    static constexpr double kMaxSinkFpm = 1000.0;
    static constexpr double kMaxBankDeg = 30.0;
    static constexpr double kSpeedHighKt = 20.0;  // above vref
    static constexpr double kSpeedLowKt = 5.0;    // below vref
    static constexpr double kMaxDots = 1.0;
    static constexpr double kWarnPersistS = 1.5;  // out of limits this long before a warning
    static constexpr double kWarnRepeatS = 6.0;   // same warning again at most this often
    // Go-around: after the 500 ft gate, climbing and this far above the
    // lowest point since - a low pass back up to a ~900 ft circuit counts,
    // a hill under a level aircraft or a balloon in the flare doesn't.
    static constexpr double kGoAroundClimbFt = 200.0;
    static constexpr double kGoAroundVsFpm = 200.0;
    static constexpr double kMaxGapS = 2.0;       // bigger time gap = reposition/reload
    static constexpr double kJumpFt = 500.0;      // AGL jump between frames = reposition

private:
    uint32_t Deviations(const ApproachSample& s, bool with_gear) const;

    bool armed_ = false;
    bool past_500_ = false;
    double min_agl_since_500_ = 0.0;
    bool has_prev_ = false;
    ApproachSample prev_{};
    ApproachSummary summary_{};
    // Per warning bit: since when it's been out of limits (-1 = in limits)
    // and when it was last called out.
    double out_since_[7] = {-1, -1, -1, -1, -1, -1, -1};
    double last_warned_[7] = {-1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9};
};

} // namespace flytogether
