// Pure-logic test for shared_cockpit/dataref_learner.h: noise detection in
// the baseline, reporting switches, spring-back buttons and gauges that
// start moving later.

#include "shared_cockpit/dataref_learner.h"

#include <cassert>
#include <cstdio>

using flytogether::DatarefLearner;

int main() {
    DatarefLearner learner;
    // 0 = clock (moves all the time), 1 = beacon switch, 2 = starter button
    // (springs back), 3 = RPM needle (starts moving after engine start),
    // 4 = untouched switch, 5 = float with tiny jitter, 6 = array (COM freq pair)
    learner.Begin(7, 0.0);

    double t = 0.0;
    auto tick = [&](double clock, double beacon, double starter, double rpm, double jitter, double com_standby) {
        learner.Observe(0, {clock}, t);
        learner.Observe(1, {beacon}, t);
        learner.Observe(2, {starter}, t);
        learner.Observe(3, {rpm}, t);
        learner.Observe(4, {0.0}, t);
        learner.Observe(5, {1.0 + jitter}, t);
        learner.Observe(6, {122.8, com_standby}, t);
    };

    // Baseline: only the clock and the float jitter move.
    for (; t < DatarefLearner::kBaselineS; t += 0.25) {
        tick(t, 0, 0, 0, (static_cast<int>(t * 4) % 2) * 1e-7, 118.0);
    }
    assert(!learner.InBaseline(t));
    assert(learner.noisy_count() == 1); // the clock; 1e-7 jitter is below the tolerance
    std::printf("Baseline marks the self-changing clock as noisy, ignores float jitter: OK\n");

    // Watching: beacon on, starter pressed and released, COM standby dialled,
    // then the engine runs and RPM keeps changing.
    tick(t, 1, 0, 0, 0, 118.0);
    t += 0.25;
    tick(t, 1, 1, 0, 0, 118.0);
    t += 0.25;
    tick(t, 1, 0, 0, 0, 119.1);
    for (int i = 0; i < 20; ++i) {
        t += 0.25;
        tick(t, 1, 0, 800.0 + i * 10, 0, 119.1);
    }

    const auto changes = learner.Changes();
    assert(changes.size() == 3);
    assert(changes[0].index == 1 && changes[0].before[0] == 0 && changes[0].after[0] == 1);
    assert(changes[1].index == 2 && changes[1].after[0] == 0 && changes[1].change_count == 2); // sprang back
    assert(changes[2].index == 6 && changes[2].after[1] == 119.1);
    std::printf("Switch, spring-back button and array change reported in order: OK\n");
    std::printf("RPM needle that keeps moving is dropped as noisy: OK\n");

    learner.Stop();
    assert(learner.Changes().size() == 3); // still readable after Stop
    learner.Observe(4, {1.0}, t + 1);
    assert(learner.Changes().size() == 3); // not observing any more

    learner.Begin(7, 100.0);
    assert(learner.Changes().empty());
    std::printf("Stop keeps results, Begin starts fresh: OK\n");

    std::printf("\nALL DATAREF LEARNER CHECKS PASSED\n");
    return 0;
}
