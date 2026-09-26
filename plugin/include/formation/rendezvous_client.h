#pragma once

#include "flytogether/udp_socket.h"
#include "formation/rendezvous_protocol.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flytogether {

// Speaks the rendezvous/relay protocol (server/protocol.go) over its own
// UDP "control" socket, separate from FormationSync's peer-to-peer data
// socket. See docs/plan.md section 7 and server/README.md.
//
// Direct peer-to-peer (UDP hole punching) also runs over THIS socket, not
// the sync engines' own fixed-port sockets: the peer_addr the server hands
// out is this socket's public ip:port as the server observed it, so only
// datagrams sent from and to this socket match the NAT mappings the server
// exchange already opened. SendRelay() therefore also sends the same bytes
// straight to every known peer, and PollIncoming() hands direct datagrams
// from a known peer to on_relay_received exactly like a relayed payload -
// callers see one stream either way. Direct datagrams carry
// kDirectDatagramTag up front so they can't be confused with the server's
// JSON (an AEAD envelope starts with random nonce bytes, which could be
// '{').
//
// This client does not itself decide "direct failed, use relay instead" -
// it always relays in parallel with the direct sends once in a session,
// relying on the receivers' sequence-number dedup to make the redundancy
// harmless.
class RendezvousClient {
public:
    // `salt` is the raw (already base64-decoded) per-session value the
    // server minted - see server/session.go's Session.Salt and
    // net/session_crypto.h. Always exactly SessionCrypto::kSaltSize bytes
    // for a server running this protocol version; a caller talking to a
    // future, larger-salt server should treat any other size as unusable
    // rather than truncating/padding it into looking valid.
    using SessionReadyFn =
        std::function<void(const std::string& code, int yourId, const std::vector<uint8_t>& salt)>;
    using PeerJoinedFn = std::function<void(int peerId, const std::string& host, uint16_t port)>;
    using PeerLeftFn = std::function<void(int peerId)>;
    using RelayReceivedFn = std::function<void(int fromPeerId, const std::vector<uint8_t>& bytes)>;
    using ErrorFn = std::function<void(const std::string& message)>;
    using DisconnectedFn = std::function<void()>;

    bool Start(const std::string& serverHost, uint16_t serverPort);

    // Tells the server we're leaving (if currently in a session) before
    // closing the socket, so remaining session members are notified via
    // peer_left immediately instead of waiting up to clientTimeout (120s,
    // see server/session.go) for the stale-client sweep to notice we're
    // gone. Best-effort (UDP, fire-and-forget) - a crash/force-kill still
    // falls back to that same 120s sweep, same as always.
    void Stop();

    // `asSpectator`: see server/protocol.go's ClientMessage.Role comment -
    // never broadcasts its own position once StartRendezvous's caller
    // (plugin_main.cpp) honors this by skipping SendOwnState/SendRelay
    // for itself, but still sees/receives everyone else normally. Only
    // meaningful for Formation - Shared Cockpit doesn't use this client
    // method (it has its own StartSharedCockpitRendezvous flow, and
    // spectating a Shared Cockpit session is out of scope for now, see
    // docs/plan.md).
    void CreateSession(bool asSpectator = false);
    void JoinSession(const std::string& code, bool asSpectator = false);
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
    //
    // Also detects a dead server/network path: every incoming message
    // (including the "keepalive_ack" the server sends back for every
    // keepalive, see server/protocol.go) resets an internal "last heard
    // from the server" timer. If nothing at all arrives for
    // kServerResponseTimeout, on_disconnected fires once and we stop
    // considering ourselves in-session (so keepalives/relay go quiet
    // instead of shouting into the void) - this is what lets a caller
    // implement auto-reconnect: rejoin with the same session code once
    // this fires, see plugin_main.cpp. The ack specifically exists so a
    // client alone in a session (no peer yet, hence no relay/peer_*
    // traffic to rely on instead) still gets a periodic "still there"
    // signal - without it, "alone and quiet" would be indistinguishable
    // from "disconnected". `timeout` defaults to kServerResponseTimeout;
    // overridable so tests don't have to wait 40 real seconds to exercise
    // this path (same pattern as RemoteAircraft::IsStale/
    // SharedCockpitSync::IsMasterStale's timeout parameter).
    void PollIncoming(std::chrono::steady_clock::duration timeout = kServerResponseTimeout);

