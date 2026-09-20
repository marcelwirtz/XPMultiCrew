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
#include <vector>

namespace flytogether {

// UDP port for Shared Cockpit sync, separate from Formation mode's
// kFormationUdpPort so both modes could in principle run at once.
constexpr uint16_t kSharedCockpitUdpPort = 49020;

enum class SharedCockpitRole { kNone, kMaster, kClient };

// Master/client position+attitude sync for Shared Cockpit (docs/plan.md
// section 6). MVP scope: the master's own aircraft state (already-existing
// AircraftStatePacket - position/attitude/surfaces are exactly what's
// needed) is broadcast to every configured client; each client tracks it
// with the same dead-reckoning (RemoteAircraft, reused as-is from
// Formation mode) and applies the smoothed pose to its OWN aircraft via
// X-Plane's override_planepath - see core/plugin_main.cpp, where that XPLM
// interaction lives (kept out of this class so the sync/networking logic
// stays testable without XPLM).
//
// Not yet implemented: request-release control ownership of individual
// switches/systems (docs/plan.md section 6's "Besitzer pro Element") and
// the aircraft-specific dataref/command mapping that needs - this class
// only keeps both instances' idea of "where is the plane and how is it
// oriented" in sync.
class SharedCockpitSync {
public:
    bool Start(SharedCockpitRole role, const std::vector<Peer>& peers);
    void Stop();

    SharedCockpitRole role() const { return role_; }

    // Master only: broadcast own state to every configured client. No-op
    // if role() != kMaster.
    void SendOwnState(const AircraftStatePacket& packet);

    // Client only: drains incoming packets into the dead-reckoning
    // tracker below. No-op if role() != kClient.
    void PollIncoming(double now_s);

    // Client only, no-op otherwise: feeds a packet that arrived via the
    // rendezvous server's relay (docs/plan.md section 7) rather than this
    // class's own direct-UDP socket - see plugin_main.cpp's
    // on_relay_received for Shared Cockpit's dedicated RendezvousClient.
    // Needed because, unlike Formation's direct-UDP-only path, Shared
    // Cockpit peers found over the internet may be behind NAT that direct
    // UDP hole-punching doesn't get through (this class's client side
    // never sends anything on its own socket, so nothing opens a NAT hole
    // for the master's inbound stream) - relay is the guaranteed fallback.
    void IngestRelayedPacket(const void* data, size_t len, double now_s);

    // Master only: also hand every outgoing packet to this callback (set
    // once by plugin_main.cpp to relay through the rendezvous server), in
    // parallel with the direct sends above - same "send both, let the
    // receiver's sequence-number dedup make the redundancy harmless"
    // policy as Formation mode (see plugin_main.cpp's
    // SendFormationStateCallback).
    void SetRelaySender(std::function<void(const void*, size_t)> sender) {
        relay_sender_ = std::move(sender);
    }

    bool HasMasterState() const { return master_state_.HasData(); }
    AircraftPose ComputeMasterPose(double now_s) const { return master_state_.ComputePose(now_s); }
    bool IsMasterStale(double now_s, double timeout_s = 5.0) const {
        return master_state_.IsStale(now_s, timeout_s);
    }

    // Client only: the master's own icao_type, straight off the last
    // received AircraftStatePacket (already carried for free - see
    // plugin_main.cpp's BuildOwnAircraftStatePacket) - "" before any
    // packet has arrived yet. Lets the client detect it's flying a
    // different aircraft type than the master, whose per-ICAO Shared
    // Cockpit profile (shared_cockpit_config.h) the client is also using -
    // see plugin_main.cpp's aircraft-mismatch check.
    std::string MasterIcaoType() const;

    // Client only: the master link's smoothed packet-loss estimate (see
    // net/link_quality.h) - meaningless (returns 0.0) before HasMasterLinkQualityData().
    bool HasMasterLinkQualityData() const { return master_link_quality_.HasData(); }
    double MasterLinkLossRatio() const { return master_link_quality_.LossRatio(); }

    // Encrypts every SEND (direct-UDP and relay alike - SendOwnState
    // seals one envelope and hands it to both) from here on - see
    // net/session_crypto.h. On the RECEIVE side, only direct-UDP
    // (PollIncoming) decrypts here; a relayed message arrives via
    // IngestRelayedPacket already decrypted by plugin_main.cpp's relay
    // dispatcher (it has to decrypt before it can even peek the magic
    // byte to route between position/dataref/ownership/weather messages
    // sharing one relay stream - see that dispatcher's comment), so
    // IngestRelayedPacket must NOT decrypt a second time.
    void SetCrypto(const SessionCrypto* crypto) { crypto_ = crypto; }

private:
    void ProcessIncomingPacket(const AircraftStatePacket& packet, double now_s);

    SharedCockpitRole role_ = SharedCockpitRole::kNone;
    UdpSocket socket_;
    std::vector<Peer> peers_; // master's clients
    RemoteAircraft master_state_; // client's dead-reckoned view of the master
    LinkQualityTracker master_link_quality_;
    std::function<void(const void*, size_t)> relay_sender_;
    const SessionCrypto* crypto_ = nullptr;
};

} // namespace flytogether
