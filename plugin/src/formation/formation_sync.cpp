#include "formation/formation_sync.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace flytogether {

bool FormationSync::Start(const std::string& peer_list_path) {
    peers_ = LoadPeerList(peer_list_path);
    listen_port_ = LoadPeerListListenPort(peer_list_path, kFormationUdpPort);

    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(listen_port_)) {
        socket_.Close();
        return false;
    }
    socket_.SetNonBlocking(true);
    return true;
}

void FormationSync::Stop() {
    socket_.Close();
    peers_.clear();
    remote_aircraft_.clear();
    link_quality_.clear();
}

void FormationSync::SendOwnState(const AircraftStatePacket& packet) {
    // Encrypted (envelope: nonce+mac+ciphertext) once crypto_ is set - see
    // SetCrypto()'s comment. Plaintext-length raw struct bytes otherwise,
    // only reachable before a session/crypto exists.
    if (crypto_) {
        const auto envelope =
            crypto_->Seal(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        for (const auto& peer : peers_) {
            socket_.SendTo(peer.host, peer.port, envelope.data(), envelope.size());
        }
        return;
    }
    for (const auto& peer : peers_) {
        socket_.SendTo(peer.host, peer.port, &packet, sizeof(packet));
    }
}

void FormationSync::PollIncoming(double now_s) {
    // Received into a buffer deliberately larger than sizeof(packet), not
    // `&packet` directly: a raw UDP recv into a fixed-size buffer either
    // silently truncates an oversized datagram to that exact size (POSIX)
    // or drops it outright (Windows, by default) - either way, reading
    // straight into an AircraftStatePacket-sized buffer would make it
    // impossible for this build to ever receive a *larger* packet from a
    // newer peer once a future version appends a field. The headroom here
    // is what actually makes that forward-compatible, not just the size
    // check below. Also comfortably covers SessionCrypto::kEnvelopeOverhead
    // once a message is encrypted.
    uint8_t buf[512]; // generous headroom above kAircraftStateMinSize (77)
    while (true) {
        const int received = socket_.ReceiveFrom(buf, sizeof(buf));
        if (received < 0) {
            break; // no more datagrams pending
        }

        // Decrypt first (if a session key is set) - everything below
        // operates on the plaintext bytes either way, so the rest of this
        // loop doesn't need to know or care whether encryption is active.
        const uint8_t* plain_data = buf;
        size_t plain_size = static_cast<size_t>(received);
        std::optional<std::vector<uint8_t>> opened;
        if (crypto_) {
            opened = crypto_->Open(buf, static_cast<size_t>(received));
            if (!opened) {
                continue; // wrong/no key, or corrupted/foreign packet - drop and keep draining
            }
            plain_data = opened->data();
            plain_size = opened->size();
        }

        if (plain_size < kAircraftStateMinSize) {
            continue; // truncated/malformed/foreign packet, keep draining the queue
        }
        // `packet` is this build's own (possibly older/smaller, possibly
        // up-to-date) view of the struct - only copy as much as both it
        // and the datagram actually have, so a bigger future packet from
        // a newer peer never overflows it, and a smaller one from an
        // older peer never reads past what was actually received. Fields
        // this build knows about but the sender's datagram didn't reach
        // keep their in-class default values (aircraft_state.h) rather
        // than uninitialized memory - see kAircraftStateMinSize's comment
        // for how a future field addition is expected to extend this.
        AircraftStatePacket packet;
        std::memcpy(&packet, plain_data, std::min(plain_size, sizeof(packet)));
        IngestPacket(packet, now_s);
    }
}

void FormationSync::IngestPacket(const AircraftStatePacket& incoming, double now_s) {
    // `<` against the frozen floor, not `!=`/exact-match against this
    // build's own kAircraftStateProtocolVersion - see aircraft_state.h's
    // kAircraftStateMinProtocolVersion comment for why.
    if (incoming.magic != kAircraftStateMagic ||
        incoming.protocol_version < kAircraftStateMinProtocolVersion ||
        !IsPlausibleAircraftState(incoming)) {
        return;
    }
    AircraftStatePacket packet = incoming;
    SanitizeIcaoType(packet.icao_type);
    SanitizeCallsign(packet.callsign);
    remote_aircraft_[packet.sender_id].OnPacketReceived(packet, now_s);
    link_quality_[packet.sender_id].OnPacketReceived(packet.sequence);
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
            // Drop the matching link-quality tracker too - keeping a stale
            // sender_id's entry around would (once it eventually reappears
            // with a fresh, unrelated sequence counter) read the huge
            // sequence jump as a real gap instead of a new session; a
            // fresh LinkQualityTracker() starting over from HasData()==false
            // is the correct behavior for "haven't heard from this sender
            // since it went stale".
            link_quality_.erase(it->first);
            it = remote_aircraft_.erase(it);
        } else {
            ++it;
        }
    }
}

void FormationSync::ForEachLinkQuality(const LinkQualityVisitor& visitor) const {
    for (const auto& [sender_id, tracker] : link_quality_) {
        if (tracker.HasData()) {
            visitor(sender_id, tracker.LossRatio());
        }
    }
}

} // namespace flytogether
