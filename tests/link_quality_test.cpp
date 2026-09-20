// Pure-math regression test for LinkQualityTracker. No XPLM SDK or
// networking involved - build and run with:
//   g++ -std=c++17 -I plugin/include -I netcore/include \
//       tests/link_quality_test.cpp plugin/src/net/link_quality.cpp \
//       -o /tmp/link_quality_test && /tmp/link_quality_test
// or via CTest (see top-level CMakeLists.txt).

#include "net/link_quality.h"

#include <cassert>
#include <cstdio>

using flytogether::LinkQualityTracker;

namespace {

void TestNoDataBeforeFirstPacket() {
    LinkQualityTracker tracker;
    assert(!tracker.HasData());
    tracker.OnPacketReceived(1);
    // One sample gives a baseline sequence to measure the next gap
    // against, even though LossRatio() has no delta to report yet
    // (it correctly still reads 0.0 - see TestBackToBackPacketsMeanZeroLoss
    // for the "no loss" case that shares that same starting value).
    assert(tracker.HasData());
    std::printf("TestNoDataBeforeFirstPacket: OK\n");
}

void TestBackToBackPacketsMeanZeroLoss() {
    LinkQualityTracker tracker;
    for (uint32_t seq = 1; seq <= 20; ++seq) {
        tracker.OnPacketReceived(seq);
    }
    assert(tracker.HasData());
    assert(tracker.LossRatio() < 1e-6);
    std::printf("TestBackToBackPacketsMeanZeroLoss: OK\n");
}

void TestGapsIncreaseLossRatio() {
    LinkQualityTracker tracker;
    tracker.OnPacketReceived(1);
    // Every other packet lost, repeatedly - should converge toward ~50%.
    for (uint32_t seq = 3; seq <= 101; seq += 2) {
        tracker.OnPacketReceived(seq);
    }
    assert(tracker.LossRatio() > 0.4 && tracker.LossRatio() < 0.6);
    std::printf("TestGapsIncreaseLossRatio: OK\n");
}

void TestRecoversAfterOutageEnds() {
    LinkQualityTracker tracker;
    tracker.OnPacketReceived(1);
    // A single large gap (e.g. a brief outage) nudges the EMA up from zero
    // by one smoothing step - it doesn't jump straight to ~100%, by
    // design (see kEmaAlpha's comment): a single stray event shouldn't
    // read as a total, ongoing outage.
    tracker.OnPacketReceived(50);
    assert(tracker.LossRatio() > 0.1);
    for (uint32_t seq = 51; seq <= 150; ++seq) {
        tracker.OnPacketReceived(seq);
    }
    // Should have decayed back down close to zero, not stayed pinned high.
    assert(tracker.LossRatio() < 0.01);
    std::printf("TestRecoversAfterOutageEnds: OK\n");
}

void TestDuplicateSequenceIsNotCountedAsLoss() {
    LinkQualityTracker tracker;
    tracker.OnPacketReceived(1);
    tracker.OnPacketReceived(2);
    tracker.OnPacketReceived(2); // duplicate/retransmit
    tracker.OnPacketReceived(3);
    assert(tracker.LossRatio() < 1e-6);
    std::printf("TestDuplicateSequenceIsNotCountedAsLoss: OK\n");
}

void TestImplausibleWraparoundDeltaIsClamped() {
    LinkQualityTracker tracker;
    tracker.OnPacketReceived(4000000000u);
    // A sender restart resetting its sequence counter near 0 looks like a
    // huge forward delta from here (unsigned wraparound) - must not read
    // as ~100% loss forever afterward.
    tracker.OnPacketReceived(1);
    assert(tracker.LossRatio() <= 1.0 + 1e-9);
    for (uint32_t seq = 2; seq <= 100; ++seq) {
        tracker.OnPacketReceived(seq);
    }
    assert(tracker.LossRatio() < 0.05);
    std::printf("TestImplausibleWraparoundDeltaIsClamped: OK\n");
}

} // namespace

int main() {
    TestNoDataBeforeFirstPacket();
    TestBackToBackPacketsMeanZeroLoss();
    TestGapsIncreaseLossRatio();
    TestRecoversAfterOutageEnds();
    TestDuplicateSequenceIsNotCountedAsLoss();
    TestImplausibleWraparoundDeltaIsClamped();
    std::printf("All link_quality tests passed.\n");
    return 0;
}
