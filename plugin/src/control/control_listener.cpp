#include "control/control_listener.h"

#include <sstream>

namespace flytogether {

bool ControlListener::Start(const Callbacks& callbacks) {
    callbacks_ = callbacks;
    if (!socket_.Open()) {
        return false;
    }
    // Loopback only: these commands start/stop sessions and reload CSL, so
    // nothing but the companion app on this same machine may send them.
    if (!socket_.Bind(kControlUdpPort, /*loopbackOnly=*/true)) {
        // Closed rather than left open-but-unbound: that socket is still in
        // blocking mode, so a later ReceiveFrom() would hang the sim.
        socket_.Close();
        return false;
    }
    socket_.SetNonBlocking(true);
    return true;
}

void ControlListener::Stop() {
    socket_.Close();
}

void ControlListener::Poll() {
    char buf[2048];
    while (true) {
        const int received = socket_.ReceiveFrom(buf, sizeof(buf) - 1);
        if (received < 0) {
            break; // no more datagrams pending
        }
        buf[received] = '\0';

        std::istringstream stream(std::string(buf, static_cast<size_t>(received)));
        std::string line;
        while (std::getline(stream, line)) {
            HandleLine(line);
        }
    }
}

void ControlListener::HandleLine(const std::string& line) {
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;

    if (cmd == "CREATE_SESSION") {
        std::string host_port, spectator_token;
        ls >> host_port >> spectator_token;
        if (callbacks_.on_create_session) {
            callbacks_.on_create_session(host_port, spectator_token == "SPECTATOR");
        }
    } else if (cmd == "JOIN_SESSION") {
        std::string host_port, code, spectator_token;
        ls >> host_port >> code >> spectator_token;
        if (callbacks_.on_join_session) {
            callbacks_.on_join_session(host_port, code, spectator_token == "SPECTATOR");
        }
    } else if (cmd == "START_SHARED_COCKPIT") {
        std::string role, host_port, code;
        ls >> role >> host_port >> code;
        if (callbacks_.on_start_shared_cockpit) {
            callbacks_.on_start_shared_cockpit(role == "MASTER", host_port, code);
        }
    } else if (cmd == "LAN_CONNECT_FORMATION") {
        std::string host_port, code;
        ls >> host_port >> code;
        if (callbacks_.on_lan_connect_formation) {
            callbacks_.on_lan_connect_formation(host_port, code);
        }
    } else if (cmd == "CLAIM_OWNERSHIP") {
        std::string category_name;
        ls >> category_name;
        DatarefCategory category;
        if (ParseDatarefCategoryName(category_name, category) && callbacks_.on_claim_ownership) {
            callbacks_.on_claim_ownership(category);
        }
    } else if (cmd == "DISCONNECT_FORMATION") {
        if (callbacks_.on_disconnect_formation) callbacks_.on_disconnect_formation();
    } else if (cmd == "DISCONNECT_SHARED_COCKPIT") {
        if (callbacks_.on_disconnect_shared_cockpit) callbacks_.on_disconnect_shared_cockpit();
    } else if (cmd == "RELOAD_CSL") {
        if (callbacks_.on_reload_csl) callbacks_.on_reload_csl();
    } else if (cmd == "SET_PREFS") {
        std::string callsign, labels, env_sync;
        ls >> callsign >> labels >> env_sync;
        if (callbacks_.on_set_prefs && !callsign.empty()) {
            callbacks_.on_set_prefs(callsign == "-" ? "" : callsign, labels != "0", env_sync != "0");
        }
    } else if (cmd == "GET_STATUS") {
        SendStatus();
    }
}

void ControlListener::SendStatus() {
    const std::string msg = "FORMATION " + formation_status_ + "\nFORMATION_CODE " + formation_code_ +
                             "\nSHARED_COCKPIT " + shared_cockpit_status_ + "\nSHARED_COCKPIT_CODE " +
                             shared_cockpit_code_ + "\nSHARED_COCKPIT_OWNERSHIP " + shared_cockpit_ownership_ +
                             "\nPEERS " + formation_peers_ + "\nLINK_QUALITY " + link_quality_ +
                             "\nSHARED_COCKPIT_AIRCRAFT_MISMATCH " + shared_cockpit_aircraft_mismatch_ +
                             "\nSIM_READY " + (sim_ready_ ? "1" : "0") + "\nPLUGIN_VERSION " + plugin_version_ +
                             "\nCSL_STATUS " + csl_status_ + "\nPREFS " + prefs_ + "\nOWN_ICAO " + own_icao_ +
                             "\n";
    socket_.SendTo("127.0.0.1", kCompanionUdpPort, msg.data(), msg.size());
}

void ControlListener::SetFormationStatus(const std::string& text) {
    formation_status_ = text;
    SendStatus();
}

void ControlListener::SetFormationCode(const std::string& code) {
    formation_code_ = code;
    SendStatus();
}

void ControlListener::SetSharedCockpitStatus(const std::string& text) {
    shared_cockpit_status_ = text;
    SendStatus();
}

void ControlListener::SetSharedCockpitCode(const std::string& code) {
    shared_cockpit_code_ = code;
    SendStatus();
}

namespace {
const char* OwnershipUiStateName(ControlListener::OwnershipUiState state) {
    switch (state) {
        case ControlListener::OwnershipUiState::kMe: return "me";
        case ControlListener::OwnershipUiState::kPeer: return "peer";
    }
    return "peer"; // unreachable for a valid enum value, but keeps this total
}
} // namespace

void ControlListener::SetSharedCockpitOwnership(
    const std::array<OwnershipUiState, kDatarefCategoryCount>& state) {
    std::ostringstream out;
    for (int i = 0; i < kDatarefCategoryCount; ++i) {
        if (i > 0) out << ' ';
        out << DatarefCategoryName(static_cast<DatarefCategory>(i)) << ':' << OwnershipUiStateName(state[i]);
    }
    shared_cockpit_ownership_ = out.str();
    SendStatus();
}

void ControlListener::SetFormationPeers(const std::string& encodedPeers) {
    formation_peers_ = encodedPeers;
    SendStatus();
}

void ControlListener::SetLinkQuality(const std::string& encoded) {
    link_quality_ = encoded;
    SendStatus();
}

void ControlListener::SetSharedCockpitAircraftMismatch(const std::string& encoded) {
    shared_cockpit_aircraft_mismatch_ = encoded;
    SendStatus();
}

void ControlListener::SetSimReady(bool ready) {
    sim_ready_ = ready;
    SendStatus();
}

void ControlListener::SetCslStatus(const std::string& text) {
    csl_status_ = text;
    SendStatus();
}

void ControlListener::SetPrefs(const std::string& encoded) {
    if (encoded == prefs_) return;
    prefs_ = encoded;
    SendStatus();
}

void ControlListener::SetOwnIcao(const std::string& icao) {
    if (icao == own_icao_) return;
    own_icao_ = icao;
    SendStatus();
}

void ControlListener::SetPluginVersion(const std::string& version) {
    plugin_version_ = version;
    SendStatus();
}

} // namespace flytogether
