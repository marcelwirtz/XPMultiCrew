#pragma once

#include "flytogether/udp_socket.h"
#include "shared_cockpit/shared_cockpit_config.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace flytogether {

// Fixed local ports for plugin<->companion-app communication (both always
// on 127.0.0.1 - this is not meant to control a remote X-Plane instance,
// only the one running alongside the companion app on the same machine).
constexpr uint16_t kControlUdpPort = 49030;    // plugin listens here
constexpr uint16_t kCompanionUdpPort = 49031;  // companion app listens here

// Replaces the in-sim XPLMCreateWindowEx control window (see the
// window-vs-companion-app discussion this followed - X-Plane's window APIs
// turned out too fiddly to get a proper cross-platform-quality UI out of,
// e.g. content not tracking the window when dragged). All session-
// management UI now lives in companion/, a normal cross-platform desktop
// (browser-based) app; the plugin only does X-Plane-side work
// (datarefs, networking, drawing) and this tiny listener.
//
// Protocol: plain-text, one command per line, sent as a UDP datagram to
// kControlUdpPort:
//   CREATE_SESSION <host:port>
//   JOIN_SESSION <host:port> <code>
//   START_SHARED_COCKPIT <MASTER|CLIENT> <rendezvous host:port> <code, empty for MASTER>
//   REQUEST_OWNERSHIP <engine|avionics|systems>
//   RESPOND_OWNERSHIP <engine|avionics|systems> <grant|deny>
//   DISCONNECT_FORMATION
//   DISCONNECT_SHARED_COCKPIT
//   GET_STATUS
// REQUEST_OWNERSHIP asks to take over a Shared Cockpit "systems" dataref
// category from whichever side currently holds it - see
// shared_cockpit/ownership_tracker.h's request/grant/deny handshake. Not
// an instant claim: the peer has to Grant or Deny it (or simply not
// respond within OwnershipTracker::kRequestTimeoutS, which reads the same
// as a Deny). RESPOND_OWNERSHIP is that Grant/Deny, for a category this
// side currently owns and the peer has an outstanding request for - a
// no-op if there's no live incoming request for it any more (already
// answered, or the requester's own timeout already passed). Both silently
// ignore an unrecognized category name (same "don't hard-fail on the
// unknown" spirit as this listener's other parsing).
// DISCONNECT_FORMATION/DISCONNECT_SHARED_COCKPIT are the explicit opt-out
// for RendezvousClient::on_disconnected's auto-reconnect (see its
// comment): plugin_main.cpp keeps retrying a lost connection on its own
// forever, specifically so a transient network/server blip doesn't need
// the user to notice and re-click Create/Join - these commands are the
// only way to actually stop that and go back to "not connected".
// Shared Cockpit's peer discovery goes through the same rendezvous/relay
// server as Formation mode (docs/plan.md section 7), not a manually-typed
// peer address - see plugin_main.cpp's dedicated g_shared_cockpit_rendezvous
// client. This is what lets it work over the internet without port
// forwarding: MASTER creates a session and gets a code back (surfaced via
// SHARED_COCKPIT status, same as Formation's CREATE_SESSION), CLIENT joins
// with that code.
// The plugin always pushes status to 127.0.0.1:kCompanionUdpPort (not just
// in reply to GET_STATUS) whenever it changes, as:
//   FORMATION <status text>
//   FORMATION_CODE <code, empty if none yet>
//   SHARED_COCKPIT <status text>
//   SHARED_COCKPIT_CODE <code, empty if none yet (CLIENT never has one)>
//   SHARED_COCKPIT_OWNERSHIP <engine>:<state> <avionics>:<state> <systems>:<state>
//     <state> is one of:
//       me        - this side owns it, no outstanding request from the peer
//       peer      - the peer owns it, this side has no outstanding request
//       pending   - the peer owns it, AND this side is waiting on a
//                   response to its own request for it
//       requested - this side owns it, AND the peer has an outstanding
//                   request for it awaiting a local Grant/Deny (via
//                   RESPOND_OWNERSHIP)
//   PEERS <sender_id>:<icao>;<sender_id>:<icao>;... (Formation only, empty if none)
//   LINK_QUALITY formation_server_rtt_ms:<ms|?> formation_peer_loss_pct:<id>:<pct>;...
//                sc_server_rtt_ms:<ms|?> sc_master_loss_pct:<pct|?>
//   SHARED_COCKPIT_AIRCRAFT_MISMATCH <own icao>:<master icao> (empty if matching/unknown)
//   SIM_READY <0|1>
//   PLUGIN_VERSION <version>
// SHARED_COCKPIT_OWNERSHIP reflects DatarefSync::Owns() for each category
// from this side's point of view (empty string before Shared Cockpit's
// dataref sync has actually started) - plugin_main.cpp pushes it whenever
// it changes, whether from this side's own REQUEST_OWNERSHIP or from the
// peer claiming a category over the network.
// PLUGIN_VERSION is the actually-running plugin's version (set once at
// startup, see plugin_main.cpp's XPMULTICREW_VERSION - derived from git at
// build time). The companion app also reads this off disk (a version.txt
// dropped next to the .xpl) and from its own embedded copy, so it can show
// "installed", "available", and (only when X-Plane is actually running)
// "currently loaded" versions - useful since installing an update doesn't
// take effect until X-Plane is restarted, so those three can legitimately
// disagree for a while.
// The *_CODE fields exist so the companion app can fill the session-code
// input field for you when you create a session, instead of you having to
// read it out of the status text and retype it (mostly useful for
// re-sharing it, or if you fat-fingered "Join" instead of "Create").
// SIM_READY reflects whether X-Plane has actually finished loading a
// flight. Set two ways: (1) XPLM_MSG_PLANE_LOADED/UNLOADED for the user's
// own aircraft, which correctly catches every flight *reload* during a
// running session; and (2) plugin_main.cpp's PollControlListenerCallback
// calling IsSimReady()/SetSimReady(true) on its own first tick each time
// it runs. (2) exists because (1) alone misses the very first flight of
// an X-Plane session: X-Plane loads the default aircraft as part of its
// own startup, before Resources/plugins are enabled, so that initial
// XPLM_MSG_PLANE_LOADED can fire before this plugin is even listening for
// it - leaving SIM_READY stuck at 0 forever otherwise, with the companion
// app's buttons permanently disabled. Flight loop callbacks (unlike
// messages) are a reliable proxy either way: they simply don't run during
// a loading screen (same reasoning applies to this listener's own Poll(),
// so a command sent while SIM_READY is 0 doesn't get lost on the protocol
// level, just delayed until loading finishes) and reliably resume once
// it's actually running - including for that very first flight.
// The companion app additionally chooses to disable its Create/Join/Start
// buttons while SIM_READY is 0, so a click doesn't just appear to do
// nothing for a while - see companion/README.md for the frontend side of
// this and companion/main.go for the status-listening side.
class ControlListener {
public:
    struct Callbacks {
        std::function<void(const std::string& hostPort)> on_create_session;
        std::function<void(const std::string& hostPort, const std::string& code)> on_join_session;
        std::function<void(bool isMaster, const std::string& serverHostPort, const std::string& code)>
            on_start_shared_cockpit;
        std::function<void()> on_disconnect_formation;
        std::function<void()> on_disconnect_shared_cockpit;
        std::function<void(DatarefCategory)> on_request_ownership;
        std::function<void(DatarefCategory, bool grant)> on_respond_ownership;
    };

