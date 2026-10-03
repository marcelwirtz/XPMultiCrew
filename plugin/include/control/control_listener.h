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
//   CREATE_SESSION <host:port> [SPECTATOR]
//   JOIN_SESSION <host:port> <code> [SPECTATOR]
//   START_SHARED_COCKPIT <MASTER|CLIENT> <rendezvous host:port> <code, empty for MASTER>
//   LAN_CONNECT_FORMATION <host:port> <code>
//   CLAIM_OWNERSHIP <engine|avionics|systems|flight>
//   DISCONNECT_FORMATION
//   DISCONNECT_SHARED_COCKPIT
//   RELOAD_CSL
//   SET_PREFS <callsign|-> <labels 0|1> <envsync 0|1> [<rightseat 0|1> [<directp2p 0|1> [<debuglog 0|1>]]]
//   SYNC_ENV                   - follow the host's time & weather once more (see below)
//   LEARN_START / LEARN_STOP
//   ROUTE_SHARE <payload> / ROUTE_CLEAR
//   CHECKLIST_SYNC <payload>   - shared checklist state, relayed to the Shared Cockpit peer as-is
//   SC_RESYNC                  - both sides re-send the values of the categories they own
//   WATCH <name[idx]>;...      - datarefs whose values the companion wants (checklist auto-check)
//   SHOW_OVERLAY <seconds> <text> - one line of text on the sim screen (the landing rating)
//   SET_APPROACH_COACH <0|1>   - Approach Coach on/off (sync/approach_coach.h); the plugin
//                                saves it itself, so it's also switchable from X-Plane's menu
//   GET_STATUS
// ROUTE_SHARE sends a planned route (the companion's own text encoding,
// passed through untouched, no spaces, at most kMaxRoutePayload bytes) to
// everyone in the Multiplayer session; ROUTE_CLEAR withdraws it.
// LEARN_START starts "learn from the cockpit" for the profile editor: the
// plugin watches every writable dataref, ignores what moves on its own for
// the first seconds, then reports what the user changes by flipping
// switches (LEARN / LEARN_CHANGES below) until LEARN_STOP.
// SET_PREFS carries the companion app's settings: the callsign shown to
// others ("-" = use the aircraft's tail number), whether XPMP2 draws
// labels/map icons for remote aircraft, and whether Formation time &
// weather sync is on (the session creator shares, everyone else follows).
// directp2p 0 turns off direct peer-to-peer (hole punching) for both
// Formation and Shared Cockpit - relay through the server only; absent = on.
// debuglog 1 writes the periodic position/peer-count lines to Log.txt;
// absent = off (startup, state changes and errors only).
// A follower takes over the host's time & weather once after joining;
// re-applying them periodically froze its sim for seconds each time, so
// after that only SYNC_ENV (the companion's button) does it again.
// Echoed back as PREFS, so the companion re-sends them whenever the plugin
// lost them (X-Plane restart).
// The optional trailing SPECTATOR token on CREATE_SESSION/JOIN_SESSION
// (Formation only - Shared Cockpit has no spectator mode, see
// docs/plan.md) means this side never broadcasts its own aircraft state
// into the session (see plugin_main.cpp's g_formation_is_spectator), but
// still sees and renders every other peer normally - a way to watch a
// formation flight without occupying a slot in it. Preserved across an
// auto-reconnect (see g_formation_reconnect_is_spectator) so a dropped-
// and-restored connection doesn't silently start broadcasting a spectator
// again.
// CLAIM_OWNERSHIP takes over a Shared Cockpit "systems" dataref category
// from whichever side currently holds it - immediately, no permission
// step - see shared_cockpit/ownership_tracker.h's claim-and-tell model.
// Silently ignores an unrecognized category name (same "don't hard-fail on
// the unknown" spirit as this listener's other parsing).
// LAN_CONNECT_FORMATION bypasses the rendezvous server entirely - it hands
// the plugin a peer's address directly, plus a manually-shared code both
// sides agree on out of band (see net/session_crypto.h's code-only
// constructor for why, and its accepted trade-off vs. the salted,
// rendezvous-minted key). The companion app currently has no UI that
// sends it (its mDNS "Nearby on LAN" discovery was removed again - it
// picked up unrelated LAN devices), but the command stays so a future
// LAN-connect UI, or a manual UDP datagram for testing, can still use it. Calling it a second time with a different
// address just adds another direct peer to the same LAN group (all LAN
// peers share one code, same as one rendezvous session's code covers
// everyone in it); calling it with a DIFFERENT code while LAN peers from
// an earlier call are still connected is rejected (see plugin_main.cpp's
// LanConnectFormation) - disconnect first to switch codes. Mutually
// exclusive with an active rendezvous Formation session (one Formation
// link uses either the rendezvous server or direct LAN peers, not both at
// once); DISCONNECT_FORMATION tears down either kind the same way.
// DISCONNECT_FORMATION/DISCONNECT_SHARED_COCKPIT are the explicit opt-out
// for RendezvousClient::on_disconnected's auto-reconnect (see its
// comment): plugin_main.cpp keeps retrying a lost connection on its own
// forever, specifically so a transient network/server blip doesn't need
// the user to notice and re-click Create/Join - these commands are the
// only way to actually stop that and go back to "not connected".
// RELOAD_CSL re-scans Resources/CSL (and the shared CSL folders, see
// plugin_main.cpp's kKnownSharedCslDirs) without restarting X-Plane - for
// a CSL package dropped in while the sim is running, or a tweaked
// xsb_aircraft.txt. Implemented as a full XPMP2 cleanup/re-init (see
// plugin_main.cpp's ReloadCsl), so remote aircraft briefly disappear and
// get re-matched against the fresh model set; sessions stay connected.
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
//   SHARED_COCKPIT_OWNERSHIP <engine>:<state> <avionics>:<state> <systems>:<state> <flight>:<state>
//     (flight = who flies the aircraft, i.e. who is MASTER right now)
//     <state> is one of:
//       me   - this side owns it
//       peer - the peer owns it
//   PEERS <sender_id>:<icao>:<callsign>;... (Formation only, empty if none)
//   LINK_QUALITY formation_server_rtt_ms:<ms|?> formation_peer_loss_pct:<id>:<pct>;...
//                formation_peer_path:<id>:<direct|relay>;...
//                sc_server_rtt_ms:<ms|?> sc_master_loss_pct:<pct|?> sc_path:<direct|relay|?>
//   PREFS <callsign|-> <labels 0|1> <envsync 0|1> <rightseat 0|1> <directp2p 0|1> <debuglog 0|1>
//   CHECKLIST_REMOTE <payload> (the peer's latest CHECKLIST_SYNC, empty if none)
//   SC_DESYNC <profiles_differ 0|1> <name>=<our value>;... (datarefs that differ from the peer)
//   WATCH_VALUES <name[idx]>=<value>;... (see WATCH)
//   OWN_ICAO <this aircraft's ICAO type, empty if unknown>
//   WIND <alt_ft>:<from_deg_true>:<kt>;... (X-Plane's wind layers at the aircraft)
//   ROUTE_SHARED <sender_id> <payload> (empty if none; sender_id 0 = our own)
//   LEARN <idle|baseline|watching> <candidates> <noisy>
//   LEARN_CHANGES <name>|<before>|<after>;... (see LEARN_START; at most 40)
//   TCAS_STATUS <ok|remote|blocked:<plugin name>> (empty before XPMP2 was enabled)
//     Who owns X-Plane's TCAS/AI planes: us, the XPMP2 Remote Client (fine,
//     it shows everyone's planes) or another plugin such as LiveTraffic
//     (then our peers are drawn but missing from TCAS and X-Plane's map).
//   SELF_POS <lat> <lon> <alt_ft> <heading_deg_true> <groundspeed_kt> <magnetic_variation_deg_east>
//            <on_ground 0|1> <ias_kt> <vs_fpm> <engines_running 0|1>   (the last four since v0.4.0)
//   PEER_POS <sender_id>:<lat>:<lon>:<alt_ft>:<heading_deg>;... (Formation, dead-reckoned)
//     Both pushed about once a second, for the companion's map page.
//   APPROACH_COACH <0|1> (whether the Approach Coach is on)
//   APPROACHES <entry>;... (our last 20 rated approaches, oldest first)
//     <entry> = <id, unix time at the end>:<stable at 500 ft 0|1>:<deviations at 500 ft>:
//               <warnings below 500 ft>:<max sink below 500 ft, fpm>:<go-around 0|1>
//     deviations/warnings are sync/approach_coach.h's ApproachDeviation bits.
//   LANDINGS <entry>;... (the last 20 landings - ours and the session's, oldest first)
//     <entry> = <sender_id, 0 = ours>:<id, unix time>:<lat>:<lon>:<heading_true>:<vs_fpm>:<peak_g>:
//               <groundspeed_kt>:<drift_deg>:<bounces>:<flare_m, -1 = unknown>:<touch_and_go 0|1>:<icao|->:<callsign|->
//     See sync/touchdown_detector.h - the companion rates them against the runway.
//   SHARED_COCKPIT_AIRCRAFT_MISMATCH <own icao>:<master icao> (empty if matching/unknown)
//   SIM_READY <0|1>
//   PLUGIN_VERSION <version>
//   CSL_STATUS <"N model(s) loaded" | "error: ..."> (empty before XPMP2 init ran)
// SHARED_COCKPIT_OWNERSHIP reflects DatarefSync::Owns() for each category
// from this side's point of view (empty string before Shared Cockpit's
// dataref sync has actually started) - plugin_main.cpp pushes it whenever
// it changes, whether from this side's own CLAIM_OWNERSHIP (or touching a
// dataref directly) or from the peer claiming a category over the network.
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
constexpr size_t kMaxRoutePayload = 2000;

