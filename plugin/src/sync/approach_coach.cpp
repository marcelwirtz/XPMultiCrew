#include "sync/approach_coach.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace flytogether {

namespace {

constexpr int kBitCount = 7;

// Short callouts below 500 ft, indexed like the ApproachDeviation bits.
const char* const kWarningText[kBitCount] = {
    "SPEED HIGH", "SPEED LOW", "SINK RATE", "GEAR UP", "BANK ANGLE", "GLIDESLOPE", "LOCALIZER",
};

bool Crossed(double prev_ft, double now_ft, double gate_ft) {
    return prev_ft > gate_ft && now_ft <= gate_ft;
}

} // namespace

void ApproachCoach::Reset() {
    armed_ = false;
    past_500_ = false;
    has_prev_ = false;
    summary_ = ApproachSummary{};
    for (int i = 0; i < kBitCount; ++i) {
        out_since_[i] = -1.0;
        last_warned_[i] = -1e9;
    }
}

uint32_t ApproachCoach::Deviations(const ApproachSample& s, bool with_gear) const {
    uint32_t d = 0;
    if (s.vref_kt > 0.0) {
        if (s.ias_kt > s.vref_kt + kSpeedHighKt) d |= ApproachDeviation::kSpeedHigh;
        if (s.ias_kt < s.vref_kt - kSpeedLowKt) d |= ApproachDeviation::kSpeedLow;
    }
    if (-s.vs_fpm > kMaxSinkFpm) d |= ApproachDeviation::kSinkRate;
    if (with_gear && s.gear_retractable && s.gear_ratio < 0.99) d |= ApproachDeviation::kGearUp;
    if (std::fabs(s.bank_deg) > kMaxBankDeg) d |= ApproachDeviation::kBank;
    if (s.on_ils) {
        if (std::fabs(s.gs_dots) > kMaxDots) d |= ApproachDeviation::kGlideslope;
        if (std::fabs(s.loc_dots) > kMaxDots) d |= ApproachDeviation::kLocalizer;
    }
    return d;
}

double ApproachCoach::EstimateVrefKt(double vso_kt, double weight_kg, double max_weight_kg) {
    if (!(vso_kt > 20.0 && vso_kt < 200.0)) {
        return 0.0;
    }
    double factor = 1.0;
    if (max_weight_kg > kHeavyKg && weight_kg > 0.0) {
        factor = std::sqrt(std::clamp(weight_kg / max_weight_kg, 0.4, 1.0));
    }
    return 1.3 * vso_kt * factor;
}

std::string ApproachCoach::Describe(uint32_t deviations, const ApproachSample& s) {
    std::string out;
    char buf[64];
    auto add = [&](const char* text) {
        out += (out.empty() ? "" : ", ");
        out += text;
    };
    if (deviations & ApproachDeviation::kSpeedHigh) {
        std::snprintf(buf, sizeof(buf), "speed +%.0f kt", s.ias_kt - s.vref_kt);
        add(buf);
    }
    if (deviations & ApproachDeviation::kSpeedLow) {
        std::snprintf(buf, sizeof(buf), "speed %.0f kt", s.ias_kt - s.vref_kt);
        add(buf);
    }
    if (deviations & ApproachDeviation::kSinkRate) {
        std::snprintf(buf, sizeof(buf), "sink %.0f fpm", -s.vs_fpm);
        add(buf);
    }
    if (deviations & ApproachDeviation::kGearUp) add("gear up");
    if (deviations & ApproachDeviation::kBank) {
        std::snprintf(buf, sizeof(buf), "bank %.0f deg", std::fabs(s.bank_deg));
        add(buf);
    }
    if (deviations & ApproachDeviation::kGlideslope) {
        std::snprintf(buf, sizeof(buf), "glideslope %.1f dots %s", std::fabs(s.gs_dots), s.gs_dots > 0 ? "low" : "high");
        add(buf);
    }
    if (deviations & ApproachDeviation::kLocalizer) {
        std::snprintf(buf, sizeof(buf), "localizer %.1f dots", std::fabs(s.loc_dots));
        add(buf);
    }
    return out;
}

