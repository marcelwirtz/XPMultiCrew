#pragma once

#include "XPLMDataAccess.h"

#include "flytogether/udp_socket.h"
#include "formation/peer_list.h"
#include "net/session_crypto.h"
#include "shared_cockpit/dataref_sync_protocol.h"
#include "shared_cockpit/ownership_tracker.h"
#include "shared_cockpit/shared_cockpit_config.h"
#include "shared_cockpit/sync_policy.h"

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
// radios, autopilot settings, ...) between Shared Cockpit peers flying the
// same aircraft. Flight controls (yoke, throttle, ...) aren't part of it -
// they travel as a stream from the pilot flying, see
// shared_cockpit/controls_sync_protocol.h.
//
// The pilot flying (master, SetAuthority(true)) is the single authority:
// see shared_cockpit/sync_policy.h for exactly who sends and applies what.
// Ownership claims are still carried on this channel, but only for the
// "flight" category - which side flies - never for switches.
//
// Works for any aircraft without per-aircraft code: since both peers fly
// the identical aircraft, watched dataref names resolve to the same thing
// on both sides. The only per-aircraft "profile" needed is which dataref
// names to watch (see shared_cockpit/shared_cockpit_config.h's DATAREF
// lines).
class DatarefSync {
public:
    // `isMaster`: see SetAuthority. The master's first Poll() sends every
    // watched value, which gives a freshly joined co-pilot the complete
    // cockpit state; the co-pilot just takes its current values as the
    // baseline.
    bool Start(const std::vector<DatarefSyncSpec>& watched, const std::vector<Peer>& peers, bool isMaster);
    void Stop();

    // Call ~10 times a second: sends what changed here, applies what the
    // peer sent - see sync_policy.h.
    void Poll(double now_s);

    // Who flies: true makes this side the authority. Called on a role swap.
    void SetAuthority(bool isMaster);
    bool is_master() const { return is_master_; }

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

    // Desync detection (plugin_main.cpp exchanges these with the peer every
    // few seconds): a hash of every watched dataref's current value, in
    // profile order, plus a hash of the profile's dataref names - equal
    // name hashes mean both sides watch the same list, so the value hashes
    // can be compared index by index.
    std::vector<uint32_t> ValueHashes() const;
    uint32_t ProfileHash() const;
    const std::string& WatchedName(size_t index) const { return watched_[index].name; }
    std::string DescribeCurrentValue(size_t index) const;
    // Master: sends every watched value now (a co-pilot asked for it, or
    // the "bring the other side back in line" button).
    void ResendAll();

    // Local UI action ("I'm taking the controls") - see
    // OwnershipTracker::Claim. Only used for DatarefCategory::kFlight.
    // Sent to the peer (+ relay) a few times in a row (best-effort UDP, no
    // ACK below this). A no-op if this side already owns `category`.
    void ClaimOwnership(DatarefCategory category);

    bool Owns(DatarefCategory category) const { return ownership_.Owns(category); }

    // Encrypts every outgoing message (dataref sync + ownership claims,
    // direct-UDP and relay alike, via the shared SendToPeers choke point)
    // from here on - see net/session_crypto.h.
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
        WatchedSyncState state;
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
    bool is_master_ = true;
    double next_refresh_s_ = 0.0;
    const SessionCrypto* crypto_ = nullptr;
};

} // namespace flytogether
