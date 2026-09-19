#pragma once

#include "XPLMDataAccess.h"

#include "flytogether/udp_socket.h"
#include "formation/peer_list.h"
#include "shared_cockpit/dataref_sync_protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flytogether {

// UDP port for generic dataref sync, separate from both Formation mode and
// Shared Cockpit's position/attitude sync.
constexpr uint16_t kDatarefSyncUdpPort = 49021;

// Generic, symmetric (NOT master/client) sync of a configured list of
// "systems" datarefs (switches, radios, autopilot modes, ...) between
// Shared Cockpit peers flying the same aircraft - see docs/plan.md
// section 6. Deliberately not used for flight-control position/attitude,
// which stays strictly master-authoritative via
// shared_cockpit/shared_cockpit_sync.h + XPLM's override_planepath:
// either side can change a watched dataref here and it mirrors to the
// other peer(s), modeling a copilot operating systems while the pilot
// flies. No per-element ownership/request-release arbitration yet
// (docs/plan.md section 6's "Besitzer pro Element") - whichever side
// wrote last simply wins if both change the same dataref near-simultaneously.
//
// Works for any aircraft without per-aircraft code: since both peers fly
// the identical aircraft, watched dataref names resolve to the same thing
// on both sides. The only per-aircraft "profile" needed is which dataref
// names to watch (see shared_cockpit/shared_cockpit_config.h's DATAREF
// lines) - not a custom mapping/getter-setter per dataref.
class DatarefSync {
public:
    bool Start(const std::vector<std::string>& watchedNames, const std::vector<Peer>& peers);
    void Stop();

    // Call every frame or so: reads each watched dataref, broadcasts any
    // that changed locally to every peer, and applies (with echo
    // prevention) any change a peer sent for a dataref we're watching.
    void Poll();

    // Feeds a message that arrived via the rendezvous server's relay
    // rather than this class's own direct-UDP socket - see
    // SharedCockpitSync::IngestRelayedPacket's comment for why relay is
    // needed as a NAT-traversal fallback for Shared Cockpit over the
    // internet. Applies exactly like a directly-received change would.
    void IngestRelayedMessage(const void* data, size_t len);

    // Also hand every locally-detected change to this callback (set once
    // by plugin_main.cpp to relay through the rendezvous server), in
    // parallel with the direct peer sends in Poll() - same policy as
    // SharedCockpitSync::SetRelaySender.
    void SetRelaySender(std::function<void(const void*, size_t)> sender) {
        relay_sender_ = std::move(sender);
    }

    size_t watched_count() const { return watched_.size(); }

private:
    struct WatchedDataref {
        std::string name;
        XPLMDataRef ref = nullptr;
        XPLMDataTypeID xplm_type = 0;
        DatarefValue last_known;
        bool has_last_known = false;
    };

    DatarefValue ReadCurrentValue(const WatchedDataref& w) const;
    void ApplyValue(WatchedDataref& w, const DatarefValue& value);
    void ApplyIncomingBytes(const void* data, size_t len);

    UdpSocket socket_;
    std::vector<Peer> peers_;
    std::vector<WatchedDataref> watched_;
    std::unordered_map<std::string, size_t> index_by_name_;
    std::function<void(const void*, size_t)> relay_sender_;
};

} // namespace flytogether
