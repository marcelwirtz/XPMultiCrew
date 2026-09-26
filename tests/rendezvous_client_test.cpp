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
constexpr uint16_t kFakeServerPort2 = 49041; // direct-P2P block, own socket so its traffic can't leak into others

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

    // --- on_session_ready decodes the base64 salt into raw bytes ---
    {
        RendezvousClient client;
        std::vector<uint8_t> received_salt;
        bool ready = false;
        client.on_session_ready = [&](const std::string&, int, const std::vector<uint8_t>& salt) {
            ready = true;
            received_salt = salt;
        };
        assert(client.Start("127.0.0.1", kFakeServerPort));
        client.CreateSession();

        std::string client_host;
        uint16_t client_port = 0;
        const std::string create_msg = RecvOne(fake_server, 500ms, &client_host, &client_port);
        // client_version is sent on every message, including create_session
        // - see server/protocol.go's ProtocolVersion comment.
        assert(create_msg.find("\"client_version\":" + std::to_string(kRendezvousProtocolVersion)) !=
               std::string::npos);

        // "AQIDBA==" is the base64 of bytes {1, 2, 3, 4} - a real server
        // would send SessionCrypto::kSaltSize (16) bytes, but this only
        // needs to prove the base64 decode itself happens correctly.
        const std::string reply =
            R"({"type":"session_created","code":"SALT01","your_id":1,"salt":"AQIDBA=="})";
        assert(fake_server.SendTo(client_host, client_port, reply.data(), reply.size()));
        PumpFor(client, 500ms);
        assert(ready);
        const std::vector<uint8_t> expected = {1, 2, 3, 4};
        assert(received_salt == expected);
        std::printf("on_session_ready decodes the salt correctly: OK\n");

        // Deliberately no client.Stop() here: it would send leave_session
        // to the shared fake_server socket, which the NEXT test block's
        // RecvOne() could then read instead of that block's own
        // create_session - see the other blocks' identical pattern of
        // just letting `client` go out of scope instead.
    }

    // --- Silence past the timeout fires on_disconnected exactly once ---
    {
        RendezvousClient client;
        bool disconnected = false;
        client.on_disconnected = [&] { disconnected = true; };
        assert(client.Start("127.0.0.1", kFakeServerPort));
        client.CreateSession();

        std::string client_host;
        uint16_t client_port = 0;
        RecvOne(fake_server, 500ms, &client_host, &client_port);
        const std::string reply = R"({"type":"session_created","code":"XYZ789","your_id":1})";
        assert(fake_server.SendTo(client_host, client_port, reply.data(), reply.size()));
        client.PollIncoming(50ms); // short override so this test doesn't wait 40 real seconds
        assert(client.InSession());
        assert(!disconnected);

        // The fake server goes silent from here - no keepalive_ack, no
        // peer/relay traffic - simulating a dead server/network path.
        std::this_thread::sleep_for(80ms);
        client.PollIncoming(50ms);
        assert(disconnected);
        assert(!client.InSession());
        std::printf("Silence past the timeout correctly fires on_disconnected: OK\n");

        // Firing again on a later poll (still silent) would make
        // plugin_main.cpp's reconnect scheduling re-trigger repeatedly for
        // one real outage - must only fire once per disconnect.
        disconnected = false;
        std::this_thread::sleep_for(80ms);
        client.PollIncoming(50ms);
        assert(!disconnected);
        std::printf("on_disconnected does not re-fire while already disconnected: OK\n");
    }

    // --- keepalive_ack alone (no peer, no relay) keeps the connection
    // alive - the scenario on_disconnected exists to not misfire on ---
    {
        RendezvousClient client;
        bool disconnected = false;
        client.on_disconnected = [&] { disconnected = true; };
        assert(client.Start("127.0.0.1", kFakeServerPort));
        client.CreateSession();

        std::string client_host;
        uint16_t client_port = 0;
        RecvOne(fake_server, 500ms, &client_host, &client_port);
        const std::string reply = R"({"type":"session_created","code":"LONE01","your_id":1})";
        assert(fake_server.SendTo(client_host, client_port, reply.data(), reply.size()));
        client.PollIncoming(50ms);
        assert(client.InSession());

        // Nothing but a bare keepalive_ack arrives (as if alone in the
        // session, waiting for a co-pilot) - must NOT be mistaken for a
        // disconnect just because there's no peer/relay traffic.
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(30ms);
            const std::string ack = R"({"type":"keepalive_ack"})";
            assert(fake_server.SendTo(client_host, client_port, ack.data(), ack.size()));
            client.PollIncoming(50ms);
        }
        assert(client.InSession());
        assert(!disconnected);
        std::printf("keepalive_ack alone correctly keeps a lone-in-session client connected: OK\n");
    }

    // --- Direct P2P over the rendezvous socket: SendRelay() also reaches a
    // known peer directly, a silent peer still punches, strangers are
    // ignored ---
    {
        UdpSocket server2;
        assert(server2.Open());
        assert(server2.Bind(kFakeServerPort2));
        server2.SetNonBlocking(true);

        RendezvousClient a, b;
        std::vector<std::vector<uint8_t>> b_received;
        int b_from = -1;
        b.on_relay_received = [&](int from, const std::vector<uint8_t>& bytes) {
            b_from = from;
            b_received.push_back(bytes);
        };
        int a_direct_up_from = -1;
        a.on_direct_path_up = [&](int peer_id) { a_direct_up_from = peer_id; };
        bool a_got_payload = false;
        a.on_relay_received = [&](int, const std::vector<uint8_t>&) { a_got_payload = true; };

        assert(a.Start("127.0.0.1", kFakeServerPort2));
        assert(b.Start("127.0.0.1", kFakeServerPort2));
        a.CreateSession();
        std::string a_host, b_host;
        uint16_t a_port = 0, b_port = 0;
        RecvOne(server2, 500ms, &a_host, &a_port);
        b.JoinSession("P2P001");
        RecvOne(server2, 500ms, &b_host, &b_port);
        assert(a_port != 0 && b_port != 0);

        const std::string a_ready = R"({"type":"session_created","code":"P2P001","your_id":1})";
        const std::string b_ready = R"({"type":"session_created","code":"P2P001","your_id":2})";
        const std::string a_learns_b =
            R"({"type":"peer_joined","peer_id":2,"peer_addr":"127.0.0.1:)" + std::to_string(b_port) + R"("})";
        const std::string b_learns_a =
            R"({"type":"peer_joined","peer_id":1,"peer_addr":"127.0.0.1:)" + std::to_string(a_port) + R"("})";
        server2.SendTo(a_host, a_port, a_ready.data(), a_ready.size());
        server2.SendTo(a_host, a_port, a_learns_b.data(), a_learns_b.size());
        server2.SendTo(b_host, b_port, b_ready.data(), b_ready.size());
        server2.SendTo(b_host, b_port, b_learns_a.data(), b_learns_a.size());
        a.PollIncoming(10s);
        b.PollIncoming(10s);
        assert(a.InSession() && b.InSession());

        const uint8_t payload[] = {'{', 1, 2, 3}; // leading '{' on purpose: must not be mistaken for JSON
        a.SendRelay(payload, sizeof(payload));
        const auto until = std::chrono::steady_clock::now() + 500ms;
        while (b_received.empty() && std::chrono::steady_clock::now() < until) {
            b.PollIncoming(10s);
            std::this_thread::sleep_for(5ms);
        }
        assert(b_received.size() == 1);
        assert(b_received[0] == std::vector<uint8_t>(payload, payload + sizeof(payload)));
        assert(b_from == 1);
        assert(b.DirectPeerCount() == 1);
        const std::string relay_msg = RecvOne(server2, 500ms);
        assert(relay_msg.find("\"type\":\"relay\"") != std::string::npos);
        std::printf("SendRelay also delivers directly to a known peer: OK\n");

        // b never sends real traffic - its punch alone must bring up a's
        // direct path, without surfacing as a payload.
        const auto punch_until = std::chrono::steady_clock::now() + 2500ms;
        while (a_direct_up_from == -1 && std::chrono::steady_clock::now() < punch_until) {
            b.PollIncoming(10s);
            a.PollIncoming(10s);
            std::this_thread::sleep_for(20ms);
        }
        assert(a_direct_up_from == 2);
        assert(!a_got_payload);
        std::printf("A silent peer's hole punch brings up the direct path: OK\n");

        // A tagged datagram from an address the server never announced is
        // dropped.
        UdpSocket stranger;
        assert(stranger.Open());
        const char forged[] = {'X', 'M', 'C', 'D', 9, 9};
        stranger.SendTo("127.0.0.1", b_port, forged, sizeof(forged));
        b_received.clear();
        PumpFor(b, 200ms);
        assert(b_received.empty());
        std::printf("Direct datagrams from unknown addresses are ignored: OK\n");
    }

    std::printf("\nALL RENDEZVOUS CLIENT CHECKS PASSED\n");
    return 0;
}
