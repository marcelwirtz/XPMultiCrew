// Pure-logic test for sync/approach_coach.h - a stable approach, an
// unstable one (fast, steep, gear up), warnings below 500 ft, the ILS-only
// 1000 ft gate, a go-around, and a takeoff not triggering anything.

#include "sync/approach_coach.h"

#include "approach_coach_recorded.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace flytogether;

namespace {

constexpr double kDt = 1.0 / 30.0;

struct Sim {
    ApproachCoach coach;
    ApproachSample s;
    std::vector<ApproachCallout> callouts;
    std::vector<ApproachSummary> summaries;

    Sim() {
        s.vref_kt = 60.0;
        s.ias_kt = 70.0;
        s.gear_retractable = true;
        s.gear_ratio = 1.0;
    }

    void Step() {
        s.time_s += kDt;
        s.agl_ft += s.vs_fpm / 60.0 * kDt;
        if (s.agl_ft <= 0.0) {
            s.agl_ft = 0.0;
            s.on_ground = true;
            s.vs_fpm = 0.0;
        } else {
            s.on_ground = false;
        }
        if (auto r = coach.Update(s, callouts)) summaries.push_back(*r);
    }

    // Fly at the current settings until reaching `agl_ft` (descending or climbing).
    void FlyTo(double agl_ft) {
        const bool down = s.vs_fpm < 0;
        for (int i = 0; i < 100000; ++i) {
            if (down ? s.agl_ft <= agl_ft : s.agl_ft >= agl_ft) return;
            if (s.on_ground) return;
            Step();
        }
    }

    void Hold(double seconds) {
        for (double t = 0; t < seconds; t += kDt) Step();
    }

