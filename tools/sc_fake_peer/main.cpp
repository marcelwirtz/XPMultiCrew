// A stand-in for the second pilot in a Shared Cockpit session, so it can be
// tested solo. Talks to the rendezvous server exactly like the plugin does
// (same RendezvousClient, SessionCrypto and wire formats) - your own
// X-Plane can't tell it from a real peer.
//
//   sc_fake_peer
//       Opens the GUI in your browser (a small page served from this
//       program on 127.0.0.1): connect, watch what your cockpit sends,
//       flip the C172's switches, press its buttons, take control.
//
//   sc_fake_peer join <code> [--server host:port]
//       Console only: joins the session your plugin created (companion:
//       Shared Cockpit, "Host") as CO-PILOT. Prints what your cockpit
//       sends - position, yoke/levers, every switch change, button presses.
//
//   sc_fake_peer host --airport <ICAO> [--runway <name>] [--air] [--xplane <folder>]
//   sc_fake_peer host --pos <lat> <lon> <elev_m> [--hdg <deg>] [--ground]
//                     [--speed <kt>] [--server host:port]
//       Console only: creates a session and is the PILOT FLYING. Join it
//       from the companion with the printed code and your aircraft follows
//       a fake pose, with the yoke and throttle moving. --airport lines it
//       up on a runway (the longest unless --runway; --air orbits 1000 ft
//       above it instead), read from your X-Plane's apt.dat. --pos: by
//       default a gentle orbit at the given elevation; --ground parks it
//       there, elev_m being the ground (field elevation) in meters MSL. Switch changes your cockpit requests are echoed
//       back like a real master's simulator would.
//
// Both swap roles like the plugin: "take" claims the controls, and your
// plugin taking them back makes this side the co-pilot again. Type "help"
// at the console for its commands (the GUI sends the very same ones).
// Commands can also be piped in from a file ("wait <seconds>" pauses).
//
// What it can't fake: anything that needs a real simulator on this side -
// it doesn't run the commands it receives or change datarefs by itself,
// and sends no digest, weather or sim time.

#include "airport_lookup.h"
#include "c172_preset.h"
#include "gui_page.h"

#include "flytogether/aircraft_state.h"
#include "formation/rendezvous_client.h"
#include "formation/rendezvous_protocol.h"
#include "net/secure_random.h"
#include "net/session_crypto.h"
#include "shared_cockpit/command_sync_protocol.h"
#include "shared_cockpit/controls_sync_protocol.h"
#include "shared_cockpit/engine_sync_protocol.h"
#include "shared_cockpit/dataref_sync_protocol.h"
#include "shared_cockpit/weather_sync_protocol.h"
#include "sync/time_sync.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ft = flytogether;

namespace {

// File-local in plugin_main.cpp - keep in sync by hand.
constexpr uint32_t kChecklistMagic = 0x4654434b; // "FTCK"
constexpr uint32_t kDigestMagic = 0x46544447;    // "FTDG"
constexpr uint32_t kResyncMagic = 0x46545253;    // "FTRS"

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6378137.0;
constexpr double kKtToMps = 0.514444;
constexpr double kMToFt = 3.28084;

constexpr uint16_t kGuiPort = 49080; // TCP, 127.0.0.1 only; falls back to any free port

double Now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// --- log: console + the GUI's last lines ---

std::mutex g_log_mutex;
std::deque<std::string> g_log_lines;
uint64_t g_log_seq = 0;
constexpr size_t kGuiLogLines = 200;

void Print(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    char line[1100];
    std::snprintf(line, sizeof(line), "[%7.1f] %s", Now(), msg);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::printf("%s\n", line);
    std::fflush(stdout);
    g_log_lines.push_back(line);
    if (g_log_lines.size() > kGuiLogLines) g_log_lines.pop_front();
    ++g_log_seq;
}

// --- small JSON writer ---

std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string JsonNumber(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

// --- dataref values ---

std::string DescribeValue(const ft::DatarefValue& v) {
    char buf[32];
    std::string out;
    switch (v.type) {
        case ft::DatarefValueType::kInt: return std::to_string(v.int_value);
        case ft::DatarefValueType::kFloat: std::snprintf(buf, sizeof(buf), "%.4g", v.float_value); return buf;
        case ft::DatarefValueType::kDouble: std::snprintf(buf, sizeof(buf), "%.6g", v.double_value); return buf;
        case ft::DatarefValueType::kIntArray:
            for (int i = 0; i < v.array_len; ++i) out += (i ? "," : "") + std::to_string(v.int_array[i]);
            return "[" + out + "]";
        case ft::DatarefValueType::kFloatArray:
            for (int i = 0; i < v.array_len; ++i) {
                std::snprintf(buf, sizeof(buf), "%s%.4g", i ? "," : "", v.float_array[i]);
                out += buf;
            }
            return "[" + out + "]";
    }
    return "?";
}

std::string ValueJson(const ft::DatarefValue& v) {
    std::string out;
    switch (v.type) {
        case ft::DatarefValueType::kInt: return std::to_string(v.int_value);
        case ft::DatarefValueType::kFloat: return JsonNumber(v.float_value);
        case ft::DatarefValueType::kDouble: return JsonNumber(v.double_value);
        case ft::DatarefValueType::kIntArray:
            for (int i = 0; i < v.array_len; ++i) out += (i ? "," : "") + std::to_string(v.int_array[i]);
            return "[" + out + "]";
        case ft::DatarefValueType::kFloatArray:
            for (int i = 0; i < v.array_len; ++i) out += (i ? "," : "") + JsonNumber(v.float_array[i]);
            return "[" + out + "]";
    }
    return "null";
}

const char* TypeTag(ft::DatarefValueType t) {
    switch (t) {
        case ft::DatarefValueType::kInt: return "i";
        case ft::DatarefValueType::kFloat: return "f";
        case ft::DatarefValueType::kDouble: return "d";
        case ft::DatarefValueType::kIntArray: return "ia";
        case ft::DatarefValueType::kFloatArray: return "fa";
    }
    return "?";
}

std::optional<ft::DatarefValueType> ParseTypeTag(const std::string& tag) {
    if (tag == "i") return ft::DatarefValueType::kInt;
    if (tag == "f") return ft::DatarefValueType::kFloat;
    if (tag == "d") return ft::DatarefValueType::kDouble;
    if (tag == "ia") return ft::DatarefValueType::kIntArray;
    if (tag == "fa") return ft::DatarefValueType::kFloatArray;
    return std::nullopt;
}

std::optional<ft::DatarefValue> ParseValue(ft::DatarefValueType type, const std::string& text) {
    ft::DatarefValue v;
    v.type = type;
    char* end = nullptr;
    switch (type) {
        case ft::DatarefValueType::kInt:
            v.int_value = static_cast<int>(std::lround(std::strtod(text.c_str(), &end)));
            return *end ? std::nullopt : std::optional(v);
        case ft::DatarefValueType::kFloat:
            v.float_value = std::strtof(text.c_str(), &end);
            return *end ? std::nullopt : std::optional(v);
        case ft::DatarefValueType::kDouble:
            v.double_value = std::strtod(text.c_str(), &end);
            return *end ? std::nullopt : std::optional(v);
        case ft::DatarefValueType::kIntArray:
        case ft::DatarefValueType::kFloatArray: {
            std::stringstream ss(text);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (v.array_len >= ft::kDatarefSyncMaxArrayLen) return std::nullopt;
                if (type == ft::DatarefValueType::kIntArray) {
                    v.int_array[v.array_len] = static_cast<int>(std::lround(std::strtod(item.c_str(), &end)));
                } else {
                    v.float_array[v.array_len] = std::strtof(item.c_str(), &end);
                }
                if (*end) return std::nullopt;
                ++v.array_len;
            }
            return v.array_len ? std::optional(v) : std::nullopt;
        }
    }
    return std::nullopt;
}

const sc_fake::PresetDataref* FindPreset(const std::string& name) {
    for (const auto& p : sc_fake::kC172Datarefs) {
        if (name == p.name) return &p;
    }
    return nullptr;
}

// Kinematic stand-in for a flight model: constant speed, vertical speed and
// turn rate, banked to match the turn.
struct FakeFlight {
    double lat = 0.0, lon = 0.0, elev_m = 0.0;
    double heading_deg = 0.0;
    double speed_mps = 0.0;
    double vs_mps = 0.0;
    double turn_dps = 0.0;
    bool on_ground = false;
    double ground_m = NAN; // known ground elevation (MSL), NaN = unknown
    double ref_m = 1.5;    // reference point above the gear (kFakeRefHeightM)

