// XPMultiCrew X-Plane plugin.
//
// Phase 0 (kept as-is, still runs): reads the aircraft's own position
// dataref, writes it to X-Plane's Log.txt, and separately sends it as a raw
// UDP packet to tools/udp_position_listener.
//
// Phase 1 (Formation mode LAN MVP, docs/plan.md section 5): reads a richer
// aircraft state (position, attitude, a few surfaces/lights, ICAO type) and
// exchanges it with an arbitrary number of peers (formation/peer_list.h)
// over UDP, tracking each one's dead-reckoned pose (sync/remote_aircraft.h)
// and drawing it via XPMP2 (TCAS override + XPLMInstance) using a small
// bundled generic CSL model (plugin/Resources/CSL/Generic) - no manual CSL
// download required for a first working version.
//
// Phase 2 (internet play, docs/plan.md section 7): a rendezvous/relay
// server (server/) lets Formation peers find each other and exchange state
// over the internet, not just LAN - see formation/rendezvous_client.h.
//
// Phase 3 (Shared Cockpit MVP, docs/plan.md section 6): one master
// broadcasts its own aircraft's position/attitude, which every client
// applies to its own aircraft by overriding X-Plane's physics
// (sim/operation/override/override_planepath) - see
// shared_cockpit/shared_cockpit_sync.h. A separate, symmetric "systems"
// sync (shared_cockpit/dataref_sync.h) mirrors an arbitrary configured list
// of switch/setting datarefs between both sides in either direction, since
// both peers fly an identical aircraft. No request-release ownership
// arbitration yet - last write wins.

#include "XPLMDataAccess.h"
#include "XPLMDefs.h"
#include "XPLMGraphics.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include "XPMPMultiplayer.h"

#include "control/control_listener.h"
#include "flytogether/position_packet.h"
#include "flytogether/udp_socket.h"
#include "formation/csl_aircraft.h"
#include "formation/formation_sync.h"
#include "formation/peer_list.h"
#include "formation/rendezvous_client.h"
#include "formation/rendezvous_protocol.h" // SplitHostPort, reused by the control listener
#include "net/session_crypto.h"
#include "shared_cockpit/dataref_sync.h"
#include "shared_cockpit/quaternion.h"
#include "shared_cockpit/shared_cockpit_config.h"
#include "shared_cockpit/shared_cockpit_sync.h"
#include "shared_cockpit/weather_sync.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Set by plugin/CMakeLists.txt via target_compile_definitions, derived
// from `git describe` - this fallback only matters if someone compiles
// this file outside that build (IDE tooling, a stray manual invocation).
#ifndef XPMULTICREW_VERSION
#define XPMULTICREW_VERSION "unknown"
#endif

namespace {

flytogether::ControlListener g_control_listener;

// --- Phase 0 spike state (unchanged) ----------------------------------------

XPLMDataRef g_latitude_ref = nullptr;
XPLMDataRef g_longitude_ref = nullptr;
XPLMDataRef g_elevation_ref = nullptr;

flytogether::UdpSocket g_udp_socket;
uint32_t g_sequence = 0;

float LogPositionCallback(float /*elapsedSinceLastCall*/,
                           float /*elapsedTimeSinceLastFlightLoop*/,
                           int /*counter*/,
                           void* /*refcon*/) {
    if (g_latitude_ref && g_longitude_ref && g_elevation_ref) {
        const double lat = XPLMGetDatad(g_latitude_ref);
        const double lon = XPLMGetDatad(g_longitude_ref);
        const double elev = XPLMGetDatad(g_elevation_ref);

        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "XPMultiCrew: lat=%.6f lon=%.6f elev=%.1fm\n", lat, lon,
                      elev);
        XPLMDebugString(buf);
    }

    return 5.0f; // reschedule in 5 seconds
}

float SendPositionOverUdpCallback(float /*elapsedSinceLastCall*/,
                                   float /*elapsedTimeSinceLastFlightLoop*/,
                                   int /*counter*/,
                                   void* /*refcon*/) {
    if (g_latitude_ref && g_longitude_ref && g_elevation_ref) {
        flytogether::PositionPacket packet;
        packet.sequence = g_sequence++;
        packet.latitude = XPLMGetDatad(g_latitude_ref);
        packet.longitude = XPLMGetDatad(g_longitude_ref);
        packet.elevation = XPLMGetDatad(g_elevation_ref);

        g_udp_socket.SendTo("127.0.0.1", flytogether::kSpikeUdpPort, &packet,
                             sizeof(packet));
    }

    return 0.2f; // 5 Hz, matches the plan's throttled position update rate
}

// --- Phase 1: Formation mode LAN MVP ----------------------------------------

XPLMDataRef g_heading_ref = nullptr;
XPLMDataRef g_pitch_ref = nullptr;
XPLMDataRef g_roll_ref = nullptr;
XPLMDataRef g_gear_ref = nullptr;      // sim/flightmodel2/gear/deploy_ratio, float[]
XPLMDataRef g_flap_ref = nullptr;      // sim/flightmodel/controls/flaprat, float
XPLMDataRef g_speedbrake_ref = nullptr; // sim/cockpit2/controls/speedbrake_ratio, float
XPLMDataRef g_engine_ref = nullptr;    // sim/flightmodel/engine/ENGN_thro, float[]
XPLMDataRef g_beacon_ref = nullptr;
XPLMDataRef g_strobe_ref = nullptr;
XPLMDataRef g_nav_ref = nullptr;
XPLMDataRef g_landing_ref = nullptr;
XPLMDataRef g_icao_ref = nullptr; // sim/aircraft/view/acf_ICAO

flytogether::FormationSync g_formation_sync;
uint32_t g_sender_id = 0;
uint32_t g_formation_sequence = 0;
// Refreshed by RefreshOwnIcaoType(), not read only once - see its comment
// for why a single XPluginStart-time read isn't enough.
char g_icao_type[9] = {}; // one extra byte so it's always null-terminated

bool g_xpmp_initialized = false;
std::unordered_map<uint32_t, std::unique_ptr<flytogether::RemoteAircraftXPMP>> g_xpmp_aircraft;
double g_last_formation_log_s = 0.0;
std::unordered_map<uint32_t, std::string> g_last_pushed_formation_peers; // for change detection only

// --- Phase 2: rendezvous/relay client (docs/plan.md section 7) -------------

flytogether::RendezvousClient g_rendezvous_client;
std::unordered_map<int, flytogether::Peer> g_rendezvous_peers; // peer_id -> addr, for RemovePeer on peer_left
bool g_rendezvous_active = false;
std::string g_formation_session_code;
int g_formation_own_peer_id = 0;

// The Formation session's encryption key, derived once from the session
// code + server-minted salt as soon as on_session_ready fires (both a
// fresh Create/Join and a reconnect rejoining the same session code/salt -
// see net/session_crypto.h's class comment for what the salt does and
// doesn't buy). Empty until then; std::optional rather than a nullable
// pointer to something heap-allocated, since a copy/move is cheap and
// this only ever needs one live instance at a time. Wired into
// g_formation_sync via SetCrypto() and used directly for Formation's
// relay path (see g_rendezvous_client.on_relay_received/
// SendFormationStateCallback below) - see FormationSync::SetCrypto's
// comment for why relay isn't handled inside that class itself.
std::optional<flytogether::SessionCrypto> g_formation_crypto;

// LAN-direct Formation: LAN_CONNECT_FORMATION (see control_listener.h)
// hands the plugin a peer's address directly, entirely bypassing
// g_rendezvous_client. Shares
// g_formation_crypto/g_formation_sync above with the rendezvous path (one
// Formation link is either rendezvous-based or LAN-direct, never both at
// once - see LanConnectFormation's comment); these three only track
// enough LAN-specific state to know which mode g_formation_crypto is
// currently in and which peers to RemovePeer() on disconnect.
bool g_lan_formation_active = false;
std::string g_lan_formation_code;
std::vector<flytogether::Peer> g_lan_formation_peers;

constexpr std::chrono::seconds kReconnectMinRetryInterval{5};
// Caps the backoff below - a co-pilot mid-flight shouldn't have to wait
// much longer than this between attempts even during a prolonged outage,
// but also shouldn't have this plugin hammering the rendezvous server
// every 5s for the whole duration of, say, a 20-minute server restart.
constexpr std::chrono::seconds kReconnectMaxRetryInterval{60};

// Wall-clock auto-reconnect gate, shared by Formation's and Shared
// Cockpit's independent rendezvous sessions (each gets its own instance -
// see g_formation_reconnect/g_shared_cockpit_reconnect below). `wanted` is
// true from the moment the user asks to connect (Create/Join Session, or
// the file-based auto-start) until they explicitly disconnect via the
// companion app - see control_listener.h's DISCONNECT_FORMATION/
// DISCONNECT_SHARED_COCKPIT. While true, RendezvousClient::on_disconnected
// (fired after kServerResponseTimeout of silence from the server) keeps
// getting retried via Due() below rather than giving up, so a transient
// network/server blip doesn't strand the user mid-flight.
struct ReconnectGate {
    bool wanted = false;
    std::chrono::steady_clock::time_point last_attempt{};
    // Doubles after every consecutive failed attempt (Due() firing again
    // without an intervening Reset()), capped at
    // kReconnectMaxRetryInterval - a short outage still gets a prompt
    // first retry at kReconnectMinRetryInterval, while a long one backs
    // off instead of retrying at a fixed 5s cadence indefinitely.
    std::chrono::seconds current_interval = kReconnectMinRetryInterval;

    // Resets the retry clock AND the backoff interval back down to
    // kReconnectMinRetryInterval - called whenever a fresh attempt is made
    // explicitly (Create/Join) or a session becomes ready again, so a
    // *future* disconnect starts backing off from the short interval
    // again instead of carrying over a long one from a previous, unrelated
    // outage.
    void Reset() {
        last_attempt = std::chrono::steady_clock::now();
        current_interval = kReconnectMinRetryInterval;
    }

    // True if a retry is due right now, in which case the retry clock is
    // reset (same as Reset(), but WITHOUT resetting current_interval -
    // this is what makes the backoff persist across consecutive misses)
    // and the interval is doubled for the next call, as a side effect, so
    // the caller's own retry counts as this interval's attempt. False if
    // not wanted, already in session, or simply not time yet.
    bool Due(bool in_session) {
        if (!wanted || in_session) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_attempt < current_interval) {
            return false;
        }
        last_attempt = now;
        current_interval = std::min(current_interval * 2, kReconnectMaxRetryInterval);
        return true;
    }
};

ReconnectGate g_formation_reconnect;
std::string g_formation_reconnect_host;
uint16_t g_formation_reconnect_port = 0;
// Only used for the very first attempt (before we have an assigned
// session code): whether that first attempt was create_session or
// join_session <user-typed code>. Every later reconnect always rejoins
// g_formation_reconnect_code instead, once on_session_ready has set it -
// re-running create_session on reconnect would hand back a *different*
// code the co-pilot doesn't know, defeating the point of a transparent
// reconnect.
bool g_formation_reconnect_create = false;
std::string g_formation_reconnect_code;
// Whether the current/most-recently-requested Formation session is
// spectating (see control_listener.h's CREATE_SESSION/JOIN_SESSION
// SPECTATOR token) - checked by SendFormationStateCallback to skip
// broadcasting this side's own position, and carried through
// MaybeReconnectFormation()'s rejoin so a dropped-and-restored connection
// doesn't silently start broadcasting a spectator again.
bool g_formation_is_spectator = false;

