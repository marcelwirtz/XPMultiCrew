#pragma once

#include "flytogether/udp_socket.h"
#include "formation/rendezvous_protocol.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace flytogether {

// Speaks the rendezvous/relay protocol (server/protocol.go) over its own
// UDP "control" socket, separate from FormationSync's peer-to-peer data
// socket. See docs/plan.md section 7 and server/README.md.
//
// This client does not itself decide "direct failed, use relay instead" -
// see plugin_main.cpp's simplification: it always relays in parallel with
// direct sends once in a session, relying on RemoteAircraft's existing
// sequence-number dedup to make the redundancy harmless.
class RendezvousClient {
public:
    using SessionReadyFn = std::function<void(const std::string& code, int yourId)>;
    using PeerJoinedFn = std::function<void(int peerId, const std::string& host, uint16_t port)>;
    using PeerLeftFn = std::function<void(int peerId)>;
    using RelayReceivedFn = std::function<void(int fromPeerId, const std::vector<uint8_t>& bytes)>;
    using ErrorFn = std::function<void(const std::string& message)>;

    bool Start(const std::string& serverHost, uint16_t serverPort);

    // Tells the server we're leaving (if currently in a session) before
    // closing the socket, so remaining session members are notified via
    // peer_left immediately instead of waiting up to clientTimeout (120s,
    // see server/session.go) for the stale-client sweep to notice we're
    // gone. Best-effort (UDP, fire-and-forget) - a crash/force-kill still
    // falls back to that same 120s sweep, same as always.
    void Stop();

    void CreateSession();
    void JoinSession(const std::string& code);
    void SendRelay(const void* data, size_t len);

    // Drains all pending server messages, invoking the callbacks below, and
    // - as long as we're in a session - sends a keepalive roughly every 10s
    // using a wall-clock timer (std::chrono::steady_clock) rather than the
    // caller's own clock. This matters: an earlier version scheduled
    // keepalives from X-Plane's sim-elapsed-time, which stops advancing
    // while the sim is paused/loading, while the server's 30s eviction
    // timeout is wall-clock based - a pause longer than that gap silently
    // desynced the two, and the client kept trying to relay into a session
    // the server had already forgotten. Call this at whatever cadence is
    // convenient (e.g. once per rendered frame); it self-throttles.
    void PollIncoming();

    bool InSession() const { return in_session_; }

    SessionReadyFn on_session_ready;
    PeerJoinedFn on_peer_joined;
    PeerLeftFn on_peer_left;
    RelayReceivedFn on_relay_received;
    ErrorFn on_error;

private:
    void Send(const RendezvousClientMessage& msg);
    void SendKeepaliveNow();
    void MaybeSendKeepalive();

    UdpSocket socket_;
    std::string server_host_;
    uint16_t server_port_ = 0;
    bool in_session_ = false;
    std::chrono::steady_clock::time_point last_keepalive_sent_{};
};

} // namespace flytogether