    void Step(double dt) {
        heading_deg = std::fmod(heading_deg + turn_dps * dt + 360.0, 360.0);
        const double h = heading_deg * kPi / 180.0;
        const double north = speed_mps * std::cos(h) * dt;
        const double east = speed_mps * std::sin(h) * dt;
        lat += north / kEarthRadiusM * 180.0 / kPi;
        lon += east / (kEarthRadiusM * std::cos(lat * kPi / 180.0)) * 180.0 / kPi;
        if (!on_ground) elev_m += vs_mps * dt;
    }

    double BankDeg() const {
        if (on_ground) return 0.0;
        return std::atan(speed_mps * turn_dps * kPi / 180.0 / 9.81) * 180.0 / kPi;
    }
};

// What the pilot flying's simulator would do with the C172's key and
// starter: the co-pilot's X-Plane only shows ignition_key and ENGN_running
// as sent by the pilot flying (OUTPUT in the profile), so without this a
// fake pilot flying would leave the engine off for good.
constexpr const char* kIgnitionKeyRef = "sim/cockpit2/engine/actuators/ignition_key";
constexpr const char* kEngineRunningRef = "sim/flightmodel/engine/ENGN_running";

struct FakeEngine {
    int key = 0;           // 0 off, 1 R, 2 L, 3 both, 4 start (spring-loaded back to 3)
    bool running = false;
    bool starter_held = false; // sim/starters/engage_starter_1
    double cranking_s = 0.0;

    static constexpr double kCrankToStartS = 1.2;

    bool Cranking() const { return key == 4 || starter_held; }

    // True if a command concerned the engine.
    bool OnCommand(const std::string& name, bool begin) {
        if (name == "laminar/c172/ignition_up") {
            if (begin) key = std::min(key + 1, 4);
            else if (key == 4) key = 3;
        } else if (name == "laminar/c172/ignition_down") {
            if (begin) key = std::max(key - 1, 0);
        } else if (name == "sim/starters/engage_starter_1") {
            starter_held = begin;
        } else if (name == "sim/starters/shut_down") {
            if (begin) running = false;
        } else {
            return false;
        }
        return true;
    }

    void Step(double dt, float mixture) {
        if (Cranking() && !running) {
            cranking_s += dt;
            if (cranking_s >= kCrankToStartS && mixture > 0.1f && key != 0) running = true;
        } else {
            cranking_s = 0.0;
        }
        if (key == 0 || mixture < 0.05f) running = false;
    }

