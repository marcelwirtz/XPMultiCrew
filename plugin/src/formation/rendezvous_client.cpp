#include "formation/rendezvous_client.h"

#include "formation/base64.h"

#include <algorithm>
#include <cstring>

namespace flytogether {

bool RendezvousClient::Start(const std::string& serverHost, uint16_t serverPort) {
    server_host_ = serverHost;
    server_port_ = serverPort;
    direct_peers_.clear();
    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(0)) { // ephemeral local port
        return false;
    }
    socket_.SetNonBlocking(true);
    return true;
}

void RendezvousClient::Stop() {
    if (in_session_) {
        RendezvousClientMessage msg;
        msg.type = "leave_session";
        Send(msg);
    }
    socket_.Close();
    in_session_ = false;
    direct_peers_.clear();
}

void RendezvousClient::Send(const RendezvousClientMessage& msg) {
    const std::string json = EncodeClientMessage(msg);
    if (!socket_.SendTo(server_host_, server_port_, json.data(), json.size())) {
        // Most likely cause: DNS resolution of server_host_ failed (see
        // UdpSocket::ResolveHostPort) - this used to fail completely
        // silently, leaving the caller stuck thinking a session might still
        // come through when the request never actually left the machine.
        if (on_error) {
            on_error("failed to send to " + server_host_ + ":" + std::to_string(server_port_) +
                      " (DNS resolution or network error)");
        }
    }
}

void RendezvousClient::CreateSession(bool asSpectator) {
    RendezvousClientMessage msg;
    msg.type = "create_session";
    msg.is_spectator = asSpectator;
    Send(msg);
}

void RendezvousClient::JoinSession(const std::string& code, bool asSpectator) {
    RendezvousClientMessage msg;
    msg.type = "join_session";
    msg.code = code;
    msg.is_spectator = asSpectator;
    Send(msg);
}

void RendezvousClient::SendRelay(const void* data, size_t len) {
    if (!in_session_) {
        return;
    }
    RendezvousClientMessage msg;
    msg.type = "relay";
    msg.payload = Base64Encode(data, len);
    Send(msg);
    SendDirectToPeers(data, len);
}

void RendezvousClient::SendDirectToPeers(const void* data, size_t len) {
    if (direct_peers_.empty()) {
        return;
    }
    std::vector<char> datagram(sizeof(kDirectDatagramTag) + len);
    std::memcpy(datagram.data(), kDirectDatagramTag, sizeof(kDirectDatagramTag));
    if (len > 0) {
        std::memcpy(datagram.data() + sizeof(kDirectDatagramTag), data, len);
    }
    for (const auto& [peer_id, peer] : direct_peers_) {
        socket_.SendTo(peer.host, peer.port, datagram.data(), datagram.size());
    }
    last_direct_tx_ = std::chrono::steady_clock::now();
}

void RendezvousClient::MaybeSendPunch() {
    if (!in_session_ || direct_peers_.empty()) {
        return;
    }
    if (std::chrono::steady_clock::now() - last_direct_tx_ < kPunchInterval) {
        return;
    }
    SendDirectToPeers(nullptr, 0);
}