    bool Said(const std::string& part) const {
        for (const auto& c : callouts) {
            if (c.text.find(part) != std::string::npos) return true;
        }
        return false;
    }
};

void StartHigh(Sim& sim) {
    sim.s.agl_ft = 2000.0;
    sim.s.vs_fpm = 0.0;
    sim.Hold(1.0); // armed
}

void TestStableApproach() {
    Sim sim;
    StartHigh(sim);
    sim.s.vs_fpm = -600.0;
    sim.FlyTo(0.0);
    assert(!sim.Said("1000")); // no ILS - no 1000 ft gate
    assert(sim.Said("500 - STABLE"));
    assert(!sim.Said("GO AROUND"));
    assert(sim.summaries.size() == 1);
    assert(sim.summaries[0].stable_at_500);
    assert(sim.summaries[0].warnings_below_500 == 0);
    assert(!sim.summaries[0].go_around);
    std::printf("stable approach: OK\n");
}

void TestUnstableApproach() {
    Sim sim;
    StartHigh(sim);
    sim.s.vs_fpm = -1400.0;
    sim.s.ias_kt = 90.0;
    sim.s.gear_ratio = 0.0;
    sim.FlyTo(0.0);
    assert(sim.Said("500 - UNSTABLE"));
    assert(sim.Said("speed +30 kt"));
    assert(sim.Said("sink 1400 fpm"));
    assert(sim.Said("gear up"));
    assert(sim.Said("GO AROUND"));
    assert(sim.Said("SINK RATE")); // still too steep below 500 ft
    assert(sim.summaries.size() == 1);
    const auto& r = sim.summaries[0];
    assert(!r.stable_at_500);
    assert(r.deviations_at_500 & ApproachDeviation::kGearUp);
    assert(r.warnings_below_500 & ApproachDeviation::kSinkRate);
    assert(!(r.warnings_below_500 & ApproachDeviation::kGearUp)); // gear only at the gate
    assert(r.max_sink_below_500_fpm >= 1399.0);
    std::printf("unstable approach: OK\n");
}

void TestWarningNeedsToPersist() {
    Sim sim;
    StartHigh(sim);
    sim.s.vs_fpm = -600.0;
    sim.FlyTo(400.0);
    sim.s.bank_deg = 35.0;
    sim.Hold(1.0); // shorter than kWarnPersistS
    sim.s.bank_deg = 0.0;
    sim.Hold(1.0);
    assert(!sim.Said("BANK ANGLE"));
    sim.s.bank_deg = 35.0;
    sim.Hold(2.0);
    assert(sim.Said("BANK ANGLE"));
    std::printf("warning needs to persist: OK\n");
}

void TestIlsGate1000() {
    Sim sim;
    StartHigh(sim);
    sim.s.on_ils = true;
    sim.s.gs_dots = 1.5;
    sim.s.vs_fpm = -700.0;
    sim.FlyTo(900.0);
    assert(sim.Said("1000 - UNSTABLE: glideslope 1.5 dots low"));
    sim.s.gs_dots = 0.2;
    sim.FlyTo(0.0);
    assert(sim.Said("500 - STABLE"));
    std::printf("ILS 1000 ft gate: OK\n");
}

void TestGoAround() {
    Sim sim;
    StartHigh(sim);
    sim.s.vs_fpm = -600.0;
    sim.FlyTo(300.0);
    sim.s.vs_fpm = 800.0;
    sim.FlyTo(1100.0);
    assert(sim.summaries.size() == 1);
    assert(sim.summaries[0].go_around);
    // The second approach is rated again.
    sim.s.vs_fpm = -600.0;
    sim.FlyTo(0.0);
    assert(sim.summaries.size() == 2);
    assert(!sim.summaries[1].go_around);
    std::printf("go-around: OK\n");
}

void TestTakeoffAndLowFlightSilent() {
    Sim sim;
    sim.s.agl_ft = 0.0;
    sim.s.on_ground = true;
    sim.Hold(1.0);
    sim.s.vs_fpm = 800.0;
    sim.s.agl_ft = 1.0;
    sim.FlyTo(600.0); // never above kArmFt
    sim.s.vs_fpm = -500.0;
    sim.FlyTo(0.0);
    assert(sim.callouts.empty());
    assert(sim.summaries.empty());
    std::printf("takeoff / low circuit below arm height silent: OK\n");
}

void TestRepositionResets() {
    Sim sim;
    StartHigh(sim);
    sim.s.vs_fpm = -600.0;
    sim.FlyTo(600.0);
    sim.s.agl_ft = 3000.0; // placed elsewhere
    sim.Step();
    sim.s.agl_ft = 300.0; // and back low - no gate crossing, not armed
    sim.Step();
    sim.FlyTo(0.0);
    assert(!sim.Said("500"));
    assert(sim.summaries.empty());
    std::printf("reposition resets: OK\n");
}

void TestVrefEstimate() {
    // C172 (max 1160 kg): fixed POH speed, whatever it weighs.
    assert(std::fabs(ApproachCoach::EstimateVrefKt(40.0, 1000.0, 1160.0) - 52.0) < 0.01);
    // Heavy: scaled with sqrt(weight / max) - A330 at 180 of 242 t.
    const double a330 = ApproachCoach::EstimateVrefKt(125.0, 180000.0, 242000.0);
    assert(a330 > 138.0 && a330 < 142.0);
    assert(std::fabs(ApproachCoach::EstimateVrefKt(125.0, 0.0, 242000.0) - 162.5) < 0.01); // weight unknown
    // Unset/implausible Vso: no speed checks.
    assert(ApproachCoach::EstimateVrefKt(0.0, 1000.0, 1160.0) == 0.0);
    assert(ApproachCoach::EstimateVrefKt(500.0, 1000.0, 1160.0) == 0.0);
    std::printf("Vref estimate: OK\n");
}

// Feeds a recorded approach (approach_coach_recorded.h) through the coach
// with the C172's Vref (1.3 x Vso 40 kt).
template <size_t N>
std::vector<ApproachSummary> ReplayRecordedAll(const double (&rows)[N][6], std::vector<std::string>& texts) {
    ApproachCoach coach;
    std::vector<ApproachSummary> results;
    for (const auto& r : rows) {
        ApproachSample s;
        s.time_s = 1000.0 + r[0];
        s.on_ground = r[1] != 0.0;
        s.agl_ft = r[2];
        s.vs_fpm = r[3];
        s.ias_kt = r[4];
        s.bank_deg = r[5];
        s.vref_kt = 52.0;
        std::vector<ApproachCallout> callouts;
        if (const auto summary = coach.Update(s, callouts)) results.push_back(*summary);
        for (const auto& c : callouts) texts.push_back(c.text);
    }
    return results;
}

template <size_t N>
ApproachSummary ReplayRecorded(const double (&rows)[N][6], std::vector<std::string>& texts) {
    const auto results = ReplayRecordedAll(rows, texts);
    assert(results.size() == 1);
    return results.back();
}

void TestRecordedApproaches() {
    std::vector<std::string> texts;
    const ApproachSummary stable = ReplayRecorded(kRecordedStableC172, texts);
    // A normal 70 kt C172 approach must not nag at all.
    assert(stable.stable_at_500 && stable.warnings_below_500 == 0 && !stable.go_around);
    assert(texts.size() == 1);

    texts.clear();
    const ApproachSummary steep = ReplayRecorded(kRecordedSteepC172, texts);
    assert(steep.stable_at_500);
    assert(steep.warnings_below_500 & ApproachDeviation::kSinkRate);
    assert(steep.max_sink_below_500_fpm > 1000.0);

    // The low pass only climbs back to the circuit (~900 ft AGL): it must
    // still end as a go-around, and the landing after it gets its own,
    // clean rating instead of the dive's.
    texts.clear();
    const auto both = ReplayRecordedAll(kRecordedLowPassThenLandC172, texts);
    assert(both.size() == 2);
    assert(both[0].go_around && !both[0].stable_at_500);
    assert(both[0].deviations_at_500 & ApproachDeviation::kSinkRate);
    assert(both[0].max_sink_below_500_fpm > 1000.0);
    assert(!both[1].go_around && both[1].stable_at_500 && both[1].warnings_below_500 == 0);
    assert(both[1].max_sink_below_500_fpm < 1000.0);
    std::printf("recorded C172 approaches: OK\n");
}

} // namespace

int main() {
    TestVrefEstimate();
    TestStableApproach();
    TestUnstableApproach();
    TestWarningNeedsToPersist();
    TestIlsGate1000();
    TestGoAround();
    TestTakeoffAndLowFlightSilent();
    TestRepositionResets();
    TestRecordedApproaches();
    std::printf("All approach coach tests passed.\n");
    return 0;
}
