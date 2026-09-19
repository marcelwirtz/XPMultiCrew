#include "formation/formation_sync.h"

namespace flytogether {

bool FormationSync::Start(const std::string& peer_list_path) {
    peers_ = LoadPeerList(peer_list_path);

    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(kFormationUdpPort)) {
        return false;
    }
    socket_.SetNonBlocking(true);
    return true;
}

void FormationSync::Stop() {
    socket_.Close();
    peers_.clear();
    remote_aircraft_.clear();
}

void FormationSync::SendOwnState(const AircraftStatePacket& packet) {
    for (const auto& peer : peers_) {
        socket_.SendTo(peer.host, peer.port, &packet, sizeof(packet));
    }
}

void FormationSync::PollIncoming(double now_s) {
    AircraftStatePacket packet;
    while (true) {
        const int received = socket_.ReceiveFrom(&packet, sizeof(packet));
        if (received < 0) {
            break; // no more datagrams pending
        }
        if (received != static_cast<int>(sizeof(packet))) {
            continue; // malformed/foreign packet, keep draining the queue
        }
        IngestPacket(packet, now_s);
    }
}

void FormationSync::IngestPacket(const AircraftStatePacket& packet, double now_s) {
    if (packet.magic != kAircraftStateMagic ||
        packet.protocol_version != kAircraftStateProtocolVersion) {
        return;
    }
    remote_aircraft_[packet.sender_id].OnPacketReceived(packet, now_s);
}

void FormationSync::AddPeer(const std::string& host, uint16_t port) {
    for (const auto& peer : peers_) {
        if (peer.host == host && peer.port == port) {
            return; // already present
        }
    }
    peers_.push_back(Peer{host, port});
}

void FormationSync::RemovePeer(const std::string& host, uint16_t port) {
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        if (it->host == host && it->port == port) {
            peers_.erase(it);
            return;
        }
    }
}

void FormationSync::ForEachRemoteAircraft(double now_s, const RemoteAircraftVisitor& visitor) const {
    for (const auto& [sender_id, aircraft] : remote_aircraft_) {
        if (!aircraft.HasData()) {
            continue;
        }
        visitor(sender_id, aircraft.ComputePose(now_s), aircraft.latest());
    }
}

void FormationSync::RemoveStaleAircraft(double now_s, double timeout_s) {
    for (auto it = remote_aircraft_.begin(); it != remote_aircraft_.end();) {
        if (it->second.IsStale(now_s, timeout_s)) {
            it = remote_aircraft_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace flytogether