    float Rpm(float throttle) const {
        if (running) return 700.0f + 1700.0f * std::clamp(throttle, 0.0f, 1.0f);
        return Cranking() ? 250.0f : 0.0f;
    }
};

struct Options {
    bool host = false;
    std::string code;
    std::string server = "127.0.0.1:45000";
    bool has_pos = false;
    double lat = 0.0, lon = 0.0, elev_m = 0.0, heading_deg = 0.0;
    double speed_kt = -1.0;
    bool ground = false;
    std::string icao = "C172";
    int engines = 1;
    std::string airport, runway, xplane; // --airport: placed on a runway
    bool air = false;                    // --airport --air: 1000 ft above it
    double ground_m = NAN;               // the ground under the start position, if known
    std::string placement;               // what --airport resolved to, for the log
};

// The fake's reference point above its "gear" - only reported so the
// co-pilot can work out the ground; it puts its own aircraft at its own
// measured height (plugin_main.cpp's SharedCockpitGroundOffsetM).
constexpr double kFakeRefHeightM = 1.5;

// --airport -> position; --ground -> elev is the ground. False + error if
// the airport can't be placed.
bool ResolvePosition(Options& o, std::string& error) {
    if (!o.host) return true;
    if (!o.airport.empty()) {
        const auto p = airport_lookup::Resolve(o.xplane, o.airport, o.runway, error);
        if (!p) return false;
        o.lat = p->lat;
        o.lon = p->lon;
        o.heading_deg = p->heading_deg;
        o.ground = !o.air;
        o.ground_m = p->ground_m;
        o.elev_m = p->ground_m + (o.air ? 1000.0 * 0.3048 : 0.0);
        o.has_pos = true;
        o.placement = p->description;
    } else if (!o.has_pos) {
        error = "host needs an airport (or a position: lat/lon/elevation)";
        return false;
    } else if (o.ground) {
        o.ground_m = o.elev_m;
    }
    if (o.ground) o.elev_m = o.ground_m + kFakeRefHeightM;
    return true;
}

void PrintUsage(const char* argv0) {
    std::printf(
        "Usage:\n"
        "  %s                              GUI in your browser\n"
        "  %s join <code> [--server host:port]\n"
        "  %s host --airport <ICAO> [--runway <name>] [--air] [--xplane <folder>]\n"
        "  %s host --pos <lat> <lon> <elev_m> [--hdg <deg>] [--ground] [--speed <kt>]\n"
        "        [--icao C172] [--engines 1] [--server host:port]\n"
        "Default server: 127.0.0.1:45000 (the local server/ binary). Use the same\n"
        "server your companion is set to.\n",
        argv0, argv0, argv0, argv0);
}

bool ParseArgs(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    const std::string mode = argv[1];
    int i = 2;
    if (mode == "join") {
        if (argc < 3) return false;
        o.code = argv[2];
        i = 3;
    } else if (mode == "host") {
        o.host = true;
    } else {
        return false;
    }
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has1 = i + 1 < argc;
        if (a == "--server" && has1) {
            o.server = argv[++i];
        } else if (a == "--pos" && i + 3 < argc) {
            o.lat = std::atof(argv[++i]);
            o.lon = std::atof(argv[++i]);
            o.elev_m = std::atof(argv[++i]);
            o.has_pos = true;
        } else if (a == "--hdg" && has1) {
            o.heading_deg = std::atof(argv[++i]);
        } else if (a == "--speed" && has1) {
            o.speed_kt = std::atof(argv[++i]);
        } else if (a == "--ground") {
            o.ground = true;
        } else if (a == "--airport" && has1) {
            o.airport = argv[++i];
        } else if (a == "--runway" && has1) {
            o.runway = argv[++i];
        } else if (a == "--air") {
            o.air = true;
        } else if (a == "--xplane" && has1) {
            o.xplane = argv[++i];
        } else if (a == "--icao" && has1) {
            o.icao = argv[++i];
        } else if (a == "--engines" && has1) {
            o.engines = std::clamp(std::atoi(argv[++i]), 1, ft::kControlsMaxEngines);
        } else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n", a.c_str());
            return false;
        }
    }
    std::string error;
    if (!ResolvePosition(o, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return true;
}

// "connect mode=host server=h:p lat=.. lon=.. elev=.. hdg=.. speed=.. ground=1"
// - the GUI's connect form, also usable from the console.
std::optional<Options> ParseConnect(const std::vector<std::string>& args, std::string& error) {
    Options o;
    for (const auto& a : args) {
        const auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = a.substr(0, eq), value = a.substr(eq + 1);
        if (key == "mode") o.host = value == "host";
        else if (key == "code") o.code = value;
        else if (key == "server" && !value.empty()) o.server = value;
        else if (key == "lat" && !value.empty()) o.lat = std::atof(value.c_str()), o.has_pos = true;
        else if (key == "lon") o.lon = std::atof(value.c_str());
        else if (key == "elev") o.elev_m = std::atof(value.c_str());
        else if (key == "hdg") o.heading_deg = std::atof(value.c_str());
        else if (key == "speed" && !value.empty()) o.speed_kt = std::atof(value.c_str());
        else if (key == "ground") o.ground = value == "1";
        else if (key == "icao" && !value.empty()) o.icao = value;
        else if (key == "airport") o.airport = value;
        else if (key == "runway") o.runway = value;
        else if (key == "air") o.air = value == "1";
        else if (key == "xplane") o.xplane = value;
    }
    if (!o.host && o.code.empty()) {
        error = "join needs the session code";
        return std::nullopt;
    }
    if (!ResolvePosition(o, error)) return std::nullopt;
    return o;
}

class FakePeer {
public:
    explicit FakePeer(const Options& o) : opt_(o) {
        ft::FillSecureRandom(reinterpret_cast<uint8_t*>(&sender_id_), sizeof(sender_id_));
        master_ = o.host;
        controls_.engines = o.engines;
        for (int e = 0; e < ft::kControlsMaxEngines; ++e) {
            controls_.throttle[e] = o.ground ? 0.0f : 0.6f;
            controls_.mixture[e] = 1.0f;
            controls_.prop[e] = 1.0f;
        }
        if (o.host) {
            flight_.lat = o.lat;
            flight_.lon = o.lon;
            flight_.elev_m = o.elev_m;
            flight_.heading_deg = o.heading_deg;
            flight_.on_ground = o.ground;
            flight_.ground_m = o.ground_m;
            if (!o.placement.empty()) Log("placed at %s", o.placement.c_str());
            const double kt = o.speed_kt >= 0.0 ? o.speed_kt : (o.ground ? 0.0 : 90.0);
            flight_.speed_mps = kt * kKtToMps;
            flight_.turn_dps = o.ground ? 0.0 : 1.0; // half standard rate: a wide, calm orbit
            wiggle_ = !o.ground;
        }
    }

    bool Start() {
        std::string host;
        uint16_t port = 0;
        if (!ft::SplitHostPort(opt_.server, host, port)) {
            Log("bad server '%s', expected host:port", opt_.server.c_str());
            return false;
        }
        rv_.on_session_ready = [this](const std::string& code, int id, const std::vector<uint8_t>& salt) {
            if (salt.size() != ft::SessionCrypto::kSaltSize) {
                Log("server sent a %zu-byte salt, expected %zu - giving up", salt.size(),
                    ft::SessionCrypto::kSaltSize);
                return;
            }
            std::array<uint8_t, ft::SessionCrypto::kSaltSize> s{};
            std::copy(salt.begin(), salt.end(), s.begin());
            crypto_.emplace(code, s);
            code_ = code;
            if (opt_.host) {
                Log("session ready, CODE: %s  (companion -> Shared Cockpit -> join as co-pilot)", code.c_str());
            } else {
                Log("joined session %s as peer %d", code.c_str(), id);
            }
            MaybeBegin();
        };
        rv_.on_peer_joined = [this](int id, const std::string& h, uint16_t p) {
            Log("peer %d joined from %s:%u", id, h.c_str(), p);
            peer_present_ = true;
            MaybeBegin();
        };
        rv_.on_peer_left = [this](int id) {
            Log("peer %d left", id);
            peer_present_ = false;
            running_ = false;
        };
        rv_.on_relay_received = [this](int, const std::vector<uint8_t>& bytes) { OnMessage(bytes); };
        rv_.on_direct_path_up = [](int id) { Log("direct P2P path to peer %d is up", id); };
        rv_.on_error = [](const std::string& m) { Log("server error: %s", m.c_str()); };
        rv_.on_disconnected = []() { Log("lost the rendezvous server (no answer for 40 s)"); };

        if (!rv_.Start(host, port)) {
            Log("couldn't open a UDP socket");
            return false;
        }
        Log("connecting to %s:%u as %s ...", host.c_str(), port, opt_.host ? "PILOT FLYING (host)" : "CO-PILOT");
        if (opt_.host) {
            rv_.CreateSession();
        } else {
            rv_.JoinSession(opt_.code);
        }
        return true;
    }

    void Stop() { rv_.Stop(); }

    // One main-loop tick (50 Hz).
    void Tick(double now, double dt) {
        rv_.PollIncoming();
        if (master_) {
            flight_.Step(dt);
            engine_.Step(dt, controls_.mixture[0]);
            PublishEngine();
        }

        if (running_ && master_ && now >= next_state_send_) {
            // 20 Hz, like SendSharedCockpitStateCallback - on a fixed grid,
            // the 50 Hz main loop would otherwise round every gap up to 60 ms.
            next_state_send_ = std::max(next_state_send_ + 0.05, now - 0.05);
            SendState(now);
        }
        if (now - rate_window_start_ >= 1.0) {
            pos_rate_ = position_count_ / (now - rate_window_start_);
            position_count_ = 0;
            rate_window_start_ = now;
        }
        if (console_summary_ && running_ && !master_ && now >= next_summary_) {
            next_summary_ = now + 3.0;
            PrintMasterSummary();
        }
    }

    void SetConsoleSummary(bool on) { console_summary_ = on; }

    // --- commands, run on the main thread ---

    void CmdTake() {
        if (master_) {
            Log("already pilot flying");
            return;
        }
        ft::OwnershipClaimMessage claim;
        claim.category = ft::DatarefCategory::kFlight;
        const auto encoded = ft::EncodeOwnershipClaimMessage(claim);
        for (int i = 0; i < 3; ++i) Send(encoded); // like DatarefSync::ClaimOwnership
        BecomeMaster();
    }

    void CmdSet(const std::string& name, const std::string& type_tag, const std::string& text) {
        ft::DatarefValueType type;
        if (!type_tag.empty()) {
            const auto t = ParseTypeTag(type_tag);
            if (!t) {
                Log("unknown type '%s' (i, f, d, ia, fa)", type_tag.c_str());
                return;
            }
            type = *t;
        } else if (auto it = values_.find(name); it != values_.end()) {
            type = it->second.type;
        } else if (const auto* preset = FindPreset(name)) {
            type = preset->type;
        } else {
            type = text.find(',') != std::string::npos   ? ft::DatarefValueType::kFloatArray
                   : text.find('.') != std::string::npos ? ft::DatarefValueType::kFloat
                                                         : ft::DatarefValueType::kInt;
            Log("type of %s not seen yet, guessing '%s' - wrong type is ignored by X-Plane, "
                "use 'set <name> <i|f|d|ia|fa> <value>' to be sure",
                name.c_str(), TypeTag(type));
        }
        const auto value = ParseValue(type, text);
        if (!value) {
            Log("can't parse '%s' as %s", text.c_str(), TypeTag(type));
            return;
        }
        SetValue(name, *value);
    }

    // Scalar, or element 0 of an array with the rest kept - what a switch
    // in the GUI or a scenario changes. An array not seen yet goes out with
    // just that one element, which the plugin writes alone.
    void CmdSetFirst(const std::string& name, double number) {
        ft::DatarefValue v;
        if (auto it = values_.find(name); it != values_.end()) {
            v = it->second;
        } else if (const auto* preset = FindPreset(name)) {
            v.type = preset->type;
        } else {
            Log("%s: type unknown - use 'set <name> <type> <value>'", name.c_str());
            return;
        }
        switch (v.type) {
            case ft::DatarefValueType::kInt: v.int_value = static_cast<int>(std::lround(number)); break;
            case ft::DatarefValueType::kFloat: v.float_value = static_cast<float>(number); break;
            case ft::DatarefValueType::kDouble: v.double_value = number; break;
            case ft::DatarefValueType::kIntArray:
                v.array_len = std::max(v.array_len, 1);
                v.int_array[0] = static_cast<int>(std::lround(number));
                break;
            case ft::DatarefValueType::kFloatArray:
                v.array_len = std::max(v.array_len, 1);
                v.float_array[0] = static_cast<float>(number);
                break;
        }
        SetValue(name, v);
        if (master_ && name == kIgnitionKeyRef) engine_.key = published_key_ = std::clamp(static_cast<int>(std::lround(number)), 0, 4);
        if (master_ && name == kEngineRunningRef) engine_.running = published_running_ = number != 0.0;
    }

    void CmdScenario(int index) {
        const int count = static_cast<int>(sizeof(sc_fake::kC172Scenarios) / sizeof(sc_fake::kC172Scenarios[0]));
        if (index < 0 || index >= count) {
            Log("no scenario %d", index);
            return;
        }
        const auto& s = sc_fake::kC172Scenarios[index];
        Log("scenario '%s'", s.label);
        for (int i = 0; i < s.count; ++i) CmdSetFirst(s.settings[i].name, s.settings[i].value);
    }

    void CmdCommand(const std::string& name, ft::CommandPhase phase) {
        ft::CommandMessage msg;
        msg.sender_id = sender_id_;
        msg.sequence = ++command_sequence_;
        msg.phase = phase;
        msg.name = name;
        Send(ft::EncodeCommandMessage(msg));
        Log("command %s %s", name.c_str(), phase == ft::CommandPhase::kBegin ? "BEGIN" : "END");
        // Pressed in both cockpits - and this one's "simulator" is us.
        if (master_) engine_.OnCommand(name, phase == ft::CommandPhase::kBegin);
    }

    void CmdFlight(const std::string& what, double value) {
        if (what == "speed") flight_.speed_mps = std::max(0.0, value) * kKtToMps;
        if (what == "turn") flight_.turn_dps = value;
        if (what == "vs") flight_.vs_mps = value / kMToFt / 60.0;
        if (what == "thr") {
            for (int e = 0; e < ft::kControlsMaxEngines; ++e) controls_.throttle[e] = static_cast<float>(value);
        }
        if (what == "mix") {
            for (int e = 0; e < ft::kControlsMaxEngines; ++e) controls_.mixture[e] = static_cast<float>(value);
        }
        if (what == "brake") controls_.left_brake = controls_.right_brake = static_cast<float>(value);
        if (!master_) Log("(only sent while this side is pilot flying - 'take' first)");
    }

    void CmdGround(bool on) {
        flight_.on_ground = on;
        if (on) {
            flight_.turn_dps = 0.0;
            flight_.vs_mps = 0.0;
            if (!std::isnan(flight_.ground_m)) flight_.elev_m = flight_.ground_m + flight_.ref_m;
        }
        Log("%s - elevation stays %.1f m MSL", on ? "on ground" : "airborne", flight_.elev_m);
    }

    void CmdWiggle(bool on) {
        wiggle_ = on;
        if (!on) controls_.yoke_pitch = controls_.yoke_roll = controls_.yoke_heading = 0.0f;
        Log("yoke/rudder wiggle %s", on ? "on" : "off");
    }

    void CmdQuiet(bool on) {
        quiet_ = on;
        Log("dataref/command log %s", on ? "off" : "on");
    }

    void CmdResync() {
        if (master_) {
            ResendAll();
        } else {
            Send(WithMagic(kResyncMagic, nullptr, 0));
            Log("asked the pilot flying for the full cockpit state");
        }
    }

    void CmdList() const {
        std::string out = std::to_string(values_.size()) + " dataref(s) known:\n";
        for (const auto& [name, v] : values_) {
            char line[200];
            std::snprintf(line, sizeof(line), "  %-60s %-2s %s\n", name.c_str(), TypeTag(v.type),
                          DescribeValue(v).c_str());
            out += line;
        }
        Print(out);
    }

    void CmdStatus() const {
        Log("code %s | peer %s | %s | path %s | server rtt %s | %s", code_.empty() ? "-" : code_.c_str(),
            peer_present_ ? "present" : "none", master_ ? "PILOT FLYING" : "CO-PILOT",
            rv_.DirectPeerCount() ? "direct" : "relay",
            rv_.HasRtt() ? (std::to_string(rv_.Rtt().count()) + " ms").c_str() : "?",
            master_ ? DescribeFlight().c_str() : "");
    }

    std::string StateJson() const {
        std::ostringstream j;
        j << "{\"connected\":true,\"mode\":" << JsonString(opt_.host ? "host" : "join")
          << ",\"code\":" << JsonString(code_) << ",\"server\":" << JsonString(opt_.server)
          << ",\"session\":" << (crypto_ ? "true" : "false") << ",\"peer\":" << (peer_present_ ? "true" : "false")
          << ",\"running\":" << (running_ ? "true" : "false") << ",\"role\":" << JsonString(master_ ? "pf" : "cp")
          << ",\"path\":" << JsonString(rv_.DirectPeerCount() ? "direct" : "relay")
          << ",\"rtt\":" << (rv_.HasRtt() ? std::to_string(rv_.Rtt().count()) : "null")
          << ",\"quiet\":" << (quiet_ ? "true" : "false");

        j << ",\"flight\":{\"lat\":" << JsonNumber(flight_.lat) << ",\"lon\":" << JsonNumber(flight_.lon)
          << ",\"alt_ft\":" << JsonNumber(flight_.elev_m * kMToFt) << ",\"hdg\":" << JsonNumber(flight_.heading_deg)
          << ",\"gs_kt\":" << JsonNumber(flight_.speed_mps / kKtToMps)
          << ",\"vs_fpm\":" << JsonNumber(flight_.vs_mps * kMToFt * 60.0) << ",\"turn\":" << JsonNumber(flight_.turn_dps)
          << ",\"ground\":" << (flight_.on_ground ? "true" : "false") << ",\"wiggle\":" << (wiggle_ ? "true" : "false")
          << ",\"key\":" << engine_.key << ",\"running\":" << (engine_.running ? "true" : "false")
          << ",\"cranking\":" << (engine_.Cranking() ? "true" : "false")
          << ",\"rpm\":" << JsonNumber(engine_.Rpm(controls_.throttle[0])) << "}";

        j << ",\"pf\":";
        if (!master_ && last_master_packet_) {
            const ft::AircraftStatePacket& p = *last_master_packet_;
            const std::string icao(p.icao_type, strnlen(p.icao_type, sizeof(p.icao_type)));
            j << "{\"icao\":" << JsonString(icao) << ",\"lat\":" << JsonNumber(p.latitude)
              << ",\"lon\":" << JsonNumber(p.longitude) << ",\"alt_ft\":" << JsonNumber(p.elevation_m * kMToFt)
              << ",\"hdg\":" << JsonNumber(p.heading_deg)
              << ",\"gs_kt\":" << JsonNumber(std::hypot(p.velocity_x_mps, p.velocity_z_mps) / kKtToMps)
              << ",\"vs_fpm\":" << JsonNumber(p.velocity_y_mps * kMToFt * 60.0)
              << ",\"pitch\":" << JsonNumber(p.pitch_deg) << ",\"roll\":" << JsonNumber(p.roll_deg)
              << ",\"ground\":" << ((p.flags & ft::StateFlags::kOnGround) ? "true" : "false")
              << ",\"rpm\":" << JsonNumber(p.prop_rpm) << ",\"rate\":" << JsonNumber(pos_rate_)
              << ",\"age\":" << JsonNumber(Now() - last_master_packet_at_) << "}";
        } else {
            j << "null";
        }

        // The controls shown: what we receive as co-pilot, what we send as PF.
        const ft::ControlsState* c = master_ ? &controls_ : (last_controls_ ? &*last_controls_ : nullptr);
        j << ",\"ctl\":";
        if (c) {
            j << "{\"yp\":" << JsonNumber(c->yoke_pitch) << ",\"yr\":" << JsonNumber(c->yoke_roll)
              << ",\"yh\":" << JsonNumber(c->yoke_heading) << ",\"lb\":" << JsonNumber(c->left_brake)
              << ",\"rb\":" << JsonNumber(c->right_brake) << ",\"thr\":" << JsonNumber(c->throttle[0])
              << ",\"mix\":" << JsonNumber(c->mixture[0]) << ",\"prop\":" << JsonNumber(c->prop[0])
              << ",\"age\":" << JsonNumber(master_ ? 0.0 : Now() - last_controls_at_) << "}";
        } else {
            j << "null";
        }

        j << ",\"values\":{";
        bool first = true;
        for (const auto& [name, v] : values_) {
            j << (first ? "" : ",") << JsonString(name) << ":{\"t\":\"" << TypeTag(v.type) << "\",\"v\":" << ValueJson(v)
              << "}";
            first = false;
        }
        j << "}}";
        return j.str();
    }

private:
    void MaybeBegin() {
        if (running_ || !crypto_ || !peer_present_) return;
        running_ = true;
        Log("Shared Cockpit running - this side is %s", master_ ? "PILOT FLYING" : "CO-PILOT");
        if (!master_) {
            // Same as StartSharedCockpit for a client: ask for the full state.
            Send(WithMagic(kResyncMagic, nullptr, 0));
        } else {
            ResendAll();
        }
    }

    void BecomeMaster() {
        // Carry on from where the plugin's aircraft was, with its velocity -
        // like ReleasePhysicsOverride does on a real handover.
        if (last_master_packet_) {
            const ft::AircraftStatePacket& p = *last_master_packet_;
            flight_.lat = p.latitude;
            flight_.lon = p.longitude;
            flight_.elev_m = p.elevation_m;
            flight_.on_ground = (p.flags & ft::StateFlags::kOnGround) != 0;
            flight_.ground_m = p.agl_m >= 0.0f ? p.elevation_m - p.agl_m : NAN;
            flight_.ref_m = p.ref_height_agl_m >= 0.0f ? p.ref_height_agl_m : kFakeRefHeightM;
            const double vx = p.velocity_x_mps, vz = p.velocity_z_mps; // OpenGL: x east, z south
            flight_.speed_mps = std::hypot(vx, vz);
            flight_.heading_deg = flight_.speed_mps > 1.0
                                      ? std::fmod(std::atan2(vx, -vz) * 180.0 / kPi + 360.0, 360.0)
                                      : p.heading_deg;
            flight_.vs_mps = flight_.on_ground ? 0.0 : p.velocity_y_mps;
            flight_.turn_dps = 0.0;
        }
        if (last_controls_) {
            controls_ = *last_controls_;
            controls_.engines = std::max(controls_.engines, 1);
        }
        const auto first_int = [this](const char* name) {
            const auto it = values_.find(name);
            if (it == values_.end()) return 0;
            const ft::DatarefValue& v = it->second;
            return v.type == ft::DatarefValueType::kIntArray && v.array_len ? v.int_array[0] : v.int_value;
        };
        engine_ = FakeEngine{};
        engine_.key = std::clamp(first_int(kIgnitionKeyRef), 0, 3);
        engine_.running = first_int(kEngineRunningRef) != 0;
        published_key_ = engine_.key;
        published_running_ = engine_.running;
        wiggle_ = false;
        master_ = true;
        Log("I HAVE CONTROL - now pilot flying: %s", DescribeFlight().c_str());
        ResendAll(); // a new master sends everything once (DatarefSync::SetAuthority)
    }

    // Sends the key position and the running state when they change, like a
    // real master's DatarefSync would.
    void PublishEngine() {
        if (engine_.key != published_key_) {
            published_key_ = engine_.key;
            CmdSetFirst(kIgnitionKeyRef, engine_.key);
        }
        if (engine_.running != published_running_) {
            published_running_ = engine_.running;
            CmdSetFirst(kEngineRunningRef, engine_.running ? 1 : 0);
            Log("ENGINE %s", engine_.running ? "STARTED" : "STOPPED");
        }
    }

    std::string DescribeFlight() const {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.5f %.5f %.0f ft hdg %.0f %.0f kt vs %.0f fpm turn %.1f deg/s%s",
                      flight_.lat, flight_.lon, flight_.elev_m * kMToFt, flight_.heading_deg,
                      flight_.speed_mps / kKtToMps, flight_.vs_mps * kMToFt * 60.0, flight_.turn_dps,
                      flight_.on_ground ? " ON GROUND" : "");
        return buf;
    }

    static std::vector<uint8_t> WithMagic(uint32_t magic, const void* data, size_t len) {
        std::vector<uint8_t> out(sizeof(magic) + len);
        std::memcpy(out.data(), &magic, sizeof(magic));
        if (len) std::memcpy(out.data() + sizeof(magic), data, len);
        return out;
    }

    void Send(const std::vector<uint8_t>& plain) {
        if (!crypto_ || plain.empty()) return;
        const auto envelope = crypto_->Seal(plain);
        rv_.SendRelay(envelope.data(), envelope.size());
    }

    void SendDataref(const std::string& name, const ft::DatarefValue& value) {
        ft::DatarefSyncMessage msg;
        msg.name = name;
        msg.value = value;
        Send(ft::EncodeDatarefSyncMessage(msg));
    }

    void SetValue(const std::string& name, const ft::DatarefValue& value) {
        if (!running_) Log("(no peer yet - only kept here)");
        SendDataref(name, value);
        values_[name] = value;
        Log("%s %s = %s", master_ ? "SENT" : "REQUESTED", name.c_str(), DescribeValue(value).c_str());
    }

    void ResendAll() {
        for (const auto& [name, v] : values_) SendDataref(name, v);
        if (!values_.empty()) Log("sent all %zu known dataref value(s)", values_.size());
    }

    void SendState(double now) {
        ft::AircraftStatePacket p;
        p.sender_id = sender_id_;
        p.sequence = state_sequence_++;
        p.latitude = flight_.lat;
        p.longitude = flight_.lon;
        p.elevation_m = flight_.elev_m;
        p.heading_deg = static_cast<float>(flight_.heading_deg);
        p.pitch_deg = flight_.on_ground ? 0.0f : 2.0f;
        p.roll_deg = static_cast<float>(flight_.BankDeg());
        p.gear_ratio = 1.0f;
        p.engine_ratio = engine_.running ? controls_.throttle[0] : 0.0f;
        p.light_bits = ft::LightBits::kBeacon | ft::LightBits::kNav;
        std::memcpy(p.icao_type, opt_.icao.data(), std::min<size_t>(opt_.icao.size(), sizeof(p.icao_type)));
        std::memcpy(p.callsign, "FAKE", 4);
        const double h = flight_.heading_deg * kPi / 180.0;
        p.velocity_x_mps = static_cast<float>(flight_.speed_mps * std::sin(h));
        p.velocity_z_mps = static_cast<float>(-flight_.speed_mps * std::cos(h));
        p.velocity_y_mps = static_cast<float>(flight_.on_ground ? 0.0 : flight_.vs_mps);
        p.yaw_rate_dps = static_cast<float>(flight_.turn_dps);
        p.prop_rpm = engine_.Rpm(controls_.throttle[0]);
        if (flight_.on_ground) p.flags |= ft::StateFlags::kOnGround;
        if (!std::isnan(flight_.ground_m)) {
            // Lets the co-pilot put the aircraft on ITS ground, so the
            // field elevation from apt.dat only has to be roughly right.
            p.agl_m = static_cast<float>(std::max(0.0, flight_.elev_m - flight_.ground_m));
            p.ref_height_agl_m = static_cast<float>(flight_.ref_m);
        }

        if (wiggle_) {
            controls_.yoke_pitch = static_cast<float>(0.3 * std::sin(now * 2.0 * kPi / 4.0));
            controls_.yoke_roll = static_cast<float>(0.4 * std::sin(now * 2.0 * kPi / 6.0));
            controls_.yoke_heading = static_cast<float>(0.3 * std::sin(now * 2.0 * kPi / 8.0));
        }
        p.yoke_pitch_ratio = controls_.yoke_pitch;
        p.yoke_roll_ratio = controls_.yoke_roll;
        p.yoke_heading_ratio = controls_.yoke_heading;

        const auto* bytes = reinterpret_cast<const uint8_t*>(&p);
        Send(std::vector<uint8_t>(bytes, bytes + sizeof(p)));
        controls_.sequence = ++controls_sequence_;
        Send(ft::EncodeControlsState(controls_));
        if (now >= next_engine_send_) {
            next_engine_send_ = now + 0.2;
            Send(ft::EncodeEngineState(FakeEngineGauges()));
        }
    }

    // Rough C172-like gauges for the engine message, from throttle and
    // whether the fake engine runs - enough to see them move.
    ft::EngineState FakeEngineGauges() {
        ft::EngineState st;
        st.sequence = ++engine_sequence_;
        st.engines = std::min(controls_.engines, ft::kEngineSyncMaxEngines);
        const bool on = engine_.running;
        const float thr = std::clamp(controls_.throttle[0], 0.0f, 1.0f);
        for (int i = 0; i < st.engines; ++i) {
            ft::EngineGauges& e = st.engine[i];
            e.prop_rad_s = e.engine_rad_s = engine_.Rpm(thr) * 2.0f * static_cast<float>(kPi) / 60.0f;
            e.egt_c = on ? 400.0f + 300.0f * thr : 20.0f;
            e.cht_c = on ? 120.0f + 60.0f * thr : 20.0f;
            e.oil_temp = on ? 160.0f + 30.0f * thr : 60.0f; // the C172 has it in deg F
            e.oil_press_psi = on ? 50.0f + 25.0f * thr : 0.0f;
            e.fuel_flow_kg_s = on ? 0.003f + 0.010f * thr : 0.0f;
            e.manifold_inhg = on ? 12.0f + 16.0f * thr : 29.9f;
        }
        st.tanks = 2;
        st.fuel_kg[0] = st.fuel_kg[1] = 60.0f;
        return st;
    }

    void PrintMasterSummary() const {
        if (!last_master_packet_) {
            Log("no position from the pilot flying yet");
            return;
        }
        const ft::AircraftStatePacket& p = *last_master_packet_;
        const double gs_kt = std::hypot(p.velocity_x_mps, p.velocity_z_mps) / kKtToMps;
        char ctl[160] = "no controls stream";
        if (last_controls_) {
            const ft::ControlsState& c = *last_controls_;
            std::snprintf(ctl, sizeof(ctl), "yoke p%+.2f r%+.2f rud%+.2f brk %.2f/%.2f thr %.2f mix %.2f prop %.2f",
                          c.yoke_pitch, c.yoke_roll, c.yoke_heading, c.left_brake, c.right_brake, c.throttle[0],
                          c.mixture[0], c.prop[0]);
        }
        Log("PF %.8s %.5f %.5f %.0f ft hdg %.0f gs %.0f kt %s rpm %.0f | %.1f pos/s | %s", p.icao_type, p.latitude,
            p.longitude, p.elevation_m * kMToFt, p.heading_deg, gs_kt,
            (p.flags & ft::StateFlags::kOnGround) ? "GND" : "AIR", p.prop_rpm, pos_rate_, ctl);
    }

    void OnMessage(const std::vector<uint8_t>& bytes) {
        uint32_t raw_magic = 0;
        if (bytes.size() >= 4) std::memcpy(&raw_magic, bytes.data(), 4);
        if (raw_magic == ft::kWeatherStateMagic) {
            if (!seen_weather_) Log("weather from the pilot flying (not applied here)");
            seen_weather_ = true;
            return;
        }
        if (!crypto_) return;
        const auto opened = crypto_->Open(bytes);
        if (!opened) {
            Log("undecryptable message (%zu bytes) - different session/key?", bytes.size());
            return;
        }
        const std::vector<uint8_t>& plain = *opened;
        uint32_t magic = 0;
        if (plain.size() >= 4) std::memcpy(&magic, plain.data(), 4);
        const double now = Now();

        if (magic == ft::kAircraftStateMagic) {
            if (master_) return;
            if (plain.size() < ft::kAircraftStateMinSize) return;
            ft::AircraftStatePacket p;
            std::memcpy(&p, plain.data(), std::min(plain.size(), sizeof(p)));
            if (!last_master_packet_) {
                Log("first position from the pilot flying");
                next_summary_ = now + 0.5;
            }
            if (!last_master_packet_ || static_cast<int32_t>(p.sequence - last_master_packet_->sequence) > 0 ||
                p.sender_id != last_master_packet_->sender_id) {
                last_master_packet_ = p;
                last_master_packet_at_ = now;
                ++position_count_;
            }
        } else if (magic == ft::kControlsSyncMagic) {
            if (master_) return;
            if (auto c = ft::DecodeControlsState(plain.data(), plain.size())) {
                last_controls_ = c;
                last_controls_at_ = now;
            }
        } else if (magic == ft::kDatarefSyncMagic) {
            ft::DatarefSyncMessage msg;
            if (!ft::DecodeDatarefSyncMessage(plain.data(), plain.size(), msg)) return;
            const auto it = values_.find(msg.name);
            const bool changed = it == values_.end() || it->second != msg.value;
            values_[msg.name] = msg.value;
            if (master_) {
                // A real master's sim applies the co-pilot's request and then
                // reports it as its own state - echo it back.
                SendDataref(msg.name, msg.value);
                if (changed && !quiet_) {
                    Log("co-pilot set %s = %s (applied + echoed)", msg.name.c_str(), DescribeValue(msg.value).c_str());
                }
            } else if (changed && !quiet_) {
                Log("PF  %s = %s", msg.name.c_str(), DescribeValue(msg.value).c_str());
            }
        } else if (magic == ft::kOwnershipClaimMagic) {
            ft::OwnershipClaimMessage claim;
            if (!ft::DecodeOwnershipClaimMessage(plain.data(), plain.size(), claim)) return;
            if (claim.category == ft::DatarefCategory::kFlight && master_) {
                master_ = false;
                last_master_packet_.reset();
                last_controls_.reset();
                Log("PEER TOOK CONTROL - this side is co-pilot now");
            }
        } else if (magic == ft::kCommandSyncMagic) {
            if (auto c = ft::DecodeCommandMessage(plain.data(), plain.size())) {
                if (!command_dedup_.Accept(c->sender_id, c->sequence)) return;
                if (!quiet_) {
                    Log("button %s %s", c->name.c_str(), c->phase == ft::CommandPhase::kBegin ? "pressed" : "released");
                }
                if (master_) engine_.OnCommand(c->name, c->phase == ft::CommandPhase::kBegin);
            }
        } else if (magic == kResyncMagic) {
            if (now - last_resync_request_ < 1.0) return; // same request via relay and direct
            last_resync_request_ = now;
            Log("peer asked for a full resync");
            if (master_) ResendAll();
        } else if (magic == kChecklistMagic) {
            const std::string text(plain.begin() + 4, plain.end());
            if (text != last_checklist_) {
                last_checklist_ = text;
                Log("checklist state: %s", text.c_str());
            }
        } else if (magic == kDigestMagic) {
            // Desync check digest - nothing to compare against without a sim.
        } else if (magic == ft::kTimeSyncMagic) {
            if (!seen_time_) Log("sim time from the pilot flying (not applied here)");
            seen_time_ = true;
        } else if (magic == ft::kEngineSyncMagic) {
            if (const auto st = ft::DecodeEngineState(plain.data(), plain.size()); st && st->engines > 0 && !seen_engines_) {
                seen_engines_ = true;
                Log("engine gauges from the pilot flying: %.0f rpm, EGT %.0f C, oil %.0f psi (shown once)",
                    st->engine[0].prop_rad_s * 60.0 / (2.0 * kPi), st->engine[0].egt_c, st->engine[0].oil_press_psi);
            }
        } else {
            Log("unknown message, magic 0x%08x, %zu bytes", magic, plain.size());
        }
    }

    Options opt_;
    ft::RendezvousClient rv_;
    std::optional<ft::SessionCrypto> crypto_;
    std::string code_;
    uint32_t sender_id_ = 0;
    bool peer_present_ = false;
    bool running_ = false;
    bool master_ = false;
    bool wiggle_ = false;
    bool quiet_ = false;
    bool console_summary_ = true;
    bool seen_weather_ = false;
    bool seen_time_ = false;

    FakeFlight flight_;
    FakeEngine engine_;
    int published_key_ = 0;
    bool published_running_ = false;
    ft::ControlsState controls_;
    uint32_t state_sequence_ = 0;
    uint32_t controls_sequence_ = 0;
    uint32_t command_sequence_ = 0;
    double next_state_send_ = 0.0;
    double last_resync_request_ = -100.0;

    std::optional<ft::AircraftStatePacket> last_master_packet_;
    double last_master_packet_at_ = 0.0;
    std::optional<ft::ControlsState> last_controls_;
    double last_controls_at_ = 0.0;
    int position_count_ = 0;
    double rate_window_start_ = 0.0;
    double pos_rate_ = 0.0;
    double next_summary_ = 0.0;

    std::map<std::string, ft::DatarefValue> values_;
    ft::CommandDedup command_dedup_;
    double next_engine_send_ = 0.0;
    bool seen_engines_ = false;
    uint32_t engine_sequence_ = 0;
    std::string last_checklist_;
};