// Rebuilds and pushes the Formation status text, including how many
// peers are currently in the session - called from on_peer_joined and
// on_peer_left too (not just on_session_ready), so the companion app
// actually shows when someone joins or leaves instead of only ever
// showing the state as of when *you* connected. That gap is exactly what
// silently connecting someone else's aircraft into FormationSync without
// updating this string used to hide.
void UpdateFormationStatus() {
    char status[160];
    std::snprintf(status, sizeof(status), "connected, code '%s', peer %d - %zu peer(s) online",
                  g_formation_session_code.c_str(), g_formation_own_peer_id, g_rendezvous_peers.size());
    g_control_listener.SetFormationStatus(status);
}

// Wires up g_rendezvous_client's callbacks exactly once - called from
// StartRendezvous() below, itself only ever triggered by the companion
// app's "Create Session"/"Join Session" requests.
void SetupRendezvousCallbacksOnce() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    g_rendezvous_client.on_session_ready = [](const std::string& code, int your_id,
                                               const std::vector<uint8_t>& salt) {
        // Derive (or re-derive, on a reconnect - see net/session_crypto.h)
        // this session's encryption key before anything else below, since
        // Formation traffic must never go out unencrypted once a session
        // exists - see docs/plan.md's Session-Auth/Verschlüsselung. A
        // wrong-sized salt means the server isn't speaking this protocol
        // version correctly despite the client_version check having
        // already passed (a server bug, not something a retry fixes) -
        // fail loudly and don't proceed, rather than silently falling
        // back to plaintext.
        if (salt.size() != flytogether::SessionCrypto::kSaltSize) {
            char err_buf[160];
            std::snprintf(err_buf, sizeof(err_buf),
                          "XPMultiCrew: rendezvous server sent a malformed session salt (%zu "
                          "bytes, expected %zu) - refusing to proceed insecurely\n",
                          salt.size(), flytogether::SessionCrypto::kSaltSize);
            XPLMDebugString(err_buf);
            g_control_listener.SetFormationStatus("error: server sent an invalid session salt");
            return;
        }
        std::array<uint8_t, flytogether::SessionCrypto::kSaltSize> salt_array{};
        std::copy(salt.begin(), salt.end(), salt_array.begin());
        g_formation_crypto.emplace(code, salt_array);
        g_formation_sync.SetCrypto(&*g_formation_crypto);

        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "XPMultiCrew: rendezvous session ready - code '%s', you are peer "
                      "%d. Share the code with your co-pilot(s).\n",
                      code.c_str(), your_id);
        XPLMDebugString(buf);

        // A successful (re)connect - whether this is the very first one or
        // an auto-reconnect after an outage - means the link is healthy
        // again, so any backoff MaybeReconnectFormation() built up during
        // that outage no longer applies to whatever happens next. Without
        // this, a short second outage shortly after a long first one would
        // start retrying at the first outage's stretched-out interval
        // instead of ReconnectGate's normal quick first retry.
        g_formation_reconnect.Reset();
        g_formation_session_code = code;
        g_formation_own_peer_id = your_id;
        // Remember the server-assigned code so a future reconnect (see
        // on_disconnected below) rejoins this exact session instead of
        // creating a brand new one - matters whether we got here via
        // create_session (we had no code before) or join_session (the
        // user may have retyped a stale one).
        g_formation_reconnect_code = code;
        g_formation_reconnect_create = false;
        UpdateFormationStatus();
        g_control_listener.SetFormationCode(code);
    };
    g_rendezvous_client.on_peer_joined = [](int peer_id, const std::string& host, uint16_t port) {
        g_rendezvous_peers[peer_id] = flytogether::Peer{host, port};
        g_formation_sync.AddPeer(host, port);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "XPMultiCrew: rendezvous peer %d joined at %s:%u\n", peer_id,
                      host.c_str(), port);
        XPLMDebugString(buf);
        UpdateFormationStatus();
    };
    g_rendezvous_client.on_peer_left = [](int peer_id) {
        const auto it = g_rendezvous_peers.find(peer_id);
        if (it != g_rendezvous_peers.end()) {
            g_formation_sync.RemovePeer(it->second.host, it->second.port);
            g_rendezvous_peers.erase(it);
        }
        UpdateFormationStatus();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "XPMultiCrew: rendezvous peer %d left\n", peer_id);
        XPLMDebugString(buf);
    };
    g_rendezvous_client.on_relay_received = [](int /*from_peer_id*/,
                                                 const std::vector<uint8_t>& bytes) {
        // Decrypted first (see FormationSync::SetCrypto's comment for why
        // Formation's relay path is handled here rather than inside that
        // class) - dropped silently on failure, same as a malformed/
        // wrong-magic direct-UDP packet. Not yet in a session at all
        // (crypto unset) is itself impossible here: on_relay_received can
        // only ever fire after on_session_ready already set it.
        if (!g_formation_crypto) {
            return;
        }
        const auto opened = g_formation_crypto->Open(bytes);
        if (!opened) {
            return;
        }
        // `<` against the frozen size floor, not exact-match, and copy
        // only min(opened->size(), sizeof(packet)) - same forward-
        // compatibility reasoning as FormationSync::PollIncoming's direct-
        // UDP receive.
        if (opened->size() < flytogether::kAircraftStateMinSize) {
            return;
        }
        flytogether::AircraftStatePacket packet;
        std::memcpy(&packet, opened->data(), std::min(opened->size(), sizeof(packet)));
        g_formation_sync.IngestPacket(packet, XPLMGetElapsedTime());
    };
    g_rendezvous_client.on_error = [](const std::string& message) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "XPMultiCrew: rendezvous error: %s\n", message.c_str());
        XPLMDebugString(buf);
        g_control_listener.SetFormationStatus("error: " + message);
    };
    g_rendezvous_client.on_disconnected = []() {
        XPLMDebugString("XPMultiCrew: rendezvous connection lost\n");
        for (const auto& [peer_id, peer] : g_rendezvous_peers) {
            g_formation_sync.RemovePeer(peer.host, peer.port);
        }
        g_rendezvous_peers.clear();
        g_last_pushed_formation_peers.clear();
        g_control_listener.SetFormationPeers("");
        // g_formation_reconnect.wanted stays true here (unless the user
        // explicitly disconnected, in which case Stop() already made this
        // moot) - UpdateFormationCallback's reconnect scheduler picks this
        // up and keeps retrying on its own.
        g_control_listener.SetFormationStatus(g_formation_reconnect.wanted ? "connection lost, reconnecting..."
                                                                            : "not connected");
    };
}

// Callable from the companion app's "Create Session"/"Join Session"
// requests. Formation used to also support a file-based auto-start
// (XPMultiCrew_rendezvous.txt, evaluated once in XPluginEnable) - removed
// since it could only ever run before the flight loop (and thus the
// connection's keepalive) was even active, which is exactly the
// loading-screen keepalive-timing bug server/session.go's clientTimeout
// comment describes; a button click always happens well after that.
// Tears down LAN-direct Formation state (see LanConnectFormation below) -
// shared by DisconnectFormation() and StartRendezvous(), since starting a
// rendezvous session while LAN peers are active would otherwise silently
// reassign g_formation_crypto out from under them (same
// use-after-free-shaped hazard SetCrypto(nullptr)-before-reset already
// guards against for the rendezvous path - see DisconnectFormation's
// comment).
void ClearLanFormation() {
    if (!g_lan_formation_active) {
        return;
    }
    for (const auto& peer : g_lan_formation_peers) {
        g_formation_sync.RemovePeer(peer.host, peer.port);
    }
    g_lan_formation_peers.clear();
    g_lan_formation_active = false;
    g_lan_formation_code.clear();
    g_formation_sync.SetCrypto(nullptr);
    g_formation_crypto.reset();
}

void StartRendezvous(const std::string& host, uint16_t port, bool create, const std::string& code,
                      bool asSpectator) {
    SetupRendezvousCallbacksOnce();
    ClearLanFormation(); // a rendezvous session always takes over the Formation link, see its comment

    g_formation_reconnect.wanted = true;
    g_formation_reconnect_host = host;
    g_formation_reconnect_port = port;
    g_formation_reconnect_create = create;
    g_formation_reconnect_code = code;
    g_formation_is_spectator = asSpectator;
    // Resets the retry clock and backoff so UpdateFormationCallback's
    // scheduler waits a full kReconnectMinRetryInterval before its first
    // attempt, instead of potentially firing a redundant duplicate
    // CreateSession/JoinSession moments after this one while still waiting
    // on session_ready.
    g_formation_reconnect.Reset();

    if (g_rendezvous_active) {
        // Always allow a fresh attempt rather than getting permanently
        // stuck: a previous attempt that opened its socket fine but then
        // silently failed to actually reach the server (e.g. a DNS
        // resolution error, now surfaced via on_error instead of being
        // silent) would otherwise block every future click forever, since
        // this flag used to only get set on success and never reset short
        // of a full plugin disable/enable.
        XPLMDebugString("XPMultiCrew: rendezvous already active, reconnecting fresh\n");
        g_rendezvous_client.Stop();
        g_rendezvous_peers.clear();
        g_rendezvous_active = false;
    }
    if (!g_rendezvous_client.Start(host, port)) {
        XPLMDebugString("XPMultiCrew: failed to start rendezvous client (UDP socket?)\n");
        g_control_listener.SetFormationStatus("failed to start (UDP socket busy?)");
        return;
    }
    g_rendezvous_active = true;
    g_control_listener.SetFormationCode(""); // clear any stale code from a previous session
    g_control_listener.SetFormationStatus("connecting to " + host + ":" + std::to_string(port) + "...");
    if (create) {
        g_rendezvous_client.CreateSession(asSpectator);
    } else {
        g_rendezvous_client.JoinSession(code, asSpectator);
    }
}

// Companion app's explicit "Disconnect" - the only thing that actually
// stops StartRendezvous()/on_disconnected's auto-reconnect (see
// g_formation_reconnect's comment) instead of just losing the
// connection and trying again.
void DisconnectFormation() {
    g_formation_reconnect.wanted = false;
    for (const auto& [peer_id, peer] : g_rendezvous_peers) {
        g_formation_sync.RemovePeer(peer.host, peer.port);
    }
    g_rendezvous_peers.clear();
    g_last_pushed_formation_peers.clear();
    g_rendezvous_client.Stop(); // sends leave_session if we were actually in one
    g_rendezvous_active = false;
    g_formation_session_code.clear();
    g_formation_own_peer_id = 0;
    // Clear FormationSync's pointer BEFORE destroying what it points to -
    // g_formation_crypto.reset() below would otherwise leave it dangling
    // until the next session's on_session_ready overwrites it (harmless
    // in practice, since peers_ is also empty by now so SendOwnState's
    // loop body never runs, but Seal() is still called unconditionally
    // ahead of that loop - see FormationSync::SendOwnState - so a stray
    // send between here and a future SetCrypto() call would be a real
    // use-after-free, not just a logic bug).
    g_formation_sync.SetCrypto(nullptr);
    g_formation_crypto.reset();
    ClearLanFormation(); // no-op if the current/previous Formation link wasn't LAN-direct
    g_control_listener.SetFormationCode("");
    g_control_listener.SetFormationPeers("");
    g_control_listener.SetFormationStatus("not connected");
}

