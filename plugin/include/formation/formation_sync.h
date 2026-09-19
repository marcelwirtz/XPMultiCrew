#pragma once

#include "flytogether/aircraft_state.h"
#include "flytogether/udp_socket.h"
#include "formation/peer_list.h"
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
    bool Start(const std::string& peer_list_path);
    void Stop();

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

private:
    UdpSocket socket_;
    std::vector<Peer> peers_;
    std::unordered_map<uint32_t, RemoteAircraft> remote_aircraft_;
};

} // namespace flytogether