const char* kHelp =
    "Commands:\n"
    "  connect mode=join code=<code> [server=host:port]\n"
    "  connect mode=host airport=<ICAO> [runway=<name>] [air=1] [xplane=<folder>] [server=host:port]\n"
    "  connect mode=host lat=<..> lon=<..> elev=<m> [hdg=<deg>] [ground=1] [server=host:port]\n"
    "  disconnect\n"
    "  take                        claim the controls (become pilot flying)\n"
    "  set <dataref> [type] <val>  move a switch; type i|f|d|ia|fa (arrays: 1,0,0)\n"
    "  set0 <dataref> <val>        set a scalar / element 0 of an array\n"
    "  scenario <0|1|2>            C172: cold & dark / before start / ready for takeoff\n"
    "  cmd <command> [hold_s]      press a button (default hold 0.2 s)\n"
    "  begin|end <command>         press / release separately\n"
    "  list                        all datarefs seen so far with their values\n"
    "  resync                      co-pilot: ask for the full state, PF: send it\n"
    "  status                      session, role, path, fake flight\n"
    "  quiet on|off                hide dataref/button lines\n"
    " as pilot flying:\n"
    "  speed <kt>  turn <deg/s>  vs <fpm>  thr|mix <0..1>  brake <0..1>\n"
    "  ground on|off               wiggle on|off  (yoke/rudder sine)\n"
    "  wait <s>                    pause (for piped scripts)\n"
    "  quit\n";