// LAN_CONNECT_FORMATION's handler - see control_listener.h's wire-format
// comment. Unlike StartRendezvous(), this never touches
// g_rendezvous_client/g_formation_reconnect at all: there's no session to
// (re)join, just a peer address to start sending direct UDP to. That also
// means a LAN peer going quiet is NOT auto-retried (MaybeReconnectFormation
// only ever drives the rendezvous path) - accepted v1 scope, see
// docs/plan.md's LAN-Auto-Discovery open items; re-issuing
// LAN_CONNECT_FORMATION is the way back in.
void LanConnectFormation(const std::string& hostPort, const std::string& code) {
    if (g_rendezvous_active) {
        g_control_listener.SetFormationStatus(
            "formation already connected via a rendezvous server - disconnect first");
        return;
    }
    if (code.empty()) {
        g_control_listener.SetFormationStatus("LAN connect needs a session code");
        return;
    }
    std::string host;
    uint16_t port = 0;
    if (!flytogether::SplitHostPort(hostPort, host, port)) {
        g_control_listener.SetFormationStatus("invalid LAN peer address (need host:port)");
        return;
    }
    if (g_lan_formation_active && g_lan_formation_code != code) {
        g_control_listener.SetFormationStatus(
            "LAN formation already active with a different code - disconnect first");
        return;
    }
    if (!g_lan_formation_active) {
        g_formation_crypto.emplace(code); // code-only derivation - see SessionCrypto's LAN constructor
        g_formation_sync.SetCrypto(&*g_formation_crypto);
        g_lan_formation_active = true;
        g_lan_formation_code = code;
        g_control_listener.SetFormationCode(code);
    }
    // Adding the same peer twice is a harmless no-op (FormationSync::AddPeer),
    // but only track it once here too so ClearLanFormation() doesn't issue a
    // redundant RemovePeer.
    const bool already_tracked = std::any_of(
        g_lan_formation_peers.begin(), g_lan_formation_peers.end(),
        [&](const flytogether::Peer& p) { return p.host == host && p.port == port; });
    if (!already_tracked) {
        g_lan_formation_peers.push_back(flytogether::Peer{host, port});
    }
    g_formation_sync.AddPeer(host, port);
    g_control_listener.SetFormationStatus("connected (LAN), " + std::to_string(g_lan_formation_peers.size()) +
                                           " peer(s)");
}

// Reconnect scheduler for both rendezvous sessions - if the user wants to
// be connected (g_formation_reconnect.wanted/g_shared_cockpit_reconnect.wanted)
// but currently isn't (RendezvousClient::InSession() false, e.g. right
// after on_disconnected fired), retry using a wall-clock timer for the
// same reason MaybeSendKeepalive() does (so a long sim pause/loading
// screen doesn't desync the retry cadence), starting at
// kReconnectMinRetryInterval and backing off toward
// kReconnectMaxRetryInterval the longer the outage lasts - see
// ReconnectGate's comment.
// Called from both UpdateFormationCallback and
// PollSharedCockpitRendezvousCallback, which already run every frame
// unconditionally.
void MaybeReconnectFormation() {
    if (!g_formation_reconnect.Due(g_rendezvous_client.InSession())) {
        return;
    }
    XPLMDebugString("XPMultiCrew: attempting to reconnect the rendezvous session...\n");
    // Rejoin the exact same session once we have an assigned code (see
    // on_session_ready) rather than re-running create_session, which
    // would hand back a different code our co-pilot doesn't know.
    if (g_formation_reconnect_create && g_formation_reconnect_code.empty()) {
        g_rendezvous_client.CreateSession(g_formation_is_spectator);
    } else {
        g_rendezvous_client.JoinSession(g_formation_reconnect_code, g_formation_is_spectator);
    }
}

// Returns this plugin's own "Resources" folder, e.g.
// ".../Resources/plugins/XPMultiCrew/Resources", derived from the running
// plugin's own file path so no hardcoded install location is needed.
std::string GetPluginResourcesPath() {
    char path_buf[512] = {};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path_buf, nullptr, nullptr);
    std::string path(path_buf);
    const auto sep_pos = path.find_last_of("/\\");
    if (sep_pos != std::string::npos) {
        path.erase(sep_pos);
    }
    return path + "/Resources";
}

// Returns "<X-Plane>/Resources/plugins" - the direct parent of this
// plugin's own install folder - derived from own_resources_path (as
// returned by GetPluginResourcesPath() above, ".../plugins/XPMultiCrew/
// Resources") by stripping two path components. Used only to check the
// short, explicit allow-list below (kKnownSharedCslDirs), never as a
// general recursive search root - see that constant's comment for why.
std::string GetXPlanePluginsRootPath(const std::string& own_resources_path) {
    std::string path = own_resources_path;
    for (int i = 0; i < 2; ++i) {
        const auto sep_pos = path.find_last_of("/\\");
        if (sep_pos == std::string::npos) {
            break;
        }
        path.erase(sep_pos);
    }
    return path;
}

// Other tools (ATC clients, traffic add-ons) sometimes install a CSL
// package under a well-known shared folder name directly inside
// <X-Plane>/Resources/plugins/, rather than under their own plugin's
// Resources/ - IVAO_CSL (used by several IVAO-network clients) is the
// common one. Checking these explicitly, by name, lets a user reuse a
// package they've already installed for another tool instead of
// duplicating it under our own Resources/CSL - without recursively
// scanning the whole plugins/ folder, which would be slower (many
// installed plugins ship large, unrelated resource trees) and would
// silently pull in whatever CSL packages other tools bundle - including
// their licensing terms - without the user consciously choosing to share
// them with this plugin specifically.
const std::vector<std::string> kKnownSharedCslDirs = {
    "IVAO_CSL",
};

// XPMPMultiplayerInit + loading our own Resources/CSL and every
// kKnownSharedCslDirs entry that's installed. Shared by XPluginStart and
// ReloadCsl below. Returns the text pushed as CSL_STATUS (see
// control_listener.h) - "error: ..." only if XPMP2 itself failed to
// initialize; a single package failing to load is logged but doesn't stop
// the others.
std::string InitXpmpAndLoadCsl() {
    const std::string resources_path = GetPluginResourcesPath();
    const std::string xpmp2_resources = resources_path + "/XPMP2";
    const std::string csl_path = resources_path + "/CSL";

    const char* xpmp_err = XPMPMultiplayerInit("XPMultiCrew", xpmp2_resources.c_str(),
                                                nullptr, "GENR", "XPMultiCrew");
    if (xpmp_err && xpmp_err[0]) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "XPMultiCrew: XPMPMultiplayerInit failed: %s\n", xpmp_err);
        XPLMDebugString(buf);
        return std::string("error: XPMP2 init failed: ") + xpmp_err;
    }
    g_xpmp_initialized = true;

    const char* csl_err = XPMPLoadCSLPackage(csl_path.c_str());
    if (csl_err && csl_err[0]) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "XPMultiCrew: XPMPLoadCSLPackage(%s) failed: %s\n",
                      csl_path.c_str(), csl_err);
        XPLMDebugString(buf);
    }

    const std::string plugins_root = GetXPlanePluginsRootPath(resources_path);
    for (const auto& shared_dir_name : kKnownSharedCslDirs) {
        const std::string shared_path = plugins_root + "/" + shared_dir_name;
        std::error_code ec;
        if (!std::filesystem::is_directory(shared_path, ec) || ec) {
            continue; // not installed - not an error, just nothing to load
        }
        const char* shared_err = XPMPLoadCSLPackage(shared_path.c_str());
        char buf[512];
        if (shared_err && shared_err[0]) {
            std::snprintf(buf, sizeof(buf), "XPMultiCrew: XPMPLoadCSLPackage(%s) failed: %s\n",
                          shared_path.c_str(), shared_err);
        } else {
            std::snprintf(buf, sizeof(buf), "XPMultiCrew: loaded shared CSL package from %s\n",
                          shared_path.c_str());
        }
        XPLMDebugString(buf);
    }

    const int model_count = XPMPGetNumberOfInstalledModels();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "XPMultiCrew: %d CSL model(s) loaded\n", model_count);
    XPLMDebugString(buf);
    return std::to_string(model_count) + " model(s) loaded";
}

// RELOAD_CSL's handler (see control_listener.h) - picks up CSL packages
// dropped into Resources/CSL (or an edited xsb_aircraft.txt, e.g. a
// retuned VERT_OFFSET) without restarting X-Plane. XPMP2 has no API to
// unload or replace already-loaded models (CSLModelsAdd silently keeps
// the first definition of a duplicate key), so the only clean reload is a
// full XPMPMultiplayerCleanup/Init cycle. Every XPMP2::Aircraft has to be
// destroyed before that cleanup - UpdateFormationCallback recreates them
// on its next frame for every peer that's still active, so remote
// aircraft only blink out briefly.
void ReloadCsl() {
    XPLMDebugString("XPMultiCrew: reloading CSL models...\n");

    g_xpmp_aircraft.clear();
    if (g_xpmp_initialized) {
        XPMPMultiplayerCleanup();
        g_xpmp_initialized = false;
    }

    const std::string status = InitXpmpAndLoadCsl();
    if (g_xpmp_initialized) {
        const char* err = XPMPMultiplayerEnable();
        if (err && err[0]) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "XPMultiCrew: XPMPMultiplayerEnable failed: %s\n", err);
            XPLMDebugString(buf);
        }
    }
    g_control_listener.SetCslStatus(status);
}

// Reads the first element of a float-array dataref, or 0 if the dataref
// wasn't found (e.g. name changed in a future X-Plane version - see the
// dataref sources noted in docs/plan.md's handoff prompt discussion).
float ReadFirstArrayElement(XPLMDataRef ref) {
    if (!ref) return 0.0f;
    float value = 0.0f;
    XPLMGetDatavf(ref, &value, 0, 1);
    return value;
}

uint8_t ReadLightBits() {
    uint8_t bits = 0;
    if (g_beacon_ref && XPLMGetDataf(g_beacon_ref) > 0.5f) bits |= flytogether::LightBits::kBeacon;
    if (g_strobe_ref && XPLMGetDataf(g_strobe_ref) > 0.5f) bits |= flytogether::LightBits::kStrobe;
    if (g_nav_ref && XPLMGetDataf(g_nav_ref) > 0.5f) bits |= flytogether::LightBits::kNav;
    if (g_landing_ref && XPLMGetDataf(g_landing_ref) > 0.5f) bits |= flytogether::LightBits::kLanding;
    return bits;
}

