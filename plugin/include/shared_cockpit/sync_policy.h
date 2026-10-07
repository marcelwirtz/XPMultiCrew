#pragma once

#include "shared_cockpit/dataref_sync_protocol.h"

namespace flytogether {

// Per-dataref decisions of DatarefSync, kept free of XPLM and sockets so
// they can be tested (tests/sync_policy_test.cpp).
//
// The pilot flying (MASTER) is the only authority for the cockpit's state:
// its simulator is the one actually flying. It sends every change of a
// watched dataref - its own pilot's or one its simulator made - and
// periodically everything again. The co-pilot (CLIENT) applies what it
// gets, and a switch its own pilot moves is sent to the master as a
// request; the master applies it and from then on it's simply master
// state. Nothing is ever "claimed", so neither side can lock the other out
// or make both cockpits overwrite each other in a loop - which is what the
// earlier claim-on-touch design did as soon as a value changed by itself
// (an engine starting, a spring-loaded key) on the co-pilot's side.
//
// OUTPUT datarefs (ENGN_running, ...) are results of the master's
// simulation, not controls: the co-pilot never sends them and keeps
// re-applying the master's value, since its own (overridden) simulation
// would otherwise drift.
struct WatchedSyncState {
    bool output = false;
    bool stream = false; // master re-sends every tick (rarely needed)

    DatarefValue last_known; // what was last read/applied here
    bool has_last_known = false;
    DatarefValue master_value; // co-pilot: the master's latest value
    bool has_master_value = false;
    double hold_local_until = 0.0; // co-pilot: own change in flight to the master
};

// After the co-pilot moves a switch, the master's older value can still be
// on its way; it's not applied for this long so the switch doesn't flick
// back and forth.
constexpr double kLocalChangeHoldS = 1.5;

enum class LocalAction {
    kNone,
    kSend,        // send `current` to the peer
    kWriteMaster, // co-pilot: write state.master_value back into the sim
};

// One read of the dataref's current value. `refresh` (master only): send
// it even if unchanged.
LocalAction OnLocalValue(WatchedSyncState& state, const DatarefValue& current, bool is_master, bool refresh,
                         double now_s);

// A value from the peer. True: write it into the sim (the caller then also
// stores it as last_known via MarkApplied).
bool OnRemoteValue(WatchedSyncState& state, const DatarefValue& value, bool is_master, double now_s);

inline void MarkApplied(WatchedSyncState& state, const DatarefValue& value) {
    state.last_known = value;
    state.has_last_known = true;
}

} // namespace flytogether
