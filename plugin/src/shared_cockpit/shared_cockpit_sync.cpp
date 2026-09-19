#include "shared_cockpit/shared_cockpit_sync.h"

#include <cstring>

namespace flytogether {

bool SharedCockpitSync::Start(SharedCockpitRole role, const std::vector<Peer>& peers) {
    role_ = role;
    peers_ = peers;

    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(kSharedCockpitUdpPort)) {
        return false;
    }
    socket_.SetNonBlocking(true);
    return true;
}

void SharedCockpitSync::Stop() {
    socket_.Close();
    peers_.clear();
    role_ = SharedCockpitRole::kNone;
}

void SharedCockpitSync::SendOwnState(const AircraftStatePacket& packet) {
    if (role_ != SharedCockpitRole::kMaster) {
        return;
    }
    for (const auto& peer : peers_) {
        socket_.SendTo(peer.host, peer.port, &packet, sizeof(packet));
    }
    if (relay_sender_) {
        relay_sender_(&packet, sizeof(packet));
    }
}

void SharedCockpitSync::PollIncoming(double now_s) {
    if (role_ != SharedCockpitRole::kClient) {
        return;
    }
    AircraftStatePacket packet;
    while (true) {
        const int received = socket_.ReceiveFrom(&packet, sizeof(packet));
        if (received < 0) {
            break; // no more datagrams pending
        }
        if (received != static_cast<int>(sizeof(packet))) {
            continue; // malformed/foreign packet, keep draining the queue
        }
        ProcessIncomingPacket(packet, now_s);
    }
}

void SharedCockpitSync::IngestRelayedPacket(const void* data, size_t len, double now_s) {
    if (role_ != SharedCockpitRole::kClient || len != sizeof(AircraftStatePacket)) {
        return;
    }
    AircraftStatePacket packet;
    std::memcpy(&packet, data, sizeof(packet));
    ProcessIncomingPacket(packet, now_s);
}

void SharedCockpitSync::ProcessIncomingPacket(const AircraftStatePacket& packet, double now_s) {
    if (packet.magic != kAircraftStateMagic ||
        packet.protocol_version != kAircraftStateProtocolVersion) {
        return;
    }
    master_state_.OnPacketReceived(packet, now_s);
}

} // namespace flytogether