// Re-reads g_icao_type from sim/aircraft/view/acf_ICAO. Called from
// XPluginStart() as a best-effort first attempt, but that alone is not
// enough: X-Plane loads plugins (calling XPluginStart) before it loads
// the default aircraft (see control_listener.h's SIM_READY comment for
// the same ordering issue affecting a different dataref), so acf_ICAO can
// still be empty at that point - every peer would then broadcast an empty
// icao_type for its entire session, showing up as an unrecognized/no
// aircraft type on every other peer's side. Also called on every
// XPLM_MSG_PLANE_LOADED for the user's own aircraft (see
// XPluginReceiveMessage), which both fixes that cold-start gap and keeps
// this correct if the user changes aircraft mid-session - neither of
// which a single XPluginStart-time read could ever catch.
void RefreshOwnIcaoType() {
    if (!g_icao_ref) {
        return;
    }
    XPLMGetDatab(g_icao_ref, g_icao_type, 0, sizeof(g_icao_type) - 1);
    g_icao_type[sizeof(g_icao_type) - 1] = '\0';
}

// Reads all the datarefs both Formation mode (sending "here's another
// aircraft") and Shared Cockpit's master (sending "here's the aircraft
// we're both in") need. `sequence` is passed in/incremented by the caller
// since each use has its own independent counter.
flytogether::AircraftStatePacket BuildOwnAircraftStatePacket(uint32_t sender_id, uint32_t sequence) {
    flytogether::AircraftStatePacket packet;
    packet.sender_id = sender_id;
    packet.sequence = sequence;

    packet.latitude = g_latitude_ref ? XPLMGetDatad(g_latitude_ref) : 0.0;
    packet.longitude = g_longitude_ref ? XPLMGetDatad(g_longitude_ref) : 0.0;
    packet.elevation_m = g_elevation_ref ? XPLMGetDatad(g_elevation_ref) : 0.0;

    packet.heading_deg = g_heading_ref ? XPLMGetDataf(g_heading_ref) : 0.0f;
    packet.pitch_deg = g_pitch_ref ? XPLMGetDataf(g_pitch_ref) : 0.0f;
    packet.roll_deg = g_roll_ref ? XPLMGetDataf(g_roll_ref) : 0.0f;

    packet.gear_ratio = ReadFirstArrayElement(g_gear_ref);
    packet.flap_ratio = g_flap_ref ? XPLMGetDataf(g_flap_ref) : 0.0f;
    packet.speedbrake_ratio = g_speedbrake_ref ? XPLMGetDataf(g_speedbrake_ref) : 0.0f;
    packet.engine_ratio = ReadFirstArrayElement(g_engine_ref);
    packet.light_bits = ReadLightBits();

    std::memcpy(packet.icao_type, g_icao_type, sizeof(packet.icao_type));
    return packet;
}

float SendFormationStateCallback(float /*elapsedSinceLastCall*/,
                                  float /*elapsedTimeSinceLastFlightLoop*/,
                                  int /*counter*/,
                                  void* /*refcon*/) {
    // A spectator (see control_listener.h's CREATE_SESSION/JOIN_SESSION
    // SPECTATOR token) never broadcasts its own position - it's here to
    // watch, not to be watched - but PollIncoming/CSL rendering
    // (UpdateFormationCallback) are untouched, so it still sees and draws
    // everyone else normally.
    if (g_latitude_ref && g_longitude_ref && g_elevation_ref && !g_formation_is_spectator) {
        const flytogether::AircraftStatePacket packet =
            BuildOwnAircraftStatePacket(g_sender_id, g_formation_sequence++);

        g_formation_sync.SendOwnState(packet);
        // Also relay through the rendezvous server whenever we're in a
        // session, in parallel with the direct sends above. This is a
        // deliberate simplification vs. detecting hole-punch success/
        // failure per peer (docs/plan.md section 7): RemoteAircraft's
        // sequence-number dedup (sync/remote_aircraft.cpp) already makes
        // receiving the same state twice harmless, and the bandwidth cost
        // at this packet size/rate is negligible. Sealed with the same
        // key as the direct sends above (FormationSync::SendOwnState) -
        // see FormationSync::SetCrypto's comment for why the relay path
        // is encrypted here rather than inside that class. No-op (nothing
        // to relay) if we're not actually in a session yet.
        if (g_formation_crypto) {
            const auto envelope =
                g_formation_crypto->Seal(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
            g_rendezvous_client.SendRelay(envelope.data(), envelope.size());
        }
    }

    // 20 Hz, the upper end of the plan's recommended 10-20 Hz position rate.
    return 1.0f / 20.0f;
}

// Runs every frame: feeds incoming network packets into FormationSync,
// keeps one XPMP2 aircraft per currently-active peer (created/destroyed as
// peers appear/go stale), and pushes each one's freshly dead-reckoned pose
// so XPMP2's own per-frame UpdatePosition() has current data to draw.
float UpdateFormationCallback(float /*elapsedSinceLastCall*/,
                               float /*elapsedTimeSinceLastFlightLoop*/,
                               int /*counter*/,
                               void* /*refcon*/) {
    const double now = XPLMGetElapsedTime();
    g_formation_sync.PollIncoming(now);
    g_formation_sync.RemoveStaleAircraft(now);

    // Also drains/dispatches rendezvous messages and sends its own
    // wall-clock-timed keepalive if we're in a session - see
    // RendezvousClient::PollIncoming()'s comment for why that timer isn't
    // driven by X-Plane's (pausable) sim-elapsed time.
    g_rendezvous_client.PollIncoming();
    MaybeReconnectFormation();

    std::unordered_map<uint32_t, std::string> active;
    g_formation_sync.ForEachRemoteAircraft(
        now, [&](uint32_t sender_id, const flytogether::AircraftPose& /*pose*/,
                 const flytogether::AircraftStatePacket& latest) {
            active.emplace(sender_id,
                            std::string(latest.icao_type,
                                        strnlen(latest.icao_type, sizeof(latest.icao_type))));
        });

    // Push the peer list to the companion app (for its "connected peers"
    // list) only when it actually changed - this runs every frame, and
    // SetFormationPeers() sends a UDP packet, so unconditionally calling
    // it here would flood the socket for no reason.
    if (active != g_last_pushed_formation_peers) {
        g_last_pushed_formation_peers = active;
        std::string encoded;
        for (const auto& [sender_id, icao] : active) {
            if (!encoded.empty()) {
                encoded += ";";
            }
            encoded += std::to_string(sender_id) + ":" + icao;
        }
        g_control_listener.SetFormationPeers(encoded);
    }

    // Drop XPMP2 aircraft for peers that are no longer active.
    for (auto it = g_xpmp_aircraft.begin(); it != g_xpmp_aircraft.end();) {
        if (active.find(it->first) == active.end()) {
            it = g_xpmp_aircraft.erase(it);
        } else {
            ++it;
        }
    }

    // Create XPMP2 aircraft for newly-seen peers (not while XPMP2 is down,
    // e.g. after a failed ReloadCsl - there'd be nothing to draw them with).
    for (const auto& [sender_id, icao] : active) {
        if (g_xpmp_initialized && g_xpmp_aircraft.find(sender_id) == g_xpmp_aircraft.end()) {
            try {
                g_xpmp_aircraft.emplace(
                    sender_id, std::make_unique<flytogether::RemoteAircraftXPMP>(icao, sender_id));
            } catch (const std::exception& e) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "XPMultiCrew: failed to create CSL aircraft for peer %u: %s\n",
                              sender_id, e.what());
                XPLMDebugString(buf);
            }
        }
    }

    // Push this frame's dead-reckoned pose into each aircraft still tracked.
    g_formation_sync.ForEachRemoteAircraft(
        now, [](uint32_t sender_id, const flytogether::AircraftPose& pose,
                const flytogether::AircraftStatePacket& /*latest*/) {
            const auto it = g_xpmp_aircraft.find(sender_id);
            if (it != g_xpmp_aircraft.end()) {
                it->second->SetPose(pose);
            }
        });

    if (now - g_last_formation_log_s >= 1.0) {
        g_last_formation_log_s = now;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "XPMultiCrew[formation]: tracking %zu peer(s), %zu drawn via CSL\n",
                      g_formation_sync.tracked_aircraft_count(), g_xpmp_aircraft.size());
        XPLMDebugString(buf);
    }

    return -1.0f; // reschedule for next frame
}

// --- Phase 3: Shared Cockpit MVP (docs/plan.md section 6) -------------------
//
// Master/client position+attitude takeover only, plus generic bidirectional
// "systems" dataref sync (shared_cockpit/dataref_sync.h) - see the
// discussion that led here: no per-aircraft mapping file needed since both
// peers fly an identical aircraft, so watched dataref names resolve to the
// same thing on both sides. Ownership of individual dataref categories
// (engine/avionics/systems) is claim-and-tell: touching a switch claims
// its category immediately, no permission step - see
// shared_cockpit/ownership_tracker.h's class comment.

XPLMDataRef g_override_planepath_ref = nullptr; // int[20], index 0 = own aircraft
XPLMDataRef g_local_x_ref = nullptr;
XPLMDataRef g_local_y_ref = nullptr;
XPLMDataRef g_local_z_ref = nullptr;
XPLMDataRef g_quaternion_ref = nullptr; // float[4]
XPLMDataRef g_local_vx_ref = nullptr;
XPLMDataRef g_local_vy_ref = nullptr;
XPLMDataRef g_local_vz_ref = nullptr;

flytogether::SharedCockpitSync g_shared_cockpit;
flytogether::DatarefSync g_dataref_sync;
flytogether::WeatherSync g_weather_sync;
uint32_t g_shared_cockpit_sequence = 0;
bool g_physics_override_active = false;
bool g_shared_cockpit_active = false;

// Last ownership state actually pushed to the companion app via
// SetSharedCockpitOwnership, so PushSharedCockpitOwnershipIfChanged() (see
// below) only sends an update when something actually changed - keeps the
// common "nothing changed this tick" case a cheap no-op instead of an
// unconditional UDP send every 100ms from PollDatarefSyncCallback.
std::array<flytogether::ControlListener::OwnershipUiState, flytogether::kDatarefCategoryCount>
    g_last_pushed_ownership{};
bool g_has_pushed_ownership = false;

// Last SHARED_COCKPIT_AIRCRAFT_MISMATCH value actually pushed, same
// change-detection reasoning as g_last_pushed_ownership above - see
// PushAircraftMismatchIfChanged() in UpdateSharedCockpitCallback.
std::string g_last_pushed_aircraft_mismatch;