// Owns the (optional) FakePeer and runs every command on the main thread;
// console and GUI just queue command lines.
class App {
public:
    explicit App(bool gui) : gui_(gui) {}

    // Any thread.
    void Submit(const std::string& line) { Later(0.0, line); }

    bool quit() const { return quit_; }
    void RequestQuit() { quit_ = true; }

    void Connect(const Options& o) {
        Disconnect();
        peer_ = std::make_unique<FakePeer>(o);
        peer_->SetConsoleSummary(!gui_);
        if (!peer_->Start()) peer_.reset();
    }

    // Main thread, every tick.
    void Tick(double now, double dt) {
        std::vector<std::pair<double, std::string>> due;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            auto split = std::stable_partition(queue_.begin(), queue_.end(),
                                               [&](const auto& item) { return item.first > now; });
            due.assign(split, queue_.end());
            queue_.erase(split, queue_.end());
        }
        for (const auto& item : due) Execute(item.second);
        if (peer_) peer_->Tick(now, dt);

        if (gui_ && now >= next_snapshot_) {
            next_snapshot_ = now + 0.15;
            std::string state = peer_ ? peer_->StateJson() : std::string("{\"connected\":false}");
            std::string log;
            uint64_t seq = 0;
            {
                std::lock_guard<std::mutex> lock(g_log_mutex);
                seq = g_log_seq;
                for (const auto& l : g_log_lines) log += (log.empty() ? "" : ",") + JsonString(l);
            }
            state.pop_back(); // the closing brace
            state += ",\"log_seq\":" + std::to_string(seq) + ",\"log\":[" + log + "]}";
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_ = std::move(state);
        }
    }

    std::string Snapshot() const {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        return snapshot_;
    }

    void Shutdown() { Disconnect(); }

