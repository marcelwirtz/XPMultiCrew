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
    DatarefCategory requested_category = DatarefCategory::kSystems;
    bool ownership_requested = false;
    DatarefCategory responded_category = DatarefCategory::kSystems;
    bool responded_grant = false;
    bool ownership_responded = false;
    bool create_as_spectator = false;
    bool join_as_spectator = false;
    std::string lan_host_port, lan_code;
    bool lan_connect_called = false;

    ControlListener::Callbacks callbacks;
    callbacks.on_create_session = [&](const std::string& hp, bool as_spectator) {
        create_called = true;
        created_host_port = hp;
        create_as_spectator = as_spectator;
    };
    callbacks.on_join_session = [&](const std::string& hp, const std::string& code, bool as_spectator) {
        join_called = true;
        joined_host_port = hp;
        joined_code = code;
        join_as_spectator = as_spectator;
    };
    callbacks.on_start_shared_cockpit = [&](bool is_master, const std::string& host_port,
                                             const std::string& code) {
        sc_called = true;
        sc_is_master = is_master;
        sc_host_port = host_port;
        sc_code = code;
    };
    callbacks.on_lan_connect_formation = [&](const std::string& hp, const std::string& code) {
        lan_connect_called = true;
        lan_host_port = hp;
        lan_code = code;
    };
    callbacks.on_request_ownership = [&](DatarefCategory category) {
        ownership_requested = true;
        requested_category = category;
    };
    callbacks.on_respond_ownership = [&](DatarefCategory category, bool grant) {
        ownership_responded = true;
        responded_category = category;
        responded_grant = grant;
    };

    assert(listener.Start(callbacks));

    // IsSimReady() lets plugin_main.cpp's PollControlListenerCallback
    // self-heal SIM_READY on its own first tick (catching the case where
    // XPLM_MSG_PLANE_LOADED fires before the plugin is enabled to receive
    // it - see control_listener.h's SIM_READY comment) - starts false,
    // reflects whatever SetSimReady last set.
    assert(!listener.IsSimReady());
    listener.SetSimReady(true);
    assert(listener.IsSimReady());
    listener.SetSimReady(false);
    assert(!listener.IsSimReady());
    std::printf("IsSimReady() reflects SetSimReady(): OK\n");

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
    assert(!create_as_spectator);
    std::printf("CREATE_SESSION parsed correctly: %s\n", created_host_port.c_str());

    const std::string cmd2 = "JOIN_SESSION 1.2.3.4:9999 ABC123\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd2.data(), cmd2.size());
    PumpFor(listener, 200ms);
    assert(join_called);
    assert(joined_host_port == "1.2.3.4:9999");
    assert(joined_code == "ABC123");
    assert(!join_as_spectator);
    std::printf("JOIN_SESSION parsed correctly: %s / %s\n", joined_host_port.c_str(), joined_code.c_str());

    create_called = false;
    const std::string cmd1_spectator = "CREATE_SESSION rendezvous.example.com:45000 SPECTATOR\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd1_spectator.data(), cmd1_spectator.size());
    PumpFor(listener, 200ms);
    assert(create_called);
    assert(create_as_spectator);
    std::printf("CREATE_SESSION SPECTATOR parsed correctly\n");

    join_called = false;
    const std::string cmd2_spectator = "JOIN_SESSION 1.2.3.4:9999 ABC123 SPECTATOR\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd2_spectator.data(), cmd2_spectator.size());
    PumpFor(listener, 200ms);
    assert(join_called);
    assert(join_as_spectator);
    std::printf("JOIN_SESSION SPECTATOR parsed correctly\n");

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

    const std::string cmd_lan = "LAN_CONNECT_FORMATION 192.168.1.50:49002 LANCODE\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd_lan.data(), cmd_lan.size());
    PumpFor(listener, 200ms);
    assert(lan_connect_called);
    assert(lan_host_port == "192.168.1.50:49002");
    assert(lan_code == "LANCODE");
    std::printf("LAN_CONNECT_FORMATION parsed correctly: %s / %s\n", lan_host_port.c_str(), lan_code.c_str());

    const std::string cmd5 = "REQUEST_OWNERSHIP engine\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd5.data(), cmd5.size());
    PumpFor(listener, 200ms);
    assert(ownership_requested);
    assert(requested_category == DatarefCategory::kEngine);
    std::printf("REQUEST_OWNERSHIP parsed correctly: category=%d\n", static_cast<int>(requested_category));

    // An unrecognized category name is silently ignored, same as any
    // other unrecognized token elsewhere in this parser - the callback
    // must not fire.
    ownership_requested = false;
    const std::string cmd6 = "REQUEST_OWNERSHIP bogus\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd6.data(), cmd6.size());
    PumpFor(listener, 200ms);
    assert(!ownership_requested);
    std::printf("REQUEST_OWNERSHIP with an unknown category is ignored: OK\n");

    const std::string cmd7 = "RESPOND_OWNERSHIP avionics grant\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd7.data(), cmd7.size());
    PumpFor(listener, 200ms);
    assert(ownership_responded);
    assert(responded_category == DatarefCategory::kAvionics);
    assert(responded_grant == true);
    std::printf("RESPOND_OWNERSHIP grant parsed correctly\n");

    ownership_responded = false;
    const std::string cmd8 = "RESPOND_OWNERSHIP avionics deny\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd8.data(), cmd8.size());
    PumpFor(listener, 200ms);
    assert(ownership_responded);
    assert(responded_grant == false);
    std::printf("RESPOND_OWNERSHIP deny parsed correctly\n");

    // Neither an unknown category nor a garbage decision word should fire
    // the callback - same "don't hard-fail on the unknown" parsing spirit
    // as REQUEST_OWNERSHIP's own malformed-input check above.
    ownership_responded = false;
    const std::string cmd9 = "RESPOND_OWNERSHIP bogus grant\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd9.data(), cmd9.size());
    PumpFor(listener, 200ms);
    assert(!ownership_responded);
    const std::string cmd10 = "RESPOND_OWNERSHIP avionics maybe\n";
    companion.SendTo("127.0.0.1", kControlUdpPort, cmd10.data(), cmd10.size());
    PumpFor(listener, 200ms);
    assert(!ownership_responded);
    std::printf("RESPOND_OWNERSHIP with unknown category/decision is ignored: OK\n");

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

    // SetSharedCockpitOwnership - per-category state the companion app
    // renders as the ownership buttons'/Grant-Deny prompt's state. All
    // four OwnershipUiState values in one push, to cover each one.
    listener.SetSharedCockpitOwnership({/*systems=*/ControlListener::OwnershipUiState::kMe,
                                         /*engine=*/ControlListener::OwnershipUiState::kPeer,
                                         /*avionics=*/ControlListener::OwnershipUiState::kPending});
    bool got_ownership = false;
    for (int i = 0; i < 50 && !got_ownership; ++i) {
        const int received = companion.ReceiveFrom(buf, sizeof(buf) - 1);
        if (received > 0) {
            buf[received] = '\0';
            std::string msg(buf, static_cast<size_t>(received));
            if (msg.find("SHARED_COCKPIT_OWNERSHIP systems:me engine:peer avionics:pending") !=
                std::string::npos) {
                got_ownership = true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    assert(got_ownership);
    std::printf("SHARED_COCKPIT_OWNERSHIP correctly pushed (me/peer/pending)\n");

    // "requested" is the fourth state, exercised separately so every
    // OwnershipUiState value gets its own explicit coverage.
    listener.SetSharedCockpitOwnership({ControlListener::OwnershipUiState::kRequested,
                                         ControlListener::OwnershipUiState::kPeer,
                                         ControlListener::OwnershipUiState::kPeer});
    bool got_requested = false;
    for (int i = 0; i < 50 && !got_requested; ++i) {
        const int received = companion.ReceiveFrom(buf, sizeof(buf) - 1);
        if (received > 0) {
            buf[received] = '\0';
            std::string msg(buf, static_cast<size_t>(received));
            if (msg.find("SHARED_COCKPIT_OWNERSHIP systems:requested") != std::string::npos) {
                got_requested = true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    assert(got_requested);
    std::printf("SHARED_COCKPIT_OWNERSHIP correctly pushed (requested)\n");

    // SetLinkQuality/SetSharedCockpitAircraftMismatch - both take an
    // already-encoded string from the caller (plugin_main.cpp), same
    // pattern as SetFormationPeers above.
    listener.SetLinkQuality("formation_server_rtt_ms:42 sc_server_rtt_ms:?");
    listener.SetSharedCockpitAircraftMismatch("C172:B738");
    bool got_link_quality_fields = false;
    for (int i = 0; i < 50 && !got_link_quality_fields; ++i) {
        const int received = companion.ReceiveFrom(buf, sizeof(buf) - 1);
        if (received > 0) {
            buf[received] = '\0';
            std::string msg(buf, static_cast<size_t>(received));
            if (msg.find("LINK_QUALITY formation_server_rtt_ms:42 sc_server_rtt_ms:?") != std::string::npos &&
                msg.find("SHARED_COCKPIT_AIRCRAFT_MISMATCH C172:B738") != std::string::npos) {
                got_link_quality_fields = true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    assert(got_link_quality_fields);
    std::printf("LINK_QUALITY/SHARED_COCKPIT_AIRCRAFT_MISMATCH correctly pushed\n");

    std::printf("\nALL CONTROL LISTENER CHECKS PASSED\n");
    return 0;
}
