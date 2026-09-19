// Verifies RendezvousClient's wire behavior end-to-end over a real
// loopback UDP socket standing in for the server - same approach as
// control_listener_test.cpp. Focused on Stop()'s graceful-leave behavior
// (see server/session.go's leave_session handling): confirms it's sent
// once the client is actually in a session, and not before.

#include "formation/rendezvous_client.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace flytogether;
using namespace std::chrono_literals;

namespace {

constexpr uint16_t kFakeServerPort = 49040;

// Reads one pending datagram (and the address it came from, so a fake
// server can reply to a client's ephemeral local port) within a timeout,
// or "" if none arrived.
std::string RecvOne(UdpSocket& sock, std::chrono::milliseconds timeout, std::string* fromHost = nullptr,
                     uint16_t* fromPort = nullptr) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    char buf[4096];
    while (std::chrono::steady_clock::now() < until) {
        const int received = sock.ReceiveFrom(buf, sizeof(buf) - 1, fromHost, fromPort);
        if (received > 0) {
            buf[received] = '\0';
            return std::string(buf, static_cast<size_t>(received));
        }
        std::this_thread::sleep_for(5ms);
    }
    return "";
}

void PumpFor(RendezvousClient& client, std::chrono::milliseconds dur) {
    const auto until = std::chrono::steady_clock::now() + dur;
    while (std::chrono::steady_clock::now() < until) {
        client.PollIncoming();
        std::this_thread::sleep_for(5ms);
    }
}

} // namespace

int main() {
    UdpSocket fake_server;
    assert(fake_server.Open());
    assert(fake_server.Bind(kFakeServerPort));
    fake_server.SetNonBlocking(true);

    // --- Stop() before ever being in a session sends nothing extra ---
    {
        RendezvousClient client;
        assert(client.Start("127.0.0.1", kFakeServerPort));
        client.CreateSession();

        std::string ignored_host;
        uint16_t ignored_port = 0;
        const std::string create_msg = RecvOne(fake_server, 500ms, &ignored_host, &ignored_port);
        assert(create_msg.find("\"type\":\"create_session\"") != std::string::npos);

        client.Stop(); // never received session_created, so not in_session_
        const std::string after_stop = RecvOne(fake_server, 300ms);
        if (!after_stop.empty()) {
            std::printf("Unexpected message after Stop() while never in a session: %s\n",
                        after_stop.c_str());
            return 1;
        }
        std::printf("Stop() before joining a session correctly sends nothing: OK\n");
    }

    // --- Stop() while actually in a session sends leave_session ---
    {
        RendezvousClient client;
        assert(client.Start("127.0.0.1", kFakeServerPort));
        client.CreateSession();

        std::string client_host;
        uint16_t client_port = 0;
        const std::string create_msg = RecvOne(fake_server, 500ms, &client_host, &client_port);
        assert(create_msg.find("\"type\":\"create_session\"") != std::string::npos);
        assert(client_port != 0);

        // Act as the real server would: confirm the session, which is
        // what flips the client into in_session_.
        const std::string reply = R"({"type":"session_created","code":"ABC123","your_id":1})";
        assert(fake_server.SendTo(client_host, client_port, reply.data(), reply.size()));
        PumpFor(client, 500ms);
        assert(client.InSession());

        client.Stop();
        const std::string leave_msg = RecvOne(fake_server, 500ms);
        if (leave_msg.find("\"type\":\"leave_session\"") == std::string::npos) {
            std::printf("Expected a leave_session message, got: %s\n", leave_msg.c_str());
            return 1;
        }
        std::printf("Stop() while in a session correctly sends leave_session: %s\n", leave_msg.c_str());
    }

    std::printf("\nALL RENDEZVOUS CLIENT CHECKS PASSED\n");
    return 0;
}