class ControlListener {
public:
    struct Callbacks {
        std::function<void(const std::string& hostPort, bool asSpectator)> on_create_session;
        std::function<void(const std::string& hostPort, const std::string& code, bool asSpectator)>
            on_join_session;
        std::function<void(bool isMaster, const std::string& serverHostPort, const std::string& code)>
            on_start_shared_cockpit;
        std::function<void(const std::string& hostPort, const std::string& code)> on_lan_connect_formation;
        std::function<void()> on_disconnect_formation;
        std::function<void()> on_disconnect_shared_cockpit;
        std::function<void(DatarefCategory)> on_claim_ownership;
        std::function<void()> on_reload_csl;
        std::function<void(const std::string& callsign, bool labels, bool envSync, bool rightSeat, bool directP2P,
                           bool debugLog)>
            on_set_prefs;
        std::function<void()> on_sync_env;
        std::function<void(const std::string& payload)> on_checklist_sync;
        std::function<void()> on_sc_resync;
        std::function<void(const std::string& list)> on_watch;
        std::function<void(bool start)> on_learn;
        std::function<void(const std::string& payload)> on_route_share; // "" = clear
        std::function<void(double seconds, const std::string& text)> on_show_overlay;
        std::function<void(bool enabled)> on_set_approach_coach;
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

