#pragma once

#include "shared_cockpit/command_sync_protocol.h"
#include "shared_cockpit/shared_cockpit_config.h"

#include "XPLMUtilities.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace flytogether {

// Mirrors button presses between both Shared Cockpit sides - the COMMAND
// lines of the aircraft profile. DatarefSync keeps *values* in sync; many
// things in X-Plane aren't a value you can write but a command you press:
// autopilot modes (HDG/NAV/ALT), G1000 softkeys, FMS keys, transponder
// IDENT. A command handler on every profile command sees the local press
// (begin/end, so held buttons stay held) and sends it; the other side
// replays it with XPLMCommandBegin/End.
//
// Only commands listed in this side's own profile are ever executed on
// behalf of the peer - it can't make us press anything else (e.g.
// sim/operation/quit). A remote press whose release never arrives is ended
// after kMaxRemoteHoldS, so a lost packet can't leave a starter cranking.
class CommandSync {
public:
    using SendFn = std::function<void(const std::vector<uint8_t>& plain)>;
    using LocalPressFn = std::function<void(DatarefCategory)>;

    static constexpr double kMaxRemoteHoldS = 10.0;

    void Start(const std::vector<CommandSyncSpec>& specs, uint32_t sender_id, SendFn send, LocalPressFn on_local_press);
    void Stop();

    // A decoded-and-decrypted message from the peer.
    void Ingest(const uint8_t* data, size_t len, double now_s);
    // Call regularly: ends remote presses that have been held too long.
    void Poll(double now_s);

    size_t command_count() const { return entries_.size(); }

private:
    struct Entry {
        CommandSync* owner = nullptr;
        XPLMCommandRef ref = nullptr;
        CommandSyncSpec spec;
    };

    static int Handler(XPLMCommandRef cmd, XPLMCommandPhase phase, void* refcon);
    void SendPhase(const Entry& entry, CommandPhase phase);

    std::vector<std::unique_ptr<Entry>> entries_;
    std::unordered_map<std::string, Entry*> by_name_;
    std::unordered_map<std::string, double> remote_held_; // name -> begin time
    SendFn send_;
    LocalPressFn on_local_press_;
    uint32_t sender_id_ = 0;
    uint32_t next_sequence_ = 1;
    bool replaying_ = false;
    CommandDedup dedup_;
};

} // namespace flytogether