// Called right after a local ClaimOwnership() (so the companion app's
// buttons reflect the change immediately) and every PollDatarefSyncCallback
// tick (so it also reflects the peer claiming a category over the network,
// or this side auto-claiming one by touching a dataref directly - neither
// has any other local trigger point).
void PushSharedCockpitOwnershipIfChanged() {
    using flytogether::ControlListener;
    std::array<ControlListener::OwnershipUiState, flytogether::kDatarefCategoryCount> current{};
    for (int i = 0; i < flytogether::kDatarefCategoryCount; ++i) {
        const auto category = static_cast<flytogether::DatarefCategory>(i);
        current[i] = g_dataref_sync.Owns(category) ? ControlListener::OwnershipUiState::kMe
                                                    : ControlListener::OwnershipUiState::kPeer;
    }
    if (g_has_pushed_ownership && current == g_last_pushed_ownership) {
        return;
    }
    g_last_pushed_ownership = current;
    g_has_pushed_ownership = true;
    g_control_listener.SetSharedCockpitOwnership(current);
}

// Shared Cockpit's own rendezvous session, separate from Formation's
// g_rendezvous_client above (see control_listener.h's updated protocol
// comment) - peer discovery goes through the same rendezvous/relay server,
// but keeping a dedicated client/session means a Formation session and a
// Shared Cockpit session never share one code space or one relay stream
// (mixing AircraftStatePacket-for-Formation and
// AircraftStatePacket/DatarefSyncMessage-for-Shared-Cockpit payloads on
// one relay channel would need a type discriminator neither protocol has).
flytogether::RendezvousClient g_shared_cockpit_rendezvous;
bool g_shared_cockpit_rendezvous_active = false;

// Shared Cockpit's own encryption key - same idea as g_formation_crypto
// above, separate session/key. Wired into g_shared_cockpit and
// g_dataref_sync via SetCrypto() once derived (see
// SetupSharedCockpitRendezvousCallbacksOnce's on_session_ready below).
std::optional<flytogether::SessionCrypto> g_shared_cockpit_crypto;

// Auto-reconnect, mirroring g_formation_reconnect's comment above - same
// idea, separate session.
ReconnectGate g_shared_cockpit_reconnect;
std::string g_shared_cockpit_reconnect_host;
uint16_t g_shared_cockpit_reconnect_port = 0;
flytogether::SharedCockpitRole g_shared_cockpit_reconnect_role = flytogether::SharedCockpitRole::kNone;
std::string g_shared_cockpit_reconnect_code; // updated on_session_ready, same reasoning as Formation's
flytogether::SharedCockpitRole g_pending_shared_cockpit_role = flytogether::SharedCockpitRole::kNone;
std::vector<flytogether::DatarefSyncSpec> g_pending_shared_cockpit_datarefs;

// Client only: take over the user's own aircraft physics
// (sim/operation/override/override_planepath[0]) so it can be positioned
// directly instead of flying on its own local flight model. Only touches
// index 0 (the user's own aircraft) - see
// https://developer.x-plane.com/article/movingtheplane/'s explicit warning
// not to (ab)use this for AI aircraft, which have their own API
// (XPLMDisableAIForPlane, not used here since Formation mode draws other
// aircraft via XPMP2/TCAS override instead, not X-Plane's AI slots).
void SetPhysicsOverride(bool enabled) {
    if (!g_override_planepath_ref) {
        return;
    }
    int value = enabled ? 1 : 0;
    XPLMSetDatavi(g_override_planepath_ref, &value, 0, 1);
    g_physics_override_active = enabled;
}

void ApplyMasterPoseToOwnAircraft(const flytogether::AircraftPose& pose) {
    if (!g_physics_override_active) {
        SetPhysicsOverride(true);
    }

    double local_x = 0.0, local_y = 0.0, local_z = 0.0;
    XPLMWorldToLocal(pose.latitude, pose.longitude, pose.elevation_m, &local_x, &local_y, &local_z);

    if (g_local_x_ref) XPLMSetDatad(g_local_x_ref, local_x);
    if (g_local_y_ref) XPLMSetDatad(g_local_y_ref, local_y);
    if (g_local_z_ref) XPLMSetDatad(g_local_z_ref, local_z);
    if (g_heading_ref) XPLMSetDataf(g_heading_ref, pose.heading_deg);
    if (g_pitch_ref) XPLMSetDataf(g_pitch_ref, pose.pitch_deg);
    if (g_roll_ref) XPLMSetDataf(g_roll_ref, pose.roll_deg);
}

// Hands physics control back cleanly (see "Transitioning From Disabled to
// Enabled Flight Model" in the same X-Plane dev article): reconstruct the
// quaternion from the orientation we were last driving so re-enabling
// doesn't snap the model to some stale rotation, and zero the velocity
// vector as a safe default (a brief settle is expected and acceptable per
// docs/plan.md section 9's "notfalls mit leichtem Snap statt Drift").
// Called on disable/stop so a user is never left with a frozen aircraft.
void ReleasePhysicsOverride() {
    if (!g_physics_override_active) {
        return;
    }

    const float psi = g_heading_ref ? XPLMGetDataf(g_heading_ref) : 0.0f;
    const float theta = g_pitch_ref ? XPLMGetDataf(g_pitch_ref) : 0.0f;
    const float phi = g_roll_ref ? XPLMGetDataf(g_roll_ref) : 0.0f;
    const flytogether::Quaternion q = flytogether::EulerDegToQuaternion(psi, theta, phi);

    if (g_quaternion_ref) {
        float q_array[4] = {q.q0, q.q1, q.q2, q.q3};
        XPLMSetDatavf(g_quaternion_ref, q_array, 0, 4);
    }
    if (g_local_vx_ref) XPLMSetDataf(g_local_vx_ref, 0.0f);
    if (g_local_vy_ref) XPLMSetDataf(g_local_vy_ref, 0.0f);
    if (g_local_vz_ref) XPLMSetDataf(g_local_vz_ref, 0.0f);

    SetPhysicsOverride(false);
}

float SendSharedCockpitStateCallback(float /*elapsedSinceLastCall*/,
                                      float /*elapsedTimeSinceLastFlightLoop*/,
                                      int /*counter*/,
                                      void* /*refcon*/) {
    const flytogether::AircraftStatePacket packet =
        BuildOwnAircraftStatePacket(g_sender_id, g_shared_cockpit_sequence++);
    g_shared_cockpit.SendOwnState(packet); // no-op unless we're MASTER

    // 20 Hz, matching Formation mode's rate - Shared Cockpit needs at
    // least that for a physically-overridden aircraft to look smooth.
    return 1.0f / 20.0f;
}

// Defined further below (after StartSharedCockpitRendezvous, which its
// retry logic calls into) - forward-declared so
// PollSharedCockpitRendezvousCallback can call it.
void MaybeReconnectSharedCockpit();

// Drains/dispatches Shared Cockpit's own rendezvous session messages (peer
// discovery, relay fallback, keepalive) - same reasoning as
// UpdateFormationCallback's g_rendezvous_client.PollIncoming() call.
// Registered unconditionally at XPluginEnable (like
// PollControlListenerCallback), NOT tied to g_shared_cockpit_active's
// lifecycle: it has to already be running before StartSharedCockpit() is
// ever called, since on_peer_joined (which triggers that call) only fires
// as a result of this polling.
float PollSharedCockpitRendezvousCallback(float /*elapsedSinceLastCall*/,
                                           float /*elapsedTimeSinceLastFlightLoop*/,
                                           int /*counter*/,
                                           void* /*refcon*/) {
    g_shared_cockpit_rendezvous.PollIncoming();
    MaybeReconnectSharedCockpit();
    return -1.0f; // every frame, same reasoning as PollControlListenerCallback
}

float UpdateSharedCockpitCallback(float /*elapsedSinceLastCall*/,
                                   float /*elapsedTimeSinceLastFlightLoop*/,
                                   int /*counter*/,
                                   void* /*refcon*/) {
    const double now = XPLMGetElapsedTime();
    g_shared_cockpit.PollIncoming(now); // no-op unless we're CLIENT

    if (g_shared_cockpit.role() == flytogether::SharedCockpitRole::kClient) {
        if (g_shared_cockpit.HasMasterState() && !g_shared_cockpit.IsMasterStale(now)) {
            ApplyMasterPoseToOwnAircraft(g_shared_cockpit.ComputeMasterPose(now));
        } else if (g_physics_override_active) {
            // Master's gone quiet - hand control back rather than freezing
            // the client's aircraft in place indefinitely.
            ReleasePhysicsOverride();
        }

        // Client-side aircraft-type mismatch check: the master's ICAO
        // comes along for free on every AircraftStatePacket it already
        // sends (see BuildOwnAircraftStatePacket), so this is pure
        // comparison, no extra wire traffic. Only the client detects this
        // direction today - the master can't yet, since a client
        // deliberately never sends anything on its own direct socket (see
        // SharedCockpitSync::IngestRelayedPacket's comment on why, to
        // avoid opening an unwanted NAT hole) and there's no relay-path
        // announcement wired up for it yet.
        const std::string master_icao = g_shared_cockpit.MasterIcaoType();
        std::string mismatch;
        if (!master_icao.empty() && g_icao_type[0] != '\0' && master_icao != g_icao_type) {
            mismatch = std::string(g_icao_type) + ":" + master_icao;
        }
        if (mismatch != g_last_pushed_aircraft_mismatch) {
            g_last_pushed_aircraft_mismatch = mismatch;
            g_control_listener.SetSharedCockpitAircraftMismatch(mismatch);
        }
    }

    return -1.0f; // every frame, same reasoning as UpdateFormationCallback
}

float PollDatarefSyncCallback(float /*elapsedSinceLastCall*/,
                               float /*elapsedTimeSinceLastFlightLoop*/,
                               int /*counter*/,
                               void* /*refcon*/) {
    g_dataref_sync.Poll();
    // Catches the peer claiming a category over the network, which (unlike
    // a local ClaimOwnership() call) has no other point in this plugin
    // that would notice and push an update - see the function's comment.
    PushSharedCockpitOwnershipIfChanged();
    return 1.0f / 10.0f; // 10 Hz - switches don't need Formation's 20 Hz
}

// WeatherSync self-throttles its actual XPLMWeather.h calls internally
// (kWeatherApplyIntervalS, weather_sync.cpp) - this only needs to run
// often enough to drain the socket and let that internal timer fire
// promptly, nowhere near DatarefSync's 10 Hz.
float PollWeatherSyncCallback(float /*elapsedSinceLastCall*/,
                               float /*elapsedTimeSinceLastFlightLoop*/,
                               int /*counter*/,
                               void* /*refcon*/) {
    const double now = XPLMGetElapsedTime();
    const double latitude = g_latitude_ref ? XPLMGetDatad(g_latitude_ref) : 0.0;
    const double longitude = g_longitude_ref ? XPLMGetDatad(g_longitude_ref) : 0.0;
    const double elevation_m = g_elevation_ref ? XPLMGetDatad(g_elevation_ref) : 0.0;
    g_weather_sync.MaybeBroadcast(latitude, longitude, elevation_m, now); // no-op unless MASTER
    g_weather_sync.PollIncoming(latitude, longitude, elevation_m, now);   // no-op unless CLIENT
    return 1.0f; // 1 Hz - plenty to service a 30s internal interval
}

