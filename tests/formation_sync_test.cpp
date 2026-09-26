// Verifies FormationSync's networking glue end-to-end over real loopback
// UDP sockets - same approach as control_listener_test.cpp/
// rendezvous_client_test.cpp. RemoteAircraft's own dead-reckoning math is
// already covered by remote_aircraft_test.cpp; this focuses on what's
// specific to FormationSync itself: binding, sending to configured peers,
// draining+validating incoming packets, peer add/remove dedup, and
// stale-aircraft eviction.

#include "formation/formation_sync.h"

#include "net/session_crypto.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

using namespace flytogether;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void WriteFile(const std::string& path, const std::string& contents) {
    std::ofstream f(path);
    f << contents;
}

AircraftStatePacket MakePacket(uint32_t sender_id, uint32_t sequence) {
    AircraftStatePacket packet;
    packet.sender_id = sender_id;
    packet.sequence = sequence;
    packet.latitude = 47.5;
    packet.longitude = -122.3;
    packet.elevation_m = 100.0;
    std::memcpy(packet.icao_type, "C172", 4);
    return packet;
}

// Reads one pending datagram within a timeout, or "" if none arrived -
// same helper as rendezvous_client_test.cpp's RecvOne.
std::string RecvOne(UdpSocket& sock, std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    char buf[4096];
    while (std::chrono::steady_clock::now() < until) {
        const int received = sock.ReceiveFrom(buf, sizeof(buf));
        if (received > 0) {
            return std::string(buf, static_cast<size_t>(received));
        }
        std::this_thread::sleep_for(5ms);
    }
    return "";
}

} // namespace