private:
    void Disconnect() {
        if (!peer_) return;
        peer_->Stop();
        peer_.reset();
        Log("disconnected");
    }

    void Later(double delay, const std::string& line) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back({Now() + delay, line});
    }

    void Execute(const std::string& line) {
        std::istringstream in(line);
        std::string cmd;
        if (!(in >> cmd) || cmd[0] == '#') return;
        std::vector<std::string> args;
        for (std::string a; in >> a;) args.push_back(a);
        const auto num = [&](size_t i, double fallback) {
            return i < args.size() ? std::atof(args[i].c_str()) : fallback;
        };

        if (cmd == "quit" || cmd == "exit") {
            quit_ = true;
            return;
        }
        if (cmd == "help") {
            Print(kHelp);
            return;
        }
        if (cmd == "connect") {
            std::string error;
            if (auto o = ParseConnect(args, error)) {
                Connect(*o);
            } else {
                Log("can't connect: %s", error.c_str());
            }
            return;
        }
        if (cmd == "disconnect") {
            Disconnect();
            return;
        }
        if (!peer_) {
            Log("not connected - 'connect ...' first (see 'help')");
            return;
        }
        FakePeer& p = *peer_;
        if (cmd == "take") {
            p.CmdTake();
        } else if (cmd == "set" && (args.size() == 2 || args.size() == 3)) {
            p.CmdSet(args[0], args.size() == 3 ? args[1] : "", args.back());
        } else if (cmd == "set0" && args.size() == 2) {
            p.CmdSetFirst(args[0], num(1, 0.0));
        } else if (cmd == "scenario" && args.size() == 1) {
            p.CmdScenario(static_cast<int>(num(0, -1)));
        } else if (cmd == "cmd" && !args.empty()) {
            p.CmdCommand(args[0], ft::CommandPhase::kBegin);
            Later(num(1, 0.2), "end " + args[0]);
        } else if ((cmd == "begin" || cmd == "end") && args.size() == 1) {
            p.CmdCommand(args[0], cmd == "begin" ? ft::CommandPhase::kBegin : ft::CommandPhase::kEnd);
        } else if (cmd == "list") {
            p.CmdList();
        } else if (cmd == "resync") {
            p.CmdResync();
        } else if (cmd == "status") {
            p.CmdStatus();
        } else if ((cmd == "quiet" || cmd == "ground" || cmd == "wiggle") && args.size() == 1) {
            const bool on = args[0] == "on" || args[0] == "1";
            if (cmd == "quiet") p.CmdQuiet(on);
            if (cmd == "ground") p.CmdGround(on);
            if (cmd == "wiggle") p.CmdWiggle(on);
        } else if ((cmd == "speed" || cmd == "turn" || cmd == "vs" || cmd == "thr" || cmd == "mix" ||
                    cmd == "brake") &&
                   args.size() == 1) {
            p.CmdFlight(cmd, num(0, 0.0));
        } else {
            Log("? '%s' - type 'help'", line.c_str());
        }
    }

    bool gui_;
    std::atomic<bool> quit_{false};
    std::unique_ptr<FakePeer> peer_;
    std::mutex queue_mutex_;
    std::vector<std::pair<double, std::string>> queue_;
    mutable std::mutex snapshot_mutex_;
    std::string snapshot_ = "{\"connected\":false,\"log_seq\":0,\"log\":[]}";
    double next_snapshot_ = 0.0;
};

