#include "formation/rendezvous_client.h"

#include "formation/base64.h"

namespace flytogether {

bool RendezvousClient::Start(const std::string& serverHost, uint16_t serverPort) {
    server_host_ = serverHost;
    server_port_ = serverPort;
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

void RendezvousClient::CreateSession() {
    RendezvousClientMessage msg;
    msg.type = "create_session";
    Send(msg);
}

void RendezvousClient::JoinSession(const std::string& code) {
    RendezvousClientMessage msg;
    msg.type = "join_session";
    msg.code = code;
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
        const int received = socket_.ReceiveFrom(buf, sizeof(buf));
        if (received < 0) {
            break; // no more datagrams pending
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
            if (on_session_ready) on_session_ready(msg.code, msg.your_id);
        } else if (msg.type == "peer_joined") {
            std::string host;
            uint16_t port = 0;
            if (SplitHostPort(msg.peer_addr, host, port) && on_peer_joined) {
                on_peer_joined(msg.peer_id, host, port);
            }
        } else if (msg.type == "peer_left") {
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
        }
        // "keepalive_ack" itself needs no handling beyond the
        // last_received_ update above.
    }

    if (in_session_) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_received_ > timeout) {
            in_session_ = false;
            if (on_disconnected) on_disconnected();
        } else {
            MaybeSendKeepalive();
        }
    }
}

} // namespace flytogether
