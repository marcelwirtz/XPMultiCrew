#pragma once

#include "XPLMDataAccess.h"

#include "flytogether/udp_socket.h"
#include "formation/peer_list.h"
#include "net/session_crypto.h"
#include "shared_cockpit/dataref_sync_protocol.h"
#include "shared_cockpit/ownership_tracker.h"
#include "shared_cockpit/shared_cockpit_config.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flytogether {

// UDP port for generic dataref sync, separate from both Formation mode and
// Shared Cockpit's position/attitude sync.
constexpr uint16_t kDatarefSyncUdpPort = 49021;

// Generic sync of a configured list of "systems" datarefs (switches,
// radios, autopilot modes, ...) between Shared Cockpit peers flying the
// same aircraft - see docs/plan.md section 6. Deliberately not used for
// flight-control position/attitude, which stays strictly master-
// authoritative via shared_cockpit/shared_cockpit_sync.h + XPLM's
// override_planepath.
//
// Every watched dataref belongs to one of a small fixed set of
// DatarefCategory buckets (see shared_cockpit_config.h), and real,
// enforced ownership is tracked per category via OwnershipTracker: only
// the side that currently owns a category can have its local changes to
// that category's datarefs actually take effect and propagate - a local
// change to a dataref in a category this side doesn't own is reverted
// back to the last-known-good value instead of being broadcast (one
// deterministic snap-back, not a continuous fight against the network),
// and an incoming remote write for a category this side *does* own is
// dropped rather than applied, protecting that ownership in both
// directions. See ownership_tracker.h's class comment for why this is a
// unilateral claim/notify design rather than a request/grant handshake.
//
// Works for any aircraft without per-aircraft code: since both peers fly
// the identical aircraft, watched dataref names resolve to the same thing
// on both sides. The only per-aircraft "profile" needed is which dataref
// names (and categories) to watch (see shared_cockpit/shared_cockpit_config.h's
// DATAREF lines) - not a custom mapping/getter-setter per dataref.
class DatarefSync {
public:
    // `seedFromCurrentValues`: if true, every watched dataref's *current*
    // value is cached as already-known up front, so the first Poll()
    // afterwards only broadcasts genuine future changes instead of
    // treating every dataref as "just changed" - see Poll()'s
    // `!w.has_last_known` check. Without this, a freshly-joined side
    // would otherwise immediately broadcast its own stale/cold-start
    // values back at the peer it just joined, alongside (and racing) the
    // full state that side is itself broadcasting for the same reason -
    // whichever message arrived last would silently win, sometimes
    // clobbering a carefully-configured cockpit with the other side's
    // defaults. Pass true for the joining CLIENT (so only the MASTER's
    // already-configured state - which never sets this - reaches the
    // client deterministically); leave false for the MASTER, so its own
    // current state (fully "unknown" from this instance's point of view
    // right after Start()) *is* broadcast in full on the very next
    // Poll() - this is what gives a newly-connected client the master's
    // complete state, not just future deltas from that point on.
    //
    // `startsOwningAllCategories`: see OwnershipTracker's constructor -
    // pass true for MASTER, false for CLIENT. Independent of
    // `seedFromCurrentValues` (which is about echoing cold-start values
    // back at a just-joined peer, not about who's allowed to change
    // what), though both happen to be role-derived the same way at every
    // current call site.
    bool Start(const std::vector<DatarefSyncSpec>& watched, const std::vector<Peer>& peers,
               bool seedFromCurrentValues = false, bool startsOwningAllCategories = true);
    void Stop();

    // Call every frame or so: reads each watched dataref, broadcasts any
    // that changed locally to every peer (or, for a category this side
    // doesn't own, reverts the unauthorized local change instead - see
    // this class's comment), and applies (with echo prevention and
    // ownership gating) any change a peer sent for a dataref we're
    // watching. `now_s` should be XPLMGetElapsedTime() - needed for the
    // ownership request/grant/deny handshake's timeout (see
    // ownership_tracker.h).
    void Poll(double now_s);