std::string PresetJson() {
    std::ostringstream j;
    j << "{\"datarefs\":[";
    bool first = true;
    for (const auto& d : sc_fake::kC172Datarefs) {
        j << (first ? "" : ",") << "{\"name\":" << JsonString(d.name) << ",\"t\":\"" << TypeTag(d.type)
          << "\",\"group\":" << JsonString(d.group) << ",\"label\":" << JsonString(d.label)
          << ",\"kind\":" << JsonString(d.kind) << "}";
        first = false;
    }
    j << "],\"commands\":[";
    first = true;
    for (const auto& c : sc_fake::kC172Commands) {
        j << (first ? "" : ",") << "{\"name\":" << JsonString(c.name) << ",\"group\":" << JsonString(c.group)
          << ",\"label\":" << JsonString(c.label) << "}";
        first = false;
    }
    j << "],\"scenarios\":[";
    first = true;
    for (const auto& s : sc_fake::kC172Scenarios) {
        j << (first ? "" : ",") << JsonString(s.label);
        first = false;
    }
    j << "]}";
    return j.str();
}

// --- the GUI's HTTP server: 127.0.0.1 only, one thread per connection ---

#if defined(_WIN32)
using SockT = SOCKET;
constexpr SockT kBadSock = INVALID_SOCKET;
void CloseSock(SockT s) { closesocket(s); }
#else
using SockT = int;
constexpr SockT kBadSock = -1;
void CloseSock(SockT s) { close(s); }
#endif