void RendezvousClient::HandleDirectDatagram(const char* data, size_t len, const std::string& host,
                                            uint16_t port) {
    // Only accept direct traffic from addresses the server told us about -
    // anything else is noise (or someone probing the port), never a peer.
    const auto it = std::find_if(direct_peers_.begin(), direct_peers_.end(), [&](const auto& entry) {
        return entry.second.host == host && entry.second.port == port;
    });
    if (it == direct_peers_.end()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    DirectPeer& peer = it->second;
    const bool was_up = peer.path_up && now - peer.last_direct_rx <= kDirectPathTimeout;
    peer.last_direct_rx = now;
    peer.path_up = true;
    if (!was_up && on_direct_path_up) {
        on_direct_path_up(it->first);
    }

    const size_t payload_len = len - sizeof(kDirectDatagramTag);
    if (payload_len == 0) {
        return; // hole-punch/keepalive only
    }
    if (on_relay_received) {
        const auto* payload = reinterpret_cast<const uint8_t*>(data) + sizeof(kDirectDatagramTag);
        on_relay_received(it->first, std::vector<uint8_t>(payload, payload + payload_len));
    }
}

size_t RendezvousClient::DirectPeerCount() const {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<size_t>(std::count_if(direct_peers_.begin(), direct_peers_.end(), [&](const auto& entry) {
        return entry.second.path_up && now - entry.second.last_direct_rx <= kDirectPathTimeout;
    }));
}

void RendezvousClient::SendKeepaliveNow() {
    RendezvousClientMessage msg;
    msg.type = "keepalive";
    Send(msg);
}

void RendezvousClient::MaybeSendKeepalive() {
    if (!in_session_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_keepalive_sent_ < std::chrono::seconds(10)) {
        return;
    }
    last_keepalive_sent_ = now;
    SendKeepaliveNow();
}

void RendezvousClient::PollIncoming(std::chrono::steady_clock::duration timeout) {
    char buf[4096];
    while (true) {
        std::string from_host;
        uint16_t from_port = 0;
        const int received = socket_.ReceiveFrom(buf, sizeof(buf), &from_host, &from_port);
        if (received < 0) {
            break; // no more datagrams pending
        }
        if (static_cast<size_t>(received) >= sizeof(kDirectDatagramTag) &&
            std::memcmp(buf, kDirectDatagramTag, sizeof(kDirectDatagramTag)) == 0) {
            HandleDirectDatagram(buf, static_cast<size_t>(received), from_host, from_port);
            continue; // peer traffic, not proof the server is alive
        }

        RendezvousServerMessage msg;
        if (!DecodeServerMessage(std::string(buf, static_cast<size_t>(received)), msg)) {
            continue;
        }
        // Any decodable message at all, including a bare keepalive_ack,
        // is proof the server is still there - see kServerResponseTimeout.
        last_received_ = std::chrono::steady_clock::now();

        if (msg.type == "session_created") {
            in_session_ = true;
            last_keepalive_sent_ = last_received_;
            if (on_session_ready) on_session_ready(msg.code, msg.your_id, Base64Decode(msg.salt));
        } else if (msg.type == "peer_joined") {
            std::string host;
            uint16_t port = 0;
            if (SplitHostPort(msg.peer_addr, host, port)) {
                DirectPeer& peer = direct_peers_[msg.peer_id];
                if (peer.host != host || peer.port != port) {
                    peer = DirectPeer{host, port};
                }
                if (on_peer_joined) on_peer_joined(msg.peer_id, host, port);
            }
        } else if (msg.type == "peer_left") {
            direct_peers_.erase(msg.peer_id);
            if (on_peer_left) on_peer_left(msg.peer_id);
        } else if (msg.type == "relay") {
            if (on_relay_received) {
                on_relay_received(msg.from_peer_id, Base64Decode(msg.payload));
            }
        } else if (msg.type == "error") {
            // The most common cause is the server having evicted a session
            // we thought we were still in (e.g. after a long sim pause) -
            // stop treating ourselves as in-session so SendRelay()/the
            // keepalive above go quiet instead of retrying into nothing.
            in_session_ = false;
            if (on_error) on_error(msg.message);
        } else if (msg.type == "keepalive_ack") {
            // See HasRtt()/Rtt()'s comment for why this is an
            // approximation, not a precise per-message round trip.
            has_rtt_ = true;
            last_rtt_ = std::chrono::duration_cast<std::chrono::milliseconds>(last_received_ -
                                                                                last_keepalive_sent_);
        }
    }

    if (in_session_) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_received_ > timeout) {
            in_session_ = false;
            direct_peers_.clear(); // the rejoin re-sends the current peer list
            if (on_disconnected) on_disconnected();
        } else {
            MaybeSendKeepalive();
            MaybeSendPunch();
        }
    }
}

} // namespace flytogether
