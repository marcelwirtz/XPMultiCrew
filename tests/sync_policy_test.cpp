// Shared Cockpit dataref sync decisions (shared_cockpit/sync_policy.h),
// including the two-cockpit scenarios that broke in the claim-on-touch
// design: a value changing by itself on the co-pilot's side, and the
// spring-loaded starter key.

#include "shared_cockpit/sync_policy.h"

#include <cassert>
#include <cstdio>

using namespace flytogether;

namespace {

DatarefValue Int(int v) {
    DatarefValue value;
    value.type = DatarefValueType::kInt;
    value.int_value = v;
    return value;
}

void TestMasterSendsChangesAndRefreshes() {
    WatchedSyncState s;
    assert(OnLocalValue(s, Int(0), true, false, 0.0) == LocalAction::kSend); // first read: full state
    assert(OnLocalValue(s, Int(0), true, false, 0.1) == LocalAction::kNone);
    assert(OnLocalValue(s, Int(1), true, false, 0.2) == LocalAction::kSend);
    assert(OnLocalValue(s, Int(1), true, true, 0.3) == LocalAction::kSend); // periodic refresh
    WatchedSyncState stream;
    stream.stream = true;
    OnLocalValue(stream, Int(4), true, false, 0.0);
    assert(OnLocalValue(stream, Int(4), true, false, 0.1) == LocalAction::kSend);
    std::printf("TestMasterSendsChangesAndRefreshes: OK\n");
}

void TestMasterAppliesCopilotSwitchesButNotOutputs() {
    WatchedSyncState s;
    OnLocalValue(s, Int(0), true, false, 0.0);
    assert(OnRemoteValue(s, Int(1), true, 0.1));
    MarkApplied(s, Int(1));
    assert(OnLocalValue(s, Int(1), true, false, 0.2) == LocalAction::kNone); // not echoed back
    WatchedSyncState out;
    out.output = true;
    assert(!OnRemoteValue(out, Int(0), true, 0.0)); // ENGN_running from the co-pilot: never
    std::printf("TestMasterAppliesCopilotSwitchesButNotOutputs: OK\n");
}

void TestCopilotNeverSendsByItselfAndNeverClaims() {
    WatchedSyncState s;
    // Baseline: the co-pilot's cold-start value isn't pushed at the master.
    assert(OnLocalValue(s, Int(0), false, true, 0.0) == LocalAction::kNone);
    // The master's value arrives and is applied.
    assert(OnRemoteValue(s, Int(3), false, 1.0));
    MarkApplied(s, Int(3));
    assert(OnLocalValue(s, Int(3), false, false, 1.1) == LocalAction::kNone);
    // Repeating the same value (refresh) doesn't write again.
    assert(!OnRemoteValue(s, Int(3), false, 2.0));
    std::printf("TestCopilotNeverSendsByItselfAndNeverClaims: OK\n");
}

void TestCopilotSwitchIsSentAndNotFlickedBack() {
    WatchedSyncState s;
    OnLocalValue(s, Int(0), false, false, 0.0);
    assert(OnRemoteValue(s, Int(0), false, 0.0) == false); // equal to baseline
    // The co-pilot flips the switch.
    assert(OnLocalValue(s, Int(1), false, false, 5.0) == LocalAction::kSend);
    // The master's older value (sent before our request arrived) is held off.
    assert(!OnRemoteValue(s, Int(0), false, 5.2));
    // Later the master's state wins again.
    assert(OnRemoteValue(s, Int(0), false, 5.0 + kLocalChangeHoldS + 0.1));
    std::printf("TestCopilotSwitchIsSentAndNotFlickedBack: OK\n");
}

void TestCopilotOutputFollowsMaster() {
    WatchedSyncState s;
    s.output = true;
    OnLocalValue(s, Int(0), false, false, 0.0);
    assert(OnRemoteValue(s, Int(1), false, 1.0)); // master's engine runs
    MarkApplied(s, Int(1));
    // The co-pilot's own simulation stops it again: written back, never sent.
    assert(OnLocalValue(s, Int(0), false, false, 1.1) == LocalAction::kWriteMaster);
    assert(s.last_known == Int(1));
    assert(OnLocalValue(s, Int(1), false, false, 1.2) == LocalAction::kNone);
    std::printf("TestCopilotOutputFollowsMaster: OK\n");
}

void TestCopilotCatchingUpByItselfIsNotARequest() {
    WatchedSyncState s;
    OnLocalValue(s, Int(0), false, false, 0.0);
    // The master's value arrives while ours is still held...
    s.hold_local_until = 2.0;
    assert(!OnRemoteValue(s, Int(2), false, 1.0));
    // ...and our own simulation reaches the same value: nothing to request.
    assert(OnLocalValue(s, Int(2), false, false, 1.5) == LocalAction::kNone);
    std::printf("TestCopilotCatchingUpByItselfIsNotARequest: OK\n");
}

} // namespace

int main() {
    TestMasterSendsChangesAndRefreshes();
    TestMasterAppliesCopilotSwitchesButNotOutputs();
    TestCopilotNeverSendsByItselfAndNeverClaims();
    TestCopilotSwitchIsSentAndNotFlickedBack();
    TestCopilotOutputFollowsMaster();
    TestCopilotCatchingUpByItselfIsNotARequest();
    std::printf("\nALL SYNC POLICY CHECKS PASSED\n");
    return 0;
}