    // Feeds a message that arrived via the rendezvous server's relay
    // rather than this class's own direct-UDP socket - see
    // SharedCockpitSync::IngestRelayedPacket's comment for why relay is
    // needed as a NAT-traversal fallback for Shared Cockpit over the
    // internet. Applies exactly like a directly-received change would.
    void IngestRelayedMessage(const void* data, size_t len, double now_s);

    // Also hand every locally-detected change to this callback (set once
    // by plugin_main.cpp to relay through the rendezvous server), in
    // parallel with the direct peer sends in Poll() - same policy as
    // SharedCockpitSync::SetRelaySender.
    void SetRelaySender(std::function<void(const void*, size_t)> sender) {
        relay_sender_ = std::move(sender);
    }

    size_t watched_count() const { return watched_.size(); }

    // Local UI action ("I'd like this category") - see
    // OwnershipTracker::RequestCategory. Not an immediate claim any more:
    // sends a request to the peer (+ relay) a few times in a row (best-
    // effort UDP, no ACK below this) and waits for their Grant/Deny -
    // see ownership_tracker.h's class comment for the full handshake and
    // its timeout. `now_s` should be XPLMGetElapsedTime(), same clock as
    // Poll().
    void RequestOwnership(DatarefCategory category, double now_s);

    // Local UI action: answer a pending incoming request (see
    // HasIncomingOwnershipRequest below) for `category` with Grant
    // (`grant=true`) or Deny (`grant=false`). No-op if there's no live
    // incoming request for it any more (already answered, or the
    // requester's own timeout already passed).
    void RespondOwnership(DatarefCategory category, bool grant, double now_s);

    bool Owns(DatarefCategory category) const { return ownership_.Owns(category); }

    // Exposed for the companion app's UI (via control_listener.h's
    // SHARED_COCKPIT_OWNERSHIP line) - see OwnershipTracker's own
    // comments for exactly what "pending"/"incoming" mean.
    bool IsOwnershipRequestPending(DatarefCategory category, double now_s) const {
        return ownership_.IsRequestPending(category, now_s);
    }
    bool HasIncomingOwnershipRequest(DatarefCategory category, double now_s) const {
        return ownership_.HasIncomingRequest(category, now_s);
    }

    // Encrypts every outgoing message (dataref sync + ownership
    // request/response, direct-UDP and relay alike, via the shared
    // SendToPeers choke point) from here on - see net/session_crypto.h.
    // On the RECEIVE side, only direct-UDP (Poll()) decrypts here; a
    // relayed message arrives via IngestRelayedMessage already decrypted
    // by plugin_main.cpp's relay dispatcher - see that method's comment
    // for why (the relay channel multiplexes several message shapes onto
    // one stream, and the dispatcher has to decrypt before it can even
    // peek the magic byte to route between them).
    void SetCrypto(const SessionCrypto* crypto) { crypto_ = crypto; }

private:
    struct WatchedDataref {
        std::string name;
        XPLMDataRef ref = nullptr;
        XPLMDataTypeID xplm_type = 0;
        DatarefValue last_known;
        bool has_last_known = false;
        // See DatarefSyncSpec::stream - true bypasses change-detection in
        // Poll() and broadcasts every tick.
        bool stream = false;
        DatarefCategory category = DatarefCategory::kSystems;
    };

    DatarefValue ReadCurrentValue(const WatchedDataref& w) const;
    void ApplyValue(WatchedDataref& w, const DatarefValue& value);
    void ApplyIncomingBytes(const void* data, size_t len, double now_s);
    void SendToPeers(const std::vector<uint8_t>& encoded);

    UdpSocket socket_;
    std::vector<Peer> peers_;
    std::vector<WatchedDataref> watched_;
    std::unordered_map<std::string, size_t> index_by_name_;
    std::function<void(const void*, size_t)> relay_sender_;
    OwnershipTracker ownership_{/*startsAsOwner=*/true};
    const SessionCrypto* crypto_ = nullptr;
};

} // namespace flytogether
