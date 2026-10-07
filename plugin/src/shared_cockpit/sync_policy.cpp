#include "shared_cockpit/sync_policy.h"

namespace flytogether {

LocalAction OnLocalValue(WatchedSyncState& state, const DatarefValue& current, bool is_master, bool refresh,
                         double now_s) {
    if (is_master) {
        const bool changed = !state.has_last_known || current != state.last_known;
        MarkApplied(state, current);
        return changed || state.stream || refresh ? LocalAction::kSend : LocalAction::kNone;
    }

    if (state.output) {
        // Never sent; the master's value wins whenever the local simulation
        // moved away from it.
        if (state.has_master_value && current != state.master_value) {
            MarkApplied(state, state.master_value);
            return LocalAction::kWriteMaster;
        }
        MarkApplied(state, current);
        return LocalAction::kNone;
    }

    if (!state.has_last_known) {
        MarkApplied(state, current); // baseline, nothing moved yet
        return LocalAction::kNone;
    }
    if (current == state.last_known) {
        return LocalAction::kNone;
    }
    MarkApplied(state, current);
    if (state.has_master_value && current == state.master_value) {
        return LocalAction::kNone; // caught up with the master by itself
    }
    state.hold_local_until = now_s + kLocalChangeHoldS;
    return LocalAction::kSend;
}

bool OnRemoteValue(WatchedSyncState& state, const DatarefValue& value, bool is_master, double now_s) {
    if (is_master) {
        // The co-pilot's switch: applied, unless it's one of our
        // simulation's results (a co-pilot on an older profile).
        return !state.output;
    }
    state.master_value = value;
    state.has_master_value = true;
    if (now_s < state.hold_local_until) {
        return false; // our own change is still on its way - see kLocalChangeHoldS
    }
    return !state.has_last_known || value != state.last_known;
}

} // namespace flytogether