void SendAll(SockT s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = static_cast<int>(send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0));
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

void Respond(SockT s, int status, const char* type, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : "Not Found";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
                             "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    SendAll(s, head + body);
}

std::string HeaderValue(const std::string& head, const std::string& name) {
    std::istringstream in(head);
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key != name) continue;
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        return value;
    }
    return "";
}

void HandleConnection(SockT s, App& app, uint16_t port, const std::string& preset_json) {
#if defined(_WIN32)
    DWORD timeout_ms = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    std::string data;
    char buf[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos && data.size() < 65536) {
        const int n = static_cast<int>(recv(s, buf, sizeof(buf), 0));
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
        header_end = data.find("\r\n\r\n");
    }
    if (header_end == std::string::npos) {
        CloseSock(s);
        return;
    }
    const std::string head = data.substr(0, header_end);
    const size_t content_length =
        std::min<size_t>(std::strtoul(HeaderValue(head, "content-length").c_str(), nullptr, 10), 65536);
    std::string body = data.substr(header_end + 4);
    while (body.size() < content_length) {
        const int n = static_cast<int>(recv(s, buf, sizeof(buf), 0));
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
    }

    std::istringstream request_line(head.substr(0, head.find("\r\n")));
    std::string method, path;
    request_line >> method >> path;

    // Only this machine's own page: the Host check guards against DNS
    // rebinding, the custom header against form posts from other sites.
    const std::string host = HeaderValue(head, "host");
    const std::string port_suffix = ":" + std::to_string(port);
    if (host != "127.0.0.1" + port_suffix && host != "localhost" + port_suffix) {
        Respond(s, 403, "text/plain", "forbidden");
    } else if (method == "GET" && (path == "/" || path == "/index.html")) {
        Respond(s, 200, "text/html; charset=utf-8", sc_fake::GuiPage());
    } else if (method == "GET" && path == "/api/state") {
        Respond(s, 200, "application/json", app.Snapshot());
    } else if (method == "GET" && path == "/api/preset") {
        Respond(s, 200, "application/json", preset_json);
    } else if (method == "POST" && path == "/api/cmd" && HeaderValue(head, "x-fake-peer") == "1") {
        std::istringstream lines(body);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) app.Submit(line);
        }
        Respond(s, 200, "text/plain", "ok");
    } else {
        Respond(s, 404, "text/plain", "not found");
    }
    CloseSock(s);
}

// Returns the bound port, 0 on failure.
uint16_t StartGuiServer(App& app) {
    const SockT listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kBadSock) return 0;
#if !defined(_WIN32)
    // A restart right after quitting would otherwise find the port blocked
    // by the last run's closed connections (TIME_WAIT). Not on Windows,
    // where SO_REUSEADDR lets another program take the port over.
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(kGuiPort);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        addr.sin_port = 0; // taken - any free port
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            CloseSock(listener);
            return 0;
        }
    }
    socklen_t len = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const uint16_t port = ntohs(addr.sin_port);
    if (listen(listener, 16) != 0) {
        CloseSock(listener);
        return 0;
    }
    static const std::string preset_json = PresetJson();
    std::thread([listener, &app, port] {
        while (!app.quit()) {
            const SockT client = accept(listener, nullptr, nullptr);
            if (client == kBadSock) continue;
            std::thread(HandleConnection, client, std::ref(app), port, std::cref(preset_json)).detach();
        }
    }).detach();
    return port;
}

void OpenBrowser(const std::string& url) {
#if defined(_WIN32)
    std::system(("start \"\" \"" + url + "\"").c_str());
#elif defined(__APPLE__)
    std::system(("open '" + url + "'").c_str());
#else
    std::system(("xdg-open '" + url + "' >/dev/null 2>&1 &").c_str());
#endif
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const bool no_browser = argc == 2 && std::string(argv[1]) == "--no-browser";
    const bool gui = argc == 1 || no_browser;
    App app(gui);

    if (gui) {
        const uint16_t port = StartGuiServer(app);
        if (!port) {
            std::fprintf(stderr, "Couldn't start the GUI server on 127.0.0.1\n");
            return 1;
        }
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
        std::printf("GUI: %s  (Ctrl+C quits; console commands work here too, 'help')\n", url.c_str());
        std::fflush(stdout);
        if (!no_browser) OpenBrowser(url);
    } else {
        Options opt;
        if (!ParseArgs(argc, argv, opt)) {
            PrintUsage(argv[0]);
            return 2;
        }
        app.Connect(opt);
        std::printf("Type 'help' for commands.\n");
    }

    // Console reader: queues each line for the main loop, which owns all
    // network state. "wait" pauses the reader itself, for piped scripts.
    std::thread([&app] {
        std::string line;
        while (!app.quit() && std::getline(std::cin, line)) {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;
            if (cmd == "wait") {
                double seconds = 1.0;
                in >> seconds;
                std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            } else if (cmd == "quit" || cmd == "exit") {
                app.RequestQuit();
            } else {
                app.Submit(line);
            }
        }
        // stdin closed (end of a piped script): keep running until Ctrl+C.
    }).detach();

    double last = Now();
    while (!app.quit()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double now = Now();
        app.Tick(now, now - last);
        last = now;
    }
    app.Shutdown();
    return 0;
}
