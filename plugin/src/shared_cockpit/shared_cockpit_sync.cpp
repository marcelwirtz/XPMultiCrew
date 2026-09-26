#include "shared_cockpit/shared_cockpit_sync.h"

#include <algorithm>
#include <cstring>
#include <optional>

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
    master_link_quality_ = LinkQualityTracker();
}

std::string SharedCockpitSync::MasterIcaoType() const {
    if (!master_state_.HasData()) {
        return "";
    }
    const char* icao = master_state_.latest().icao_type;
    return std::string(icao, strnlen(icao, sizeof(master_state_.latest().icao_type)));
}

void SharedCockpitSync::SendOwnState(const AircraftStatePacket& packet) {
    if (role_ != SharedCockpitRole::kMaster) {
        return;
    }
    // Sealed once (if a session key is set - see SetCrypto's comment) and
    // reused for both the direct-UDP sends and the relay hand-off, same
    // "encrypt once, use everywhere" reasoning as FormationSync's
    // SendOwnState.
    if (crypto_) {
        const auto envelope =
            crypto_->Seal(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
        for (const auto& peer : peers_) {
            socket_.SendTo(peer.host, peer.port, envelope.data(), envelope.size());
        }
        if (relay_sender_) {
            relay_sender_(envelope.data(), envelope.size());
        }
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
    // See FormationSync::PollIncoming's comment (formation_sync.cpp) for
    // why this receives into an oversized raw buffer rather than
    // `&packet` directly, and why the size check below is against the
    // frozen kAircraftStateMinSize rather than this build's own
    // sizeof(AircraftStatePacket). Also comfortably covers
    // SessionCrypto::kEnvelopeOverhead once a message is encrypted.
    uint8_t buf[512];
    while (true) {
        const int received = socket_.ReceiveFrom(buf, sizeof(buf));
        if (received < 0) {
            break; // no more datagrams pending
        }

        const uint8_t* plain_data = buf;
        size_t plain_size = static_cast<size_t>(received);
        std::optional<std::vector<uint8_t>> opened;
        if (crypto_) {
            opened = crypto_->Open(buf, static_cast<size_t>(received));
            if (!opened) {
                continue; // wrong/no key, or corrupted/foreign packet
            }
            plain_data = opened->data();
            plain_size = opened->size();
        }
        if (plain_size < kAircraftStateMinSize) {
            continue;
        }
        AircraftStatePacket packet;
        std::memcpy(&packet, plain_data, std::min(plain_size, sizeof(packet)));
        ProcessIncomingPacket(packet, now_s);
    }
}

void SharedCockpitSync::IngestRelayedPacket(const void* data, size_t len, double now_s) {
    // Already the full, untruncated relayed payload (see
    // plugin_main.cpp's on_relay_received, which receives into a 4096-byte
    // buffer) - just needs the same relaxed size check and safe copy as
    // the direct-UDP path above, not a bigger receive buffer.
    //
    // Already PLAINTEXT, unlike the direct-UDP path (PollIncoming) below -
    // NOT decrypted again here. The relay channel multiplexes several
    // message shapes (position, dataref sync, ownership, weather) onto
    // one stream, and plugin_main.cpp's relay dispatcher has to decrypt
    // once before it can even peek the magic byte to know which Ingest*
    // to call in the first place - see that dispatcher's comment. Doing a
    // second Open() on already-decrypted bytes here would just fail
    // (Open() expects an AEAD envelope, not plaintext).
    if (role_ != SharedCockpitRole::kClient || len < kAircraftStateMinSize) {
        return;
    }
    AircraftStatePacket packet;
    std::memcpy(&packet, data, std::min(len, sizeof(packet)));
    ProcessIncomingPacket(packet, now_s);
}

void SharedCockpitSync::ProcessIncomingPacket(const AircraftStatePacket& incoming, double now_s) {
    // `<` against the frozen floor, not `!=`/exact-match - see
    // aircraft_state.h's kAircraftStateMinProtocolVersion comment.
    // Plausibility matters most here: the client writes this pose straight
    // into its own aircraft's position (override_planepath).
    if (incoming.magic != kAircraftStateMagic ||
        incoming.protocol_version < kAircraftStateMinProtocolVersion ||
        !IsPlausibleAircraftState(incoming)) {
        return;
    }
    AircraftStatePacket packet = incoming;
    SanitizeIcaoType(packet.icao_type);
    master_state_.OnPacketReceived(packet, now_s);
    master_link_quality_.OnPacketReceived(packet.sequence);
}

} // namespace flytogether
