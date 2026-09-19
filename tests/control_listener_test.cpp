// Verifies ControlListener's command parsing end-to-end over a real
// loopback UDP socket (same approach as the rendezvous/formation tests):
// a plain UdpSocket stands in for the companion app, sends real command
// lines, and we check the right callback fires with the right arguments,
// and that SetFormationStatus()/SetSharedCockpitStatus() push a status
// message a companion-app-like listener can decode.

#include "control/control_listener.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace flytogether;
using namespace std::chrono_literals;

namespace {

void PumpFor(ControlListener& listener, std::chrono::milliseconds dur) {
    const auto until = std::chrono::steady_clock::now() + dur;
    while (std::chrono::steady_clock::now() < until) {
        listener.Poll();
        std::this_thread::sleep_for(5ms);
    }
}

} // namespace

int main() {
    ControlListener listener;

    std::string created_host_port;
    bool create_called = false;
    std::string joined_host_port, joined_code;
    bool join_called = false;
    bool sc_is_master = false;
    std::string sc_host_port, sc_code;
    bool sc_called = false;

    ControlListener::Callbacks callbacks;
    callbacks.on_create_session = [&](const std::string& hp) {
        create_called = true;
        created_host_port = hp;
    };
    callbacks.on_join_session = [&](const std::string& hp, const std::string& code) {
        join_called = true;
        joined_host_port = hp;
        joined_code = code;
    };
    callbacks.on_start_shared_cockpit = [&](bool is_master, const std::string& host_port,
                                             const std::string& code) {
        sc_called = true;
        sc_is_master = is_master;
        sc_host_port = host_port;
        sc_code = code;
    };

    assert(listener.Start(callbacks));

    // A raw socket standing in for the companion app.
    UdpSocket companion;
    assert(companion.Open());
    assert(companion.Bind(kCompanionUdpPort));
    companion.SetNonBlocking(true);

    const std::string cmd1 = "CREATE_SESSION rendezvous.example.com:45000\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd1.data(), cmd1.size());
    PumpFor(listener, 200ms);
    assert(create_called);
    assert(created_host_port == "rendezvous.example.com:45000");
    std::printf("CREATE_SESSION parsed correctly: %s\n", created_host_port.c_str());

    const std::string cmd2 = "JOIN_SESSION 1.2.3.4:9999 ABC123\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd2.data(), cmd2.size());
    PumpFor(listener, 200ms);
    assert(join_called);
    assert(joined_host_port == "1.2.3.4:9999");
    assert(joined_code == "ABC123");
    std::printf("JOIN_SESSION parsed correctly: %s / %s\n", joined_host_port.c_str(), joined_code.c_str());

    const std::string cmd3 = "START_SHARED_COCKPIT CLIENT rendezvous.example.com:45000 DEF456\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd3.data(), cmd3.size());
    PumpFor(listener, 200ms);
    assert(sc_called);
    assert(sc_is_master == false);
    assert(sc_host_port == "rendezvous.example.com:45000");
    assert(sc_code == "DEF456");
    std::printf("START_SHARED_COCKPIT parsed correctly: %s / %s, master=%d\n",
                sc_host_port.c_str(), sc_code.c_str(), sc_is_master);

    // MASTER omits the code (it doesn't have one yet - the rendezvous
    // server generates it on create_session).
    sc_called = false;
    const std::string cmd4 = "START_SHARED_COCKPIT MASTER rendezvous.example.com:45000\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd4.data(), cmd4.size());
    PumpFor(listener, 200ms);
    assert(sc_called);
    assert(sc_is_master == true);
    assert(sc_host_port == "rendezvous.example.com:45000");
    assert(sc_code.empty());
    std::printf("START_SHARED_COCKPIT (MASTER, no code) parsed correctly: %s, master=%d\n",
                sc_host_port.c_str(), sc_is_master);

    // Status push: SetFormationStatus should immediately send a decodable
    // message to the companion's port.
    listener.SetFormationStatus("connected, code 'XYZ', peer 1");
    char buf[1024];
    bool got_status = false;
    for (int i = 0; i < 50 && !got_status; ++i) {
        const int received = companion.ReceiveFrom(buf, sizeof(buf) - 1);
        if (received > 0) {
            buf[received] = '\0';
            std::string msg(buf, static_cast<size_t>(received));
            std::printf("Status received:\n%s\n", msg.c_str());
            assert(msg.find("FORMATION connected, code 'XYZ', peer 1") != std::string::npos);
            assert(msg.find("SHARED_COCKPIT not started") != std::string::npos);
            assert(msg.find("SIM_READY 0") != std::string::npos); // default until SetSimReady(true)
            got_status = true;
        }
        std::this_thread::sleep_for(10ms);
    }
    assert(got_status);

    // SetSimReady should push an updated SIM_READY value the same way the
    // other Set*Status methods do.
    listener.SetSimReady(true);
    bool got_sim_ready = false;
    for (int i = 0; i < 50 && !got_sim_ready; ++i) {
        const int received = companion.ReceiveFrom(buf, sizeof(buf) - 1);
        if (received > 0) {
            buf[received] = '\0';
            std::string msg(buf, static_cast<size_t>(received));
            if (msg.find("SIM_READY 1") != std::string::npos) {
                got_sim_ready = true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    assert(got_sim_ready);
    std::printf("SetSimReady(true) correctly pushed SIM_READY 1\n");

    // SetFormationCode/SetSharedCockpitCode/SetFormationPeers - the fields
    // that let the companion app auto-fill the session code field and
    // show a connected-peers list, instead of the user reading the code
    // out of the status text by hand.
    listener.SetFormationCode("ABC123");
    listener.SetSharedCockpitCode("DEF456");
    listener.SetFormationPeers("111:C172;222:B738");
    bool got_new_fields = false;
    for (int i = 0; i < 50 && !got_new_fields; ++i) {
        const int received = companion.ReceiveFrom(buf, sizeof(buf) - 1);
        if (received > 0) {
            buf[received] = '\0';
            std::string msg(buf, static_cast<size_t>(received));
            if (msg.find("FORMATION_CODE ABC123") != std::string::npos &&
                msg.find("SHARED_COCKPIT_CODE DEF456") != std::string::npos &&
                msg.find("PEERS 111:C172;222:B738") != std::string::npos) {
                got_new_fields = true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    assert(got_new_fields);
    std::printf("FORMATION_CODE/SHARED_COCKPIT_CODE/PEERS correctly pushed\n");

    std::printf("\nALL CONTROL LISTENER CHECKS PASSED\n");
    return 0;
}
