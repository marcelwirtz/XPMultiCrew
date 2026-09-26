#pragma once

#include <cstddef>
#include <vector>

namespace flytogether {

// "Learn from the cockpit" for the companion's profile editor: watches a
// set of candidate datarefs and reports which ones the user changed by
// flipping switches, so nobody has to know dataref names by heart.
//
// Two phases after Begin():
//   1. Baseline (hands off, kBaselineS): anything that changes on its own
//      here - clocks, gauges, physics - is marked noisy and never reported.
//   2. Watching: a candidate whose value differs from its baseline, or
//      changed at least once (a push button that springs back), is
//      reported. One that keeps changing (more than kMaxChanges times, e.g.
//      an RPM needle once the engine runs) is reclassified as noisy.
//
// Pure logic, no XPLM - plugin_main.cpp does the dataref reads and feeds
// Observe(); see tests/dataref_learner_test.cpp. Values are doubles so int,
// float and double datarefs (and array elements) share one path.
class DatarefLearner {
public:
    static constexpr double kBaselineS = 3.0;
    static constexpr int kMaxChanges = 8;

    struct Change {
        size_t index = 0;           // candidate index passed to Observe()
        std::vector<double> before; // value at the end of the baseline
        std::vector<double> after;  // latest value
        int change_count = 0;
    };

    void Begin(size_t candidate_count, double now_s);
    void Stop() { active_ = false; }

    bool active() const { return active_; }
    bool InBaseline(double now_s) const { return active_ && now_s - start_s_ < kBaselineS; }

    // Latest value of candidate `index` at `now_s`. Cheap to call for every
    // candidate at a few Hz.
    void Observe(size_t index, const std::vector<double>& values, double now_s);

    // Reported changes in the order they were first seen, noisy ones
    // excluded. Stays available after Stop() until the next Begin().
    std::vector<Change> Changes() const;

    size_t noisy_count() const;

private:
    struct Candidate {
        bool has_value = false;
        bool noisy = false;
        std::vector<double> baseline;
        std::vector<double> last;
        int change_count = 0;
        size_t first_change_order = 0; // 0 = never changed after the baseline
    };

    static bool Differs(const std::vector<double>& a, const std::vector<double>& b);

    bool active_ = false;
    double start_s_ = 0.0;
    size_t next_order_ = 1;
    std::vector<Candidate> candidates_;
};

} // namespace flytogether