    bool InSession() const { return in_session_; }

    // Peers we've received a direct (non-relayed) datagram from within
    // kDirectPathTimeout - i.e. hole punching towards them worked.
    size_t DirectPeerCount() const;
    bool IsDirectPathUp(int peerId) const;

    // Round-trip time to the rendezvous server, measured from the last
    // keepalive we sent to the "keepalive_ack" the server sends back for
    // it (see PollIncoming's comment on why that ack exists at all) - the
    // cheapest possible RTT probe, since it reuses a message already sent
    // every ~10s rather than adding a dedicated ping. An approximation,
    // not an exact per-message round trip: if unrelated server traffic
    // (peer_joined, relay, ...) arrives between sending a keepalive and
    // its ack, this doesn't try to disambiguate which received message
    // was "the" ack - fine for a once-per-10s health indicator, not
    // precise enough for anything time-critical. HasRtt() is false until
    // at least one full keepalive/ack round trip has completed.
    bool HasRtt() const { return has_rtt_; }
    std::chrono::milliseconds Rtt() const { return last_rtt_; }

    SessionReadyFn on_session_ready;
    PeerJoinedFn on_peer_joined;
    PeerLeftFn on_peer_left;
    RelayReceivedFn on_relay_received;
    ErrorFn on_error;
    DisconnectedFn on_disconnected;
    // Fired once per peer the first time a direct datagram from it arrives
    // (again after the direct path had timed out) - diagnostics only.
    std::function<void(int peerId)> on_direct_path_up;

    static constexpr char kDirectDatagramTag[4] = {'X', 'M', 'C', 'D'};

private:
    void Send(const RendezvousClientMessage& msg);
    void SendOrQueue(const std::string& json);
    void FlushQueued();
    void ReportSendFailure();
    void SendKeepaliveNow();
    void MaybeSendKeepalive();
    void SendDirectToPeers(const void* data, size_t len);
    void MaybeSendPunch();
    void HandleDirectDatagram(const char* data, size_t len, const std::string& host, uint16_t port);

    struct DirectPeer {
        std::string host;
        uint16_t port = 0;
        bool path_up = false;
        std::chrono::steady_clock::time_point last_direct_rx{};
    };

    UdpSocket socket_;
    std::unordered_map<int, DirectPeer> direct_peers_; // peer_id -> observed addr
    // Messages waiting for the server's host name to resolve (create/join
    // right after Start()); small cap - keepalives/relay are pointless to
    // pile up, the create/join at the front is what matters.
    std::deque<std::string> queued_;
    static constexpr size_t kMaxQueued = 16;
    std::chrono::steady_clock::time_point last_direct_tx_{};
    std::string server_host_;
    uint16_t server_port_ = 0;
    bool in_session_ = false;
    std::chrono::steady_clock::time_point last_keepalive_sent_{};
    std::chrono::steady_clock::time_point last_received_{};
    bool has_rtt_ = false;
    std::chrono::milliseconds last_rtt_{0};

    // Comfortably more than 3 missed 10s keepalive/ack round-trips, to
    // absorb jitter/packet loss without false-triggering, but well under
    // the server's own 120s eviction (clientTimeout in server/session.go)
    // so a real outage is caught and retried well before the server would
    // have given up on us anyway.
    static constexpr std::chrono::seconds kServerResponseTimeout{40};

    // With no real traffic going out (a spectator, a Shared Cockpit client
    // with no switch changes), an empty tagged datagram still goes to every
    // peer this often, so our side of the NAT mapping gets opened and kept
    // alive.
    static constexpr std::chrono::seconds kPunchInterval{1};
    static constexpr std::chrono::seconds kDirectPathTimeout{5};
};

} // namespace flytogether