    // The two states a category's ownership can be in from this side's
    // point of view - see the SHARED_COCKPIT_OWNERSHIP wire-format comment
    // above for what each one means to the companion app.
    enum class OwnershipUiState : uint8_t { kMe, kPeer };

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
    void SetCslStatus(const std::string& text);
    // `encoded` = "<callsign|-> <0|1> <0|1>", see PREFS above.
    void SetPrefs(const std::string& encoded);
    void SetOwnIcao(const std::string& icao);
    void SetTcasStatus(const std::string& status);
    void SetLearn(const std::string& state, const std::string& changes);
    void SetSharedRoute(const std::string& encoded);
    void SetWind(const std::string& encoded);
    void SetChecklistRemote(const std::string& payload);
    void SetScDesync(const std::string& encoded);
    void SetWatchValues(const std::string& encoded);
    // Both in one push (they're refreshed together once a second) - see
    // SELF_POS/PEER_POS above for the encodings.
    void SetPositions(const std::string& self, const std::string& peers);
    void SetLandings(const std::string& encoded);
    void SetApproachCoach(bool enabled);
    void SetApproaches(const std::string& encoded);

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
    std::string csl_status_;
    std::string prefs_;
    std::string own_icao_;
    std::string tcas_status_;
    std::string learn_state_ = "idle 0 0";
    std::string learn_changes_;
    std::string shared_route_;
    std::string wind_;
    std::string checklist_remote_;
    std::string sc_desync_;
    std::string watch_values_;
    std::string self_pos_;
    std::string peer_pos_;
    std::string landings_;
    bool approach_coach_ = true;
    std::string approaches_;
};

} // namespace flytogether