std::optional<ApproachSummary> ApproachCoach::Update(const ApproachSample& s, std::vector<ApproachCallout>& callouts) {
    if (has_prev_ && (s.time_s - prev_.time_s > kMaxGapS || s.time_s < prev_.time_s ||
                      std::fabs(s.agl_ft - prev_.agl_ft) > kJumpFt)) {
        Reset(); // reposition, reload or a long stutter - start over
    }
    if (!has_prev_) {
        prev_ = s;
        has_prev_ = true;
        return std::nullopt;
    }
    const ApproachSample prev = prev_;
    prev_ = s;

    std::optional<ApproachSummary> finished;

    if (s.on_ground) {
        if (past_500_) {
            finished = summary_;
        }
        armed_ = false;
        past_500_ = false;
        return finished;
    }

    if (past_500_) {
        min_agl_since_500_ = std::min(min_agl_since_500_, s.agl_ft);
    }
    if (past_500_ && s.vs_fpm > kGoAroundVsFpm && s.agl_ft - min_agl_since_500_ > kGoAroundClimbFt) {
        summary_.go_around = true;
        finished = summary_;
        past_500_ = false;
    }
    if (s.agl_ft > kArmFt) {
        armed_ = true;
    }
    if (!armed_) {
        return finished;
    }
    const bool descending = s.vs_fpm < -100.0;

    if (!past_500_ && descending && s.on_ils && Crossed(prev.agl_ft, s.agl_ft, kGate1000Ft)) {
        const uint32_t d = Deviations(s, true);
        callouts.push_back(d == 0 ? ApproachCallout{"1000 - STABLE", 3.0, false}
                                  : ApproachCallout{"1000 - UNSTABLE: " + Describe(d, s), 6.0, true});
    }

    if (!past_500_ && descending && Crossed(prev.agl_ft, s.agl_ft, kGate500Ft)) {
        past_500_ = true;
        min_agl_since_500_ = s.agl_ft;
        summary_ = ApproachSummary{};
        summary_.deviations_at_500 = Deviations(s, true);
        summary_.stable_at_500 = summary_.deviations_at_500 == 0;
        callouts.push_back(summary_.stable_at_500
                               ? ApproachCallout{"500 - STABLE", 3.0, false}
                               : ApproachCallout{"500 - UNSTABLE: " + Describe(summary_.deviations_at_500, s) +
                                                     " - GO AROUND",
                                                 7.0, true});
        for (int i = 0; i < kBitCount; ++i) {
            out_since_[i] = -1.0;
            last_warned_[i] = s.time_s; // the gate callout just covered it
        }
        return finished;
    }

    if (past_500_ && s.agl_ft <= kGate500Ft) {
        summary_.max_sink_below_500_fpm = std::max(summary_.max_sink_below_500_fpm, -s.vs_fpm);
        if (s.agl_ft > kWarnFloorFt) {
            // Gear is checked at the gate; a warning would come too late
            // to matter and X-Plane's own horn covers it.
            const uint32_t d = Deviations(s, false);
            for (int i = 0; i < kBitCount; ++i) {
                if (!(d & (1u << i))) {
                    out_since_[i] = -1.0;
                    continue;
                }
                if (out_since_[i] < 0.0) out_since_[i] = s.time_s;
                if (s.time_s - out_since_[i] >= kWarnPersistS && s.time_s - last_warned_[i] >= kWarnRepeatS) {
                    last_warned_[i] = s.time_s;
                    summary_.warnings_below_500 |= 1u << i;
                    callouts.push_back(ApproachCallout{kWarningText[i], 3.0, true});
                }
            }
        }
    }
    return finished;
}

} // namespace flytogether