// Tears down the direct-UDP sync engines (SharedCockpitSync/DatarefSync)
// and hands physics control back if it was taken. Does NOT touch
// g_shared_cockpit_rendezvous - that's a separate session lifecycle,
// stopped independently (a peer leaving the rendezvous session should stop
// the sync engines; stopping the sync engines directly shouldn't
// necessarily tear down the session, e.g. while restarting after a
// mid-flight hiccup).
void StopSharedCockpit() {
    if (!g_shared_cockpit_active) {
        return;
    }
    XPLMUnregisterFlightLoopCallback(SendSharedCockpitStateCallback, nullptr);
    XPLMUnregisterFlightLoopCallback(UpdateSharedCockpitCallback, nullptr);
    XPLMUnregisterFlightLoopCallback(PollDatarefSyncCallback, nullptr);
    XPLMUnregisterFlightLoopCallback(PollWeatherSyncCallback, nullptr);
    ReleasePhysicsOverride();
    g_shared_cockpit.Stop();
    g_dataref_sync.Stop();
    g_weather_sync.Stop();
    g_shared_cockpit_active = false;

    // A stopped session has no master to compare against - clear any
    // mismatch warning left over from it rather than leaving a stale one
    // showing in the companion app through a fresh, unrelated session.
    if (!g_last_pushed_aircraft_mismatch.empty()) {
        g_last_pushed_aircraft_mismatch.clear();
        g_control_listener.SetSharedCockpitAircraftMismatch("");
    }
}

// Callable once Shared Cockpit's dedicated rendezvous session has
// discovered the other side's address, from
// SetupSharedCockpitRendezvousCallbacksOnce()'s on_peer_joined below (the
// only caller - the companion app is the only way into Shared Cockpit now,
// see this file's ROLE/PEER auto-start removal).
void StartSharedCockpit(flytogether::SharedCockpitRole role, const std::vector<flytogether::Peer>& peers,
                         const std::vector<flytogether::DatarefSyncSpec>& datarefs) {
    // Only ever called from on_peer_joined below, which itself can only
    // fire after on_session_ready has already derived
    // g_shared_cockpit_crypto - this should be unreachable, but refusing
    // to proceed insecurely if it somehow isn't is cheap insurance (see
    // g_rendezvous_client.on_session_ready's identical reasoning for
    // Formation).
    if (!g_shared_cockpit_crypto) {
        XPLMDebugString("XPMultiCrew: shared cockpit has no session key yet - refusing to start\n");
        g_control_listener.SetSharedCockpitStatus("error: no session key (internal error)");
        return;
    }

    if (g_shared_cockpit_active) {
        // Same reasoning as StartRendezvous(): always allow a fresh
        // attempt instead of getting stuck if the first one didn't
        // actually work out.
        XPLMDebugString("XPMultiCrew: shared cockpit already active, restarting fresh\n");
        StopSharedCockpit();
    }

    if (!g_shared_cockpit.Start(role, peers)) {
        XPLMDebugString("XPMultiCrew: failed to start shared cockpit sync (UDP port busy?)\n");
        g_control_listener.SetSharedCockpitStatus("failed to start (UDP port busy?)");
        return;
    }
    g_shared_cockpit_active = true;

    // Relay every position/dataref update through the Shared Cockpit
    // rendezvous session in parallel with the direct sends above - see
    // SharedCockpitSync::SetRelaySender's comment for why direct UDP alone
    // isn't reliable enough over the internet. RendezvousClient::SendRelay
    // no-ops when not actually in a session (e.g. the file-config manual-
    // peer path above never starts g_shared_cockpit_rendezvous), so this
    // is harmless to always wire up.
    g_shared_cockpit.SetRelaySender([](const void* data, size_t len) {
        g_shared_cockpit_rendezvous.SendRelay(data, len);
    });
    g_shared_cockpit.SetCrypto(&*g_shared_cockpit_crypto);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "XPMultiCrew: shared cockpit ready as %s, %zu peer(s)\n",
                  role == flytogether::SharedCockpitRole::kMaster ? "MASTER" : "CLIENT", peers.size());
    XPLMDebugString(buf);
    g_control_listener.SetSharedCockpitStatus(role == flytogether::SharedCockpitRole::kMaster
                                              ? "running as MASTER"
                                              : "running as CLIENT");

    XPLMRegisterFlightLoopCallback(SendSharedCockpitStateCallback, 1.0f / 20.0f, nullptr);
    XPLMRegisterFlightLoopCallback(UpdateSharedCockpitCallback, -1.0f, nullptr);

    // CLIENT seeds from its own current (cold-start) values so it doesn't
    // broadcast them back at the MASTER it just joined - see
    // DatarefSync::Start's comment. This is what makes the MASTER's
    // untouched Start() (still comparing against "nothing known yet") the
    // sole, deterministic source of the newly-joined CLIENT's initial
    // full state, instead of a race between both sides' cold-start dumps.
    const bool seed_from_current_values = role == flytogether::SharedCockpitRole::kClient;
    // MASTER starts owning every category by default (Shared Cockpit's
    // existing "master is authoritative unless told otherwise" posture);
    // CLIENT starts owning none until it claims one via the companion
    // app - see DatarefSync::Start's comment and ownership_tracker.h.
    const bool starts_owning_all_categories = role == flytogether::SharedCockpitRole::kMaster;
    if (g_dataref_sync.Start(datarefs, peers, seed_from_current_values, starts_owning_all_categories)) {
        g_dataref_sync.SetRelaySender([](const void* data, size_t len) {
            g_shared_cockpit_rendezvous.SendRelay(data, len);
        });
        g_dataref_sync.SetCrypto(&*g_shared_cockpit_crypto);
        char dr_buf[160];
        std::snprintf(dr_buf, sizeof(dr_buf), "XPMultiCrew: dataref sync watching %zu dataref(s)\n",
                      g_dataref_sync.watched_count());
        XPLMDebugString(dr_buf);
        XPLMRegisterFlightLoopCallback(PollDatarefSyncCallback, 1.0f / 10.0f, nullptr);
        // Push the fresh session's starting ownership immediately, rather
        // than waiting up to 100ms for the first PollDatarefSyncCallback
        // tick - g_has_pushed_ownership is reset so this doesn't get
        // skipped as "unchanged from last session" if the previous role
        // happened to end in the same state.
        g_has_pushed_ownership = false;
        PushSharedCockpitOwnershipIfChanged();
    } else {
        XPLMDebugString("XPMultiCrew: failed to start dataref sync (UDP port busy?)\n");
    }

    if (g_weather_sync.Start(role, peers)) {
        g_weather_sync.SetRelaySender([](const void* data, size_t len) {
            g_shared_cockpit_rendezvous.SendRelay(data, len);
        });
        XPLMRegisterFlightLoopCallback(PollWeatherSyncCallback, 1.0f, nullptr);
    } else {
        XPLMDebugString("XPMultiCrew: failed to start weather sync (UDP port busy?)\n");
    }
}