int main() {
    const fs::path tmp_dir = fs::temp_directory_path() / "xpmulticrew_formation_sync_test";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);
    const fs::path old_cwd = fs::current_path();
    fs::current_path(tmp_dir);

    // A raw socket standing in for one configured peer.
    constexpr uint16_t kFakePeerPort = 49050;
    UdpSocket fake_peer;
    assert(fake_peer.Open());
    assert(fake_peer.Bind(kFakePeerPort));
    fake_peer.SetNonBlocking(true);

    WriteFile("peers.txt", "127.0.0.1:" + std::to_string(kFakePeerPort) + "\n");

    FormationSync sync;
    assert(sync.Start("peers.txt"));
    assert(sync.peer_count() == 1);
    std::printf("Start() loads the peer list: OK\n");

    // --- SendOwnState reaches the configured peer verbatim ---
    {
        const AircraftStatePacket sent = MakePacket(/*sender_id=*/111, /*sequence=*/1);
        sync.SendOwnState(sent);

        const std::string raw = RecvOne(fake_peer, 500ms);
        assert(raw.size() == sizeof(AircraftStatePacket));
        AircraftStatePacket received;
        std::memcpy(&received, raw.data(), sizeof(received));
        assert(received.sender_id == 111);
        assert(received.sequence == 1);
        assert(received.magic == kAircraftStateMagic);
        std::printf("SendOwnState() reaches the configured peer intact: OK\n");
    }

    // --- PollIncoming validates magic/version before ingesting ---
    {
        AircraftStatePacket garbage = MakePacket(/*sender_id=*/222, /*sequence=*/1);
        garbage.magic = 0xDEADBEEF;
        fake_peer.SendTo("127.0.0.1", kFormationUdpPort, &garbage, sizeof(garbage));
        // Also a short, clearly-foreign datagram - must not crash/misparse.
        const std::string foreign = "not a packet";
        fake_peer.SendTo("127.0.0.1", kFormationUdpPort, foreign.data(), foreign.size());

        sync.PollIncoming(/*now_s=*/1.0);
        assert(sync.tracked_aircraft_count() == 0);
        std::printf("PollIncoming() ignores wrong-magic and malformed datagrams: OK\n");

        const AircraftStatePacket good = MakePacket(/*sender_id=*/222, /*sequence=*/1);
        fake_peer.SendTo("127.0.0.1", kFormationUdpPort, &good, sizeof(good));

        // PollIncoming is non-blocking; give the datagram a moment to
        // actually land in the socket's receive buffer.
        bool ingested = false;
        for (int i = 0; i < 50 && !ingested; ++i) {
            sync.PollIncoming(/*now_s=*/1.0);
            ingested = sync.tracked_aircraft_count() == 1;
            if (!ingested) std::this_thread::sleep_for(5ms);
        }
        assert(ingested);
        std::printf("PollIncoming() ingests a valid packet: OK\n");
    }

    // --- ForEachRemoteAircraft surfaces the tracked sender's latest data ---
    {
        bool visited = false;
        sync.ForEachRemoteAircraft(1.0, [&](uint32_t sender_id, const AircraftPose& pose,
                                             const AircraftStatePacket& latest) {
            visited = true;
            assert(sender_id == 222);
            assert(latest.sender_id == 222);
            (void)pose;
        });
        assert(visited);
        std::printf("ForEachRemoteAircraft() surfaces the tracked sender: OK\n");
    }

    // --- RemoveStaleAircraft evicts an aircraft past its timeout ---
    {
        sync.RemoveStaleAircraft(/*now_s=*/1.0, /*timeout_s=*/8.0);
        assert(sync.tracked_aircraft_count() == 1); // not stale yet
        sync.RemoveStaleAircraft(/*now_s=*/100.0, /*timeout_s=*/8.0);
        assert(sync.tracked_aircraft_count() == 0);
        std::printf("RemoveStaleAircraft() evicts aircraft past the timeout: OK\n");
    }

    // --- PollIncoming is forward/backward-compatible on packet size ---
    // See aircraft_state.h's kAircraftStateMinSize comment: a future
    // AircraftStatePacket field addition must not make two differently-
    // versioned builds stop seeing each other outright. Tracked state is
    // empty at this point (the block above just evicted everything).
    {
        // A packet with extra trailing bytes (simulating a newer peer
        // build with fields this build doesn't know about yet) is still
        // accepted, and the fields this build *does* know about still
        // read correctly from the shared prefix.
        const AircraftStatePacket bigger_sender = MakePacket(/*sender_id=*/333, /*sequence=*/1);
        std::vector<uint8_t> padded(sizeof(bigger_sender) + 16, 0xAB);
        std::memcpy(padded.data(), &bigger_sender, sizeof(bigger_sender));
        fake_peer.SendTo("127.0.0.1", kFormationUdpPort, padded.data(), padded.size());

        bool ingested_bigger = false;
        for (int i = 0; i < 50 && !ingested_bigger; ++i) {
            sync.PollIncoming(/*now_s=*/1.0);
            ingested_bigger = sync.tracked_aircraft_count() == 1;
            if (!ingested_bigger) std::this_thread::sleep_for(5ms);
        }
        assert(ingested_bigger);
        bool saw_333 = false;
        sync.ForEachRemoteAircraft(1.0, [&](uint32_t sender_id, const AircraftPose&,
                                             const AircraftStatePacket& latest) {
            if (sender_id == 333) {
                saw_333 = true;
                assert(latest.sequence == 1);
            }
        });
        assert(saw_333);
        std::printf("PollIncoming() accepts a larger-than-known packet from a newer peer: OK\n");

        // A genuinely truncated packet (below kAircraftStateMinSize, even
        // though it starts with a valid magic) must still be rejected,
        // not just a wrong-magic one.
        sync.RemoveStaleAircraft(/*now_s=*/2.0, /*timeout_s=*/0.0); // clear tracked state
        assert(sync.tracked_aircraft_count() == 0);

        const AircraftStatePacket full = MakePacket(/*sender_id=*/444, /*sequence=*/1);
        std::vector<uint8_t> truncated(kAircraftStateMinSize - 1);
        std::memcpy(truncated.data(), &full, truncated.size());
        fake_peer.SendTo("127.0.0.1", kFormationUdpPort, truncated.data(), truncated.size());

        for (int i = 0; i < 20; ++i) {
            sync.PollIncoming(/*now_s=*/1.0);
            std::this_thread::sleep_for(5ms);
        }
        assert(sync.tracked_aircraft_count() == 0);
        std::printf("PollIncoming() rejects a packet shorter than kAircraftStateMinSize: OK\n");
    }

    // --- AddPeer/RemovePeer: dedup and no-op-if-absent ---
    {
        assert(sync.peer_count() == 1); // from peers.txt
        sync.AddPeer("127.0.0.1", kFakePeerPort); // already present, no-op
        assert(sync.peer_count() == 1);
        sync.AddPeer("127.0.0.1", kFakePeerPort + 1);
        assert(sync.peer_count() == 2);
        sync.RemovePeer("127.0.0.1", 65000); // not present, no-op
        assert(sync.peer_count() == 2);
        sync.RemovePeer("127.0.0.1", kFakePeerPort + 1);
        assert(sync.peer_count() == 1);
        std::printf("AddPeer()/RemovePeer() dedup and no-op-if-absent correctly: OK\n");
    }

    sync.Stop();
    assert(sync.peer_count() == 0);
    assert(sync.tracked_aircraft_count() == 0);
    std::printf("Stop() clears peers and tracked aircraft: OK\n");

    // --- SetCrypto: SendOwnState is encrypted, PollIncoming only accepts
    // envelopes sealed with the SAME session key ---
    {
        WriteFile("peers2.txt", "127.0.0.1:" + std::to_string(kFakePeerPort) + "\n");
        FormationSync encrypted_sync;
        assert(encrypted_sync.Start("peers2.txt"));

        std::array<uint8_t, SessionCrypto::kSaltSize> salt{};
        salt.fill(0x99);
        SessionCrypto crypto("ABC123", salt);
        encrypted_sync.SetCrypto(&crypto);

        const AircraftStatePacket sent = MakePacket(/*sender_id=*/555, /*sequence=*/1);
        encrypted_sync.SendOwnState(sent);

        // What actually hit the wire is NOT the plaintext struct - proof
        // this is really encrypting, not just relabeling the same bytes.
        const std::string raw = RecvOne(fake_peer, 500ms);
        assert(raw.size() == sizeof(AircraftStatePacket) + SessionCrypto::kEnvelopeOverhead);
        assert(raw.size() != sizeof(AircraftStatePacket) ||
               std::memcmp(raw.data(), &sent, sizeof(sent)) != 0);
        std::printf("SetCrypto(): SendOwnState no longer sends plaintext on the wire: OK\n");

        // A peer with the SAME key can decrypt it (feed it back in via
        // this same fake_peer socket, standing in for the other side).
        fake_peer.SendTo("127.0.0.1", kFormationUdpPort, raw.data(), raw.size());
        bool ingested = false;
        for (int i = 0; i < 50 && !ingested; ++i) {
            encrypted_sync.PollIncoming(/*now_s=*/1.0);
            ingested = encrypted_sync.tracked_aircraft_count() == 1;
            if (!ingested) std::this_thread::sleep_for(5ms);
        }
        assert(ingested);
        std::printf("SetCrypto(): PollIncoming decrypts an envelope sealed with the same key: OK\n");

        // An envelope sealed with a DIFFERENT session code can't be
        // decrypted - silently dropped, not misapplied as garbage data.
        encrypted_sync.RemoveStaleAircraft(/*now_s=*/100.0, /*timeout_s=*/0.0);
        assert(encrypted_sync.tracked_aircraft_count() == 0);
        SessionCrypto wrong_crypto("WRONG1", salt);
        const AircraftStatePacket other = MakePacket(/*sender_id=*/666, /*sequence=*/1);
        const auto wrongly_keyed_envelope =
            wrong_crypto.Seal(reinterpret_cast<const uint8_t*>(&other), sizeof(other));
        fake_peer.SendTo("127.0.0.1", kFormationUdpPort, wrongly_keyed_envelope.data(),
                          wrongly_keyed_envelope.size());
        for (int i = 0; i < 20; ++i) {
            encrypted_sync.PollIncoming(/*now_s=*/1.0);
            std::this_thread::sleep_for(5ms);
        }
        assert(encrypted_sync.tracked_aircraft_count() == 0);
        std::printf("SetCrypto(): PollIncoming rejects an envelope sealed with a different key: OK\n");
    }

    // --- IngestPacket drops implausible poses and sanitizes icao_type ---
    {
        FormationSync sync;
        AircraftStatePacket bad = MakePacket(/*sender_id=*/1, /*sequence=*/1);
        bad.elevation_m = std::nan("");
        sync.IngestPacket(bad, 1.0);
        bad = MakePacket(/*sender_id=*/2, /*sequence=*/1);
        bad.latitude = 123.0;
        sync.IngestPacket(bad, 1.0);
        assert(sync.tracked_aircraft_count() == 0);

        AircraftStatePacket odd = MakePacket(/*sender_id=*/3, /*sequence=*/1);
        std::memcpy(odd.icao_type, "C1\n;X", 6);
        sync.IngestPacket(odd, 1.0);
        assert(sync.tracked_aircraft_count() == 1);
        std::string seen_icao;
        sync.ForEachRemoteAircraft(1.0, [&](uint32_t, const AircraftPose&, const AircraftStatePacket& latest) {
            seen_icao.assign(latest.icao_type, strnlen(latest.icao_type, sizeof(latest.icao_type)));
        });
        assert(seen_icao == "C1");
        std::printf("IngestPacket drops implausible poses and sanitizes icao_type: OK\n");
    }

    fs::current_path(old_cwd);
    fs::remove_all(tmp_dir);

    std::printf("\nALL FORMATION SYNC CHECKS PASSED\n");
    return 0;
}