    bool Start(const Callbacks& callbacks);
    void Stop();

    // Call every frame or so: drains and dispatches pending commands.
    void Poll();

    // Pushes an immediate status update to the companion app and remembers
    // it for the next GET_STATUS / periodic push.
    void SetFormationStatus(const std::string& text);
    void SetFormationCode(const std::string& code);
    void SetSharedCockpitStatus(const std::string& text);
    void SetSharedCockpitCode(const std::string& code);

    // The four states a category's ownership can be in from this side's
    // point of view - see the SHARED_COCKPIT_OWNERSHIP wire-format comment
    // above for what each one means to the companion app.
    enum class OwnershipUiState : uint8_t { kMe, kPeer, kPending, kRequested };

    // `state[i]` = this side's OwnershipUiState for DatarefCategory(i).
    // Pass all three every time (cheap, and avoids a partial-update
    // ordering question) rather than one at a time.
    void SetSharedCockpitOwnership(const std::array<OwnershipUiState, kDatarefCategoryCount>& state);
    void SetFormationPeers(const std::string& encodedPeers);
    // `encoded` is pre-built by the caller (plugin_main.cpp) - same
    // pattern as SetFormationPeers above - since building it needs data
    // from several independent sources (both RendezvousClients' RTT,
    // FormationSync's per-peer loss, SharedCockpitSync's master-link
    // loss) that this listener has no access to on its own.
    void SetLinkQuality(const std::string& encoded);
    // `encoded` is "" when there's no mismatch (or nothing to compare yet)
    // - see the wire-format comment above.
    void SetSharedCockpitAircraftMismatch(const std::string& encoded);
    void SetSimReady(bool ready);
    bool IsSimReady() const { return sim_ready_; }
    void SetPluginVersion(const std::string& version);

private:
    void HandleLine(const std::string& line);
    void SendStatus();

    UdpSocket socket_;
    Callbacks callbacks_;
    std::string formation_status_ = "not connected";
    std::string formation_code_;
    std::string shared_cockpit_status_ = "not started";
    std::string shared_cockpit_code_;
    std::string shared_cockpit_ownership_; // empty until SetSharedCockpitOwnership is called
    std::string formation_peers_;
    std::string link_quality_;
    std::string shared_cockpit_aircraft_mismatch_;
    bool sim_ready_ = false;
    std::string plugin_version_ = "unknown";
};

} // namespace flytogether