// Wires up g_shared_cockpit_rendezvous's callbacks exactly once. Mirrors
// SetupRendezvousCallbacksOnce() above, but for Shared Cockpit's 2-party
// session: once the other side is discovered (on_peer_joined), that's the
// signal to actually start the direct-UDP sync engines via
// StartSharedCockpit() - unlike the old manually-typed-peer path, we don't
// know the peer's address until the rendezvous server tells us.
void SetupSharedCockpitRendezvousCallbacksOnce() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    g_shared_cockpit_rendezvous.on_session_ready = [](const std::string& code, int /*your_id*/,
                                                        const std::vector<uint8_t>& salt) {
        // See Formation's on_session_ready for the full reasoning - same
        // salt-size fail-closed check, same derive-and-wire-up pattern,
        // just for Shared Cockpit's own separate session/key.
        if (salt.size() != flytogether::SessionCrypto::kSaltSize) {
            char err_buf[160];
            std::snprintf(err_buf, sizeof(err_buf),
                          "XPMultiCrew: shared cockpit rendezvous server sent a malformed session "
                          "salt (%zu bytes, expected %zu) - refusing to proceed insecurely\n",
                          salt.size(), flytogether::SessionCrypto::kSaltSize);
            XPLMDebugString(err_buf);
            g_control_listener.SetSharedCockpitStatus("error: server sent an invalid session salt");
            return;
        }
        std::array<uint8_t, flytogether::SessionCrypto::kSaltSize> salt_array{};
        std::copy(salt.begin(), salt.end(), salt_array.begin());
        // Re-derives the SAME key on a reconnect (unchanged code/salt for
        // the same session) - harmless to just overwrite; the sync
        // engines' SetCrypto pointers (set in StartSharedCockpit) still
        // point at this same std::optional's storage either way, not at
        // a moved-from/reallocated one.
        g_shared_cockpit_crypto.emplace(code, salt_array);

        // See Formation's on_session_ready for why a successful (re)connect
        // resets the backoff, not just the retry clock.
        g_shared_cockpit_reconnect.Reset();
        if (g_pending_shared_cockpit_role == flytogether::SharedCockpitRole::kMaster) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "XPMultiCrew: shared cockpit session ready - code '%s'. Share it with "
                          "your co-pilot.\n",
                          code.c_str());
            XPLMDebugString(buf);
            g_control_listener.SetSharedCockpitStatus("waiting for co-pilot, code '" + code + "'");
            g_control_listener.SetSharedCockpitCode(code);
        } else {
            XPLMDebugString("XPMultiCrew: joined shared cockpit session, waiting for host\n");
            g_control_listener.SetSharedCockpitStatus("joined, waiting for host...");
        }
        // Remember the server-assigned code so a reconnect (see
        // on_disconnected below) rejoins this exact session - same
        // reasoning as Formation's on_session_ready.
        g_shared_cockpit_reconnect_code = code;
    };
    g_shared_cockpit_rendezvous.on_peer_joined = [](int peer_id, const std::string& host, uint16_t port) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "XPMultiCrew: shared cockpit peer %d found at %s:%u\n", peer_id,
                      host.c_str(), port);
        XPLMDebugString(buf);
        StartSharedCockpit(g_pending_shared_cockpit_role, {flytogether::Peer{host, port}},
                            g_pending_shared_cockpit_datarefs);
    };
    g_shared_cockpit_rendezvous.on_peer_left = [](int /*peer_id*/) {
        XPLMDebugString("XPMultiCrew: shared cockpit peer left\n");
        g_control_listener.SetSharedCockpitStatus("co-pilot disconnected");
        StopSharedCockpit();
    };
    g_shared_cockpit_rendezvous.on_relay_received = [](int /*from_peer_id*/,
                                                         const std::vector<uint8_t>& bytes) {
        const double now = XPLMGetElapsedTime();

        // Weather is deliberately NOT encrypted (see WeatherSync's own
        // Start()/SendOwnState, which never had a SetCrypto wired up -
        // low-stakes to leave out of this rollout, no position/flight-
        // control/ownership data), but shares this same relay stream with
        // the three message shapes that ARE. Peek the RAW (pre-decrypt)
        // magic first and handle weather straight away if it matches - an
        // encrypted envelope's first 4 bytes are effectively random
        // nonce bytes, so a false-positive collision with
        // kWeatherStateMagic here is astronomically unlikely, and even
        // then WeatherSync's own magic re-check inside
        // IngestRelayedPacket would still catch it.
        uint32_t raw_magic = 0;
        if (bytes.size() >= sizeof(raw_magic)) {
            std::memcpy(&raw_magic, bytes.data(), sizeof(raw_magic));
        }
        if (raw_magic == flytogether::kWeatherStateMagic) {
            g_weather_sync.IngestRelayedPacket(bytes.data(), bytes.size());
            return;
        }

        // Everything else on this channel (position, dataref sync,
        // ownership) IS encrypted - decrypted ONCE here, before the
        // magic-based dispatch below, since that dispatch needs the
        // PLAINTEXT magic to route correctly. This is also why
        // SharedCockpitSync::IngestRelayedPacket/
        // DatarefSync::IngestRelayedMessage (called below) do NOT decrypt
        // internally: it already happened here, once, for whichever one
        // ends up receiving these bytes - see their own comments. No
        // session key yet (crypto unset) is itself impossible here:
        // on_relay_received can only fire after on_session_ready already
        // set g_shared_cockpit_crypto.
        if (!g_shared_cockpit_crypto) {
            return;
        }
        const auto opened = g_shared_cockpit_crypto->Open(bytes);
        if (!opened) {
            return; // wrong/no key, or corrupted/foreign packet
        }
        const std::vector<uint8_t>& plain = *opened;

        // Disambiguate by the leading (plaintext) magic value, not by
        // size: both AircraftStatePacket and an encoded DatarefSyncMessage
        // start with a uint32 magic, and relying on size alone (the two
        // happen to be exactly equal for some plausible dataref-name
        // lengths) risked routing a dataref-sync message through the
        // position-packet path or vice versa. Each Ingest* call
        // re-validates its own magic on top of this, so a corrupted/
        // foreign payload is still rejected either way.
        uint32_t magic = 0;
        if (plain.size() >= sizeof(magic)) {
            std::memcpy(&magic, plain.data(), sizeof(magic));
        }
        if (magic == flytogether::kAircraftStateMagic) {
            g_shared_cockpit.IngestRelayedPacket(plain.data(), plain.size(), now);
        } else if (magic == flytogether::kDatarefSyncMagic || magic == flytogether::kOwnershipClaimMagic) {
            // Both land on the same DatarefSync UDP channel and
            // IngestRelayedMessage already re-disambiguates between them
            // internally (DatarefSync::ApplyIncomingBytes peeks the magic
            // again) - the ownership magic was missing from this outer
            // gate entirely until this was first noticed, meaning an
            // ownership message relayed (rather than reaching the peer via
            // direct UDP) was silently dropped here before ever reaching
            // that internal check.
            g_dataref_sync.IngestRelayedMessage(plain.data(), plain.size());
        }
        // Anything else (wrong magic, too short): not one of ours, ignore.
    };
    g_shared_cockpit_rendezvous.on_error = [](const std::string& message) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "XPMultiCrew: shared cockpit rendezvous error: %s\n",
                      message.c_str());
        XPLMDebugString(buf);
        g_control_listener.SetSharedCockpitStatus("error: " + message);
    };
    g_shared_cockpit_rendezvous.on_disconnected = []() {
        XPLMDebugString("XPMultiCrew: shared cockpit rendezvous connection lost\n");
        // Deliberately does NOT StopSharedCockpit(): the direct-UDP sync
        // engines talk straight to the co-pilot's already-known address
        // and may well still be working fine even though our heartbeat to
        // the rendezvous server (which only matters for discovery and the
        // relay fallback) lapsed - tearing down an otherwise-healthy
        // flight over a lost backup path would be worse than just quietly
        // reconnecting it in the background.
        g_control_listener.SetSharedCockpitStatus(
            g_shared_cockpit_reconnect.wanted ? "connection lost, reconnecting..." : "not started");
    };
}

// Callable from the companion app's "Start Shared Cockpit" request. Starts
// (or restarts fresh - same reasoning as StartRendezvous()) Shared
// Cockpit's own rendezvous session; the actual sync engines only start
// once on_peer_joined fires above with the discovered peer address.
void StartSharedCockpitRendezvous(const std::string& host, uint16_t port,
                                   flytogether::SharedCockpitRole role, const std::string& code,
                                   const std::vector<flytogether::DatarefSyncSpec>& datarefs) {
    SetupSharedCockpitRendezvousCallbacksOnce();

    g_shared_cockpit_reconnect.wanted = true;
    g_shared_cockpit_reconnect_host = host;
    g_shared_cockpit_reconnect_port = port;
    g_shared_cockpit_reconnect_role = role;
    g_shared_cockpit_reconnect_code = code; // "" for MASTER's first attempt, see MaybeReconnectSharedCockpit
    g_shared_cockpit_reconnect.Reset();

    if (g_shared_cockpit_rendezvous_active) {
        XPLMDebugString("XPMultiCrew: shared cockpit rendezvous already active, reconnecting fresh\n");
        g_shared_cockpit_rendezvous.Stop();
        g_shared_cockpit_rendezvous_active = false;
        StopSharedCockpit();
    }

    g_pending_shared_cockpit_role = role;
    g_pending_shared_cockpit_datarefs = datarefs;

    if (!g_shared_cockpit_rendezvous.Start(host, port)) {
        XPLMDebugString("XPMultiCrew: failed to start shared cockpit rendezvous client (UDP socket?)\n");
        g_control_listener.SetSharedCockpitStatus("failed to start (UDP socket busy?)");
        return;
    }
    g_shared_cockpit_rendezvous_active = true;
    g_control_listener.SetSharedCockpitCode(""); // clear any stale code from a previous session
    g_control_listener.SetSharedCockpitStatus("connecting to " + host + ":" + std::to_string(port) +
                                               "...");
    if (role == flytogether::SharedCockpitRole::kMaster) {
        g_shared_cockpit_rendezvous.CreateSession();
    } else {
        g_shared_cockpit_rendezvous.JoinSession(code);
    }
}

// Companion app's explicit "Disconnect" for Shared Cockpit - mirrors
// DisconnectFormation().
void DisconnectSharedCockpit() {
    g_shared_cockpit_reconnect.wanted = false;
    StopSharedCockpit();
    g_shared_cockpit_rendezvous.Stop(); // sends leave_session if we were actually in one
    g_shared_cockpit_rendezvous_active = false;
    // Clear the consumers' pointers BEFORE destroying what they point to -
    // see DisconnectFormation()'s identical comment.
    g_shared_cockpit.SetCrypto(nullptr);
    g_dataref_sync.SetCrypto(nullptr);
    g_shared_cockpit_crypto.reset();
    g_control_listener.SetSharedCockpitCode("");
    g_control_listener.SetSharedCockpitStatus("not started");
}

// Mirrors MaybeReconnectFormation() - see its comment. Called from
// PollSharedCockpitRendezvousCallback, which already runs every frame
// unconditionally.
void MaybeReconnectSharedCockpit() {
    if (!g_shared_cockpit_reconnect.Due(g_shared_cockpit_rendezvous.InSession())) {
        return;
    }
    XPLMDebugString("XPMultiCrew: attempting to reconnect the shared cockpit rendezvous session...\n");
    if (g_shared_cockpit_reconnect_code.empty() &&
        g_shared_cockpit_reconnect_role == flytogether::SharedCockpitRole::kMaster) {
        g_shared_cockpit_rendezvous.CreateSession();
    } else {
        g_shared_cockpit_rendezvous.JoinSession(g_shared_cockpit_reconnect_code);
    }
}

double g_last_link_quality_push_s = -1000.0;

// Builds and pushes the LINK_QUALITY status line (see control_listener.h's
// wire-format comment) from every independent source that has one: both
// RendezvousClients' server RTT (RendezvousClient::Rtt(), an approximation
// - see its comment), FormationSync's per-peer packet loss, and
// SharedCockpitSync's master-link loss. Throttled to about once a second
// (same reasoning as UpdateFormationCallback's XPLMDebugString summary
// line just above it) - called from PollControlListenerCallback, which
// already runs every frame unconditionally regardless of which mode(s), if
// any, are actually active.
void MaybePushLinkQuality(double now) {
    if (now - g_last_link_quality_push_s < 1.0) {
        return;
    }
    g_last_link_quality_push_s = now;

    std::ostringstream out;
    out << "formation_server_rtt_ms:";
    if (g_rendezvous_client.HasRtt()) {
        out << g_rendezvous_client.Rtt().count();
    } else {
        out << "?";
    }

    out << " formation_peer_loss_pct:";
    bool first_peer = true;
    g_formation_sync.ForEachLinkQuality([&](uint32_t sender_id, double loss_ratio) {
        if (!first_peer) {
            out << ";";
        }
        first_peer = false;
        out << sender_id << ":" << static_cast<int>(loss_ratio * 100.0 + 0.5);
    });

    out << " sc_server_rtt_ms:";
    if (g_shared_cockpit_rendezvous.HasRtt()) {
        out << g_shared_cockpit_rendezvous.Rtt().count();
    } else {
        out << "?";
    }

    out << " sc_master_loss_pct:";
    if (g_shared_cockpit.HasMasterLinkQualityData()) {
        out << static_cast<int>(g_shared_cockpit.MasterLinkLossRatio() * 100.0 + 0.5);
    } else {
        out << "?";
    }

    g_control_listener.SetLinkQuality(out.str());
}

float PollControlListenerCallback(float /*elapsedSinceLastCall*/,
                                   float /*elapsedTimeSinceLastFlightLoop*/,
                                   int /*counter*/,
                                   void* /*refcon*/) {
    // This callback executing at all is proof the sim isn't on a loading
    // screen right now (see control_listener.h's SIM_READY comment) -
    // catches the one case XPLM_MSG_PLANE_LOADED alone misses: the very
    // first flight of an X-Plane session, which can finish loading before
    // this plugin was even enabled to receive that message. That's also
    // the one case RefreshOwnIcaoType()'s own XPLM_MSG_PLANE_LOADED call
    // misses (same message, same "not listening yet" gap) - self-heal it
    // here too, otherwise acf_ICAO could stay empty (read too early in
    // XPluginStart) for a peer's entire session on their very first
    // flight, showing up as an unrecognized aircraft type to everyone else.
    if (!g_control_listener.IsSimReady()) {
        g_control_listener.SetSimReady(true);
        RefreshOwnIcaoType();
    }
    g_control_listener.Poll();
    MaybePushLinkQuality(XPLMGetElapsedTime());
    return -1.0f; // every frame, so the companion app feels responsive
}

} // namespace

