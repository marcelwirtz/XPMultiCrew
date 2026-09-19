#include "control/control_listener.h"

#include <sstream>

namespace flytogether {

bool ControlListener::Start(const Callbacks& callbacks) {
    callbacks_ = callbacks;
    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(kControlUdpPort)) {
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
        std::string host_port;
        ls >> host_port;
        if (callbacks_.on_create_session) callbacks_.on_create_session(host_port);
    } else if (cmd == "JOIN_SESSION") {
        std::string host_port, code;
        ls >> host_port >> code;
        if (callbacks_.on_join_session) callbacks_.on_join_session(host_port, code);
    } else if (cmd == "START_SHARED_COCKPIT") {
        std::string role, host_port, code;
        ls >> role >> host_port >> code;
        if (callbacks_.on_start_shared_cockpit) {
            callbacks_.on_start_shared_cockpit(role == "MASTER", host_port, code);
        }
    } else if (cmd == "DISCONNECT_FORMATION") {
        if (callbacks_.on_disconnect_formation) callbacks_.on_disconnect_formation();
    } else if (cmd == "DISCONNECT_SHARED_COCKPIT") {
        if (callbacks_.on_disconnect_shared_cockpit) callbacks_.on_disconnect_shared_cockpit();
    } else if (cmd == "GET_STATUS") {
        SendStatus();
    }
}

void ControlListener::SendStatus() {
    const std::string msg = "FORMATION " + formation_status_ + "\nFORMATION_CODE " + formation_code_ +
                             "\nSHARED_COCKPIT " + shared_cockpit_status_ + "\nSHARED_COCKPIT_CODE " +
                             shared_cockpit_code_ + "\nPEERS " + formation_peers_ + "\nSIM_READY " +
                             (sim_ready_ ? "1" : "0") + "\nPLUGIN_VERSION " + plugin_version_ + "\n";
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

void ControlListener::SetFormationPeers(const std::string& encodedPeers) {
    formation_peers_ = encodedPeers;
    SendStatus();
}

void ControlListener::SetSimReady(bool ready) {
    sim_ready_ = ready;
    SendStatus();
}

void ControlListener::SetPluginVersion(const std::string& version) {
    plugin_version_ = version;
    SendStatus();
}

} // namespace flytogether
