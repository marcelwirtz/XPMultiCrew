#pragma once

#include "flytogether/aircraft_state.h"
#include "flytogether/udp_socket.h"
#include "formation/peer_list.h"
#include "net/link_quality.h"
#include "net/session_crypto.h"
#include "sync/remote_aircraft.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flytogether {

// Ties together the UDP transport, the (Phase 1, file-based) peer list and
// per-sender dead reckoning into one thing the plugin can drive from its
// flight loop callback. See docs/plan.md section 5.
//
// Not hardcoded to two participants: remote aircraft are tracked by the
// `sender_id` in each packet, in a map that grows/shrinks as peers
// appear/go stale, independent of how many entries are in the peer list.
class FormationSync {
public:
    // False if the LAN listen port (kFormationUdpPort, or the peer list's
    // PORT line) couldn't be bound. Peers are loaded either way and the
    // rest keeps working - rendezvous sessions don't use this socket - so
    // callers treat that as "LAN direct unavailable", not "Formation off".
    bool Start(const std::string& peer_list_path);
    void Stop();
    uint16_t listen_port() const { return listen_port_; }

    void SendOwnState(const AircraftStatePacket& packet);

    // Drains all pending incoming packets and feeds them to their
    // respective RemoteAircraft. `now_s` should be XPLMGetElapsedTime().
    void PollIncoming(double now_s);

    // Feeds one already-received packet into the same dead-reckoning
    // pipeline PollIncoming() uses - the entry point for packets that
    // arrived via the rendezvous server's relay (formation/rendezvous_client.h)
    // rather than this class's own direct-P2P socket. Silently ignored if
    // the packet's magic/version don't match.
    void IngestPacket(const AircraftStatePacket& packet, double now_s);

    // Adds/removes a peer this instance sends its own state to, beyond
    // whatever was loaded from the peer list file - used once a rendezvous
    // session discovers a peer's address for direct UDP hole-punching
    // (docs/plan.md section 7). Safe to call for a peer already present
    // (AddPeer is a no-op then) or absent (RemovePeer is a no-op then).
    void AddPeer(const std::string& host, uint16_t port);
    void RemovePeer(const std::string& host, uint16_t port);

    using RemoteAircraftVisitor = std::function<void(
        uint32_t sender_id, const AircraftPose& pose, const AircraftStatePacket& latest)>;
    void ForEachRemoteAircraft(double now_s, const RemoteAircraftVisitor& visitor) const;

    void RemoveStaleAircraft(double now_s, double timeout_s = 8.0);

    size_t peer_count() const { return peers_.size(); }
    size_t tracked_aircraft_count() const { return remote_aircraft_.size(); }

    using LinkQualityVisitor = std::function<void(uint32_t sender_id, double loss_ratio)>;
    // Visits every currently-tracked sender's LinkQualityTracker that
    // already has a measurement (see LinkQualityTracker::HasData()) - fed
    // from the exact same IngestPacket() call site as remote_aircraft_, so
    // it always covers the same set of senders. Used by plugin_main.cpp to
    // build the LINK_QUALITY status line.
    void ForEachLinkQuality(const LinkQualityVisitor& visitor) const;

    // Encrypts every direct-UDP send and decrypts every direct-UDP
    // receive with `crypto` from here on - see net/session_crypto.h and
    // docs/plan.md's Session-Auth/Verschlüsselung (Phase 4). `crypto` must
    // outlive this object (plugin_main.cpp owns it for the session's
    // lifetime); pass nullptr to go back to plaintext (only meaningful
    // before a session exists - there's no "downgrade mid-session" path).
    // The RELAY path (formation traffic through the rendezvous server) is
    // NOT handled here: unlike SharedCockpitSync/DatarefSync, Formation's
    // relay send/receive is driven directly by plugin_main.cpp rather
    // than through a SetRelaySender callback owned by this class, so
    // plugin_main.cpp seals/opens that path itself, reusing the exact
    // same SessionCrypto instance it hands to this method.
    void SetCrypto(const SessionCrypto* crypto) { crypto_ = crypto; }

private:
    UdpSocket socket_;
    uint16_t listen_port_ = kFormationUdpPort;
    std::vector<Peer> peers_;
    std::unordered_map<uint32_t, RemoteAircraft> remote_aircraft_;
    std::unordered_map<uint32_t, LinkQualityTracker> link_quality_;
    const SessionCrypto* crypto_ = nullptr;
};

} // namespace flytogether