extern "C" {

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strcpy(outName, "XPMultiCrew");
    std::strcpy(outSig, "io.github.xpmulticrew.plugin");
    std::strcpy(outDesc,
                "Cross-platform multiplayer & shared cockpit plugin for "
                "X-Plane 12 (Formation, internet play, Shared Cockpit MVP).");

    g_latitude_ref = XPLMFindDataRef("sim/flightmodel/position/latitude");
    g_longitude_ref = XPLMFindDataRef("sim/flightmodel/position/longitude");
    g_elevation_ref = XPLMFindDataRef("sim/flightmodel/position/elevation");

    g_heading_ref = XPLMFindDataRef("sim/flightmodel/position/psi");
    g_pitch_ref = XPLMFindDataRef("sim/flightmodel/position/theta");
    g_roll_ref = XPLMFindDataRef("sim/flightmodel/position/phi");
    g_gear_ref = XPLMFindDataRef("sim/flightmodel2/gear/deploy_ratio");
    g_flap_ref = XPLMFindDataRef("sim/flightmodel/controls/flaprat");
    g_speedbrake_ref = XPLMFindDataRef("sim/cockpit2/controls/speedbrake_ratio");
    g_engine_ref = XPLMFindDataRef("sim/flightmodel/engine/ENGN_thro");
    g_beacon_ref = XPLMFindDataRef("sim/cockpit/electrical/beacon_lights_on");
    g_strobe_ref = XPLMFindDataRef("sim/cockpit/electrical/strobe_lights_on");
    g_nav_ref = XPLMFindDataRef("sim/cockpit/electrical/nav_lights_on");
    g_landing_ref = XPLMFindDataRef("sim/cockpit/electrical/landing_lights_on");

    g_override_planepath_ref = XPLMFindDataRef("sim/operation/override/override_planepath");
    g_local_x_ref = XPLMFindDataRef("sim/flightmodel/position/local_x");
    g_local_y_ref = XPLMFindDataRef("sim/flightmodel/position/local_y");
    g_local_z_ref = XPLMFindDataRef("sim/flightmodel/position/local_z");
    g_quaternion_ref = XPLMFindDataRef("sim/flightmodel/position/q");
    g_local_vx_ref = XPLMFindDataRef("sim/flightmodel/position/local_vx");
    g_local_vy_ref = XPLMFindDataRef("sim/flightmodel/position/local_vy");
    g_local_vz_ref = XPLMFindDataRef("sim/flightmodel/position/local_vz");

    g_icao_ref = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
    RefreshOwnIcaoType(); // best-effort now; XPLM_MSG_PLANE_LOADED refreshes it properly - see its comment

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFEu);
    g_sender_id = dist(gen);

    g_control_listener.SetCslStatus(InitXpmpAndLoadCsl());

    return 1;
}

PLUGIN_API void XPluginStop() {
    g_latitude_ref = nullptr;
    g_longitude_ref = nullptr;
    g_elevation_ref = nullptr;

    if (g_xpmp_initialized) {
        XPMPMultiplayerCleanup();
        g_xpmp_initialized = false;
    }
}

PLUGIN_API int XPluginEnable() {
    flytogether::ControlListener::Callbacks control_callbacks;
    control_callbacks.on_create_session = [](const std::string& host_port, bool as_spectator) {
        std::string host;
        uint16_t port = 0;
        if (!flytogether::SplitHostPort(host_port, host, port)) {
            g_control_listener.SetFormationStatus("invalid server address (need host:port)");
            return;
        }
        StartRendezvous(host, port, /*create=*/true, "", as_spectator);
    };
    control_callbacks.on_join_session = [](const std::string& host_port, const std::string& code,
                                            bool as_spectator) {
        std::string host;
        uint16_t port = 0;
        if (!flytogether::SplitHostPort(host_port, host, port)) {
            g_control_listener.SetFormationStatus("invalid server address (need host:port)");
            return;
        }
        if (code.empty()) {
            g_control_listener.SetFormationStatus("enter a session code to join");
            return;
        }
        StartRendezvous(host, port, /*create=*/false, code, as_spectator);
    };
    control_callbacks.on_start_shared_cockpit = [](bool is_master, const std::string& server_host_port,
                                                     const std::string& code) {
        std::string host;
        uint16_t port = 0;
        if (!flytogether::SplitHostPort(server_host_port, host, port)) {
            g_control_listener.SetSharedCockpitStatus("invalid server address (need host:port)");
            return;
        }
        const auto role =
            is_master ? flytogether::SharedCockpitRole::kMaster : flytogether::SharedCockpitRole::kClient;
        if (role == flytogether::SharedCockpitRole::kClient && code.empty()) {
            g_control_listener.SetSharedCockpitStatus("enter a session code to join");
            return;
        }
        // The DATAREF list still comes from a config file, even though
        // ROLE/server/code here come from the companion app instead - see
        // the README's Phase 3 "not yet done" note. Prefers a per-aircraft
        // profile (user override next to X-Plane, then the plugin's own
        // bundled default) over the flat fallback file - see
        // ResolveSharedCockpitConfigPath's comment.
        const std::string icao(g_icao_type, strnlen(g_icao_type, sizeof(g_icao_type)));
        const auto file_config = flytogether::LoadSharedCockpitConfig(flytogether::ResolveSharedCockpitConfigPath(
            "XPMultiCrew_shared_cockpit.txt", icao, GetPluginResourcesPath()));
        StartSharedCockpitRendezvous(host, port, role, code, file_config.datarefs);
    };
    control_callbacks.on_lan_connect_formation = [](const std::string& host_port, const std::string& code) {
        LanConnectFormation(host_port, code);
    };
    control_callbacks.on_disconnect_formation = []() { DisconnectFormation(); };
    control_callbacks.on_disconnect_shared_cockpit = []() { DisconnectSharedCockpit(); };
    control_callbacks.on_reload_csl = []() { ReloadCsl(); };
    control_callbacks.on_claim_ownership = [](flytogether::DatarefCategory category) {
        g_dataref_sync.ClaimOwnership(category);
        PushSharedCockpitOwnershipIfChanged();
    };
    if (g_control_listener.Start(control_callbacks)) {
        XPLMRegisterFlightLoopCallback(PollControlListenerCallback, -1.0f, nullptr);
        g_control_listener.SetPluginVersion(XPMULTICREW_VERSION);
    } else {
        XPLMDebugString("XPMultiCrew: failed to start control listener (UDP port busy?), "
                         "companion app won't be reachable\n");
    }

    // Always running from enable onward, same reasoning as
    // PollControlListenerCallback: a "Start Shared Cockpit" button click can
    // happen at any time, and its CreateSession()/JoinSession() replies have
    // to be pollable immediately.
    XPLMRegisterFlightLoopCallback(PollSharedCockpitRendezvousCallback, -1.0f, nullptr);

    XPLMRegisterFlightLoopCallback(LogPositionCallback, 5.0f, nullptr);

    if (g_udp_socket.Open()) {
        XPLMRegisterFlightLoopCallback(SendPositionOverUdpCallback, 0.2f, nullptr);
    } else {
        XPLMDebugString("XPMultiCrew: failed to open UDP socket, position "
                         "will not be sent over the network\n");
    }

    const std::string peer_list_path =
        flytogether::ResolvePeerListPath("XPMultiCrew_peers.txt");
    if (g_formation_sync.Start(peer_list_path)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "XPMultiCrew: formation sync ready, sender_id=%u, "
                      "%zu peer(s) loaded from '%s'\n",
                      g_sender_id, g_formation_sync.peer_count(),
                      peer_list_path.c_str());
        XPLMDebugString(buf);

        XPLMRegisterFlightLoopCallback(SendFormationStateCallback, 1.0f / 20.0f, nullptr);
        XPLMRegisterFlightLoopCallback(UpdateFormationCallback, -1.0f, nullptr);
    } else {
        XPLMDebugString("XPMultiCrew: failed to start formation sync (UDP "
                         "port busy?), Formation mode disabled\n");
    }

    if (g_xpmp_initialized) {
        const char* err = XPMPMultiplayerEnable();
        if (err && err[0]) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "XPMultiCrew: XPMPMultiplayerEnable failed: %s\n", err);
            XPLMDebugString(buf);
        }
    }

    // Formation's internet play (rendezvous/relay) also only ever starts
    // through the companion app now, same as Shared Cockpit - see
    // StartRendezvous()'s comment for why the file-based auto-start this
    // used to support was removed. LAN peers (formation/peer_list.h,
    // loaded into g_formation_sync above) are unaffected - that's a plain
    // list of direct peer addresses, not a rendezvous session to start.

    return 1;
}

PLUGIN_API void XPluginDisable() {
    XPLMUnregisterFlightLoopCallback(PollControlListenerCallback, nullptr);
    g_control_listener.Stop();

    XPLMUnregisterFlightLoopCallback(LogPositionCallback, nullptr);
    XPLMUnregisterFlightLoopCallback(SendPositionOverUdpCallback, nullptr);
    g_udp_socket.Close();

    XPLMUnregisterFlightLoopCallback(SendFormationStateCallback, nullptr);
    XPLMUnregisterFlightLoopCallback(UpdateFormationCallback, nullptr);
    g_formation_sync.Stop();

    g_rendezvous_client.Stop();
    g_rendezvous_peers.clear();
    g_rendezvous_active = false;
    g_formation_reconnect.wanted = false; // don't reconnect into a disabled plugin

    g_xpmp_aircraft.clear();
    if (g_xpmp_initialized) {
        XPMPMultiplayerDisable();
    }

    XPLMUnregisterFlightLoopCallback(PollSharedCockpitRendezvousCallback, nullptr);
    g_shared_cockpit_rendezvous.Stop();
    g_shared_cockpit_rendezvous_active = false;
    g_shared_cockpit_reconnect.wanted = false;

    // Never leave the user's aircraft frozen under an active physics
    // override just because the plugin got disabled.
    StopSharedCockpit();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID /*inFrom*/, int inMsg, void* inParam) {
    // XPLM_MSG_PLANE_LOADED/UNLOADED's parameter is an aircraft index
    // bit-cast into the void*, not a real pointer - 0 is always the
    // user's own aircraft (see XPLMPlugin.h's comment on both messages).
    // Unlike flight-loop callbacks (which simply don't run during a
    // loading screen), XPluginReceiveMessage is delivered directly by
    // X-Plane's own messaging system, so this is the one reliable signal
    // for "a flight just finished loading" available to a plugin - see
    // control_listener.h's SIM_READY status line, which surfaces this to
    // the companion app.
    const auto plane_index = reinterpret_cast<intptr_t>(inParam);
    if (inMsg == XPLM_MSG_PLANE_LOADED && plane_index == 0) {
        g_control_listener.SetSimReady(true);
        // See RefreshOwnIcaoType()'s comment: this is what actually
        // catches acf_ICAO being empty at XPluginStart-time (X-Plane
        // hadn't loaded an aircraft yet), and keeps it correct across an
        // aircraft change mid-session too.
        RefreshOwnIcaoType();
    } else if (inMsg == XPLM_MSG_PLANE_UNLOADED && plane_index == 0) {
        g_control_listener.SetSimReady(false);
    }
}

} // extern "C"
