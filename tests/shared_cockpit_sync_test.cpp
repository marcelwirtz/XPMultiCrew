// Verifies SharedCockpitSync's master/client networking glue end-to-end
// over real loopback UDP sockets - same approach as
// formation_sync_test.cpp/rendezvous_client_test.cpp. RemoteAircraft's own
// dead-reckoning math is already covered by remote_aircraft_test.cpp; this
// focuses on what's specific to this class: role-gating (master-only send,
// client-only receive), the direct-socket path, the relay fallback path,
// and master-staleness detection.

#include "shared_cockpit/shared_cockpit_sync.h"

#include "net/session_crypto.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace flytogether;
using namespace std::chrono_literals;

namespace {

AircraftStatePacket MakePacket(uint32_t sender_id, uint32_t sequence) {
    AircraftStatePacket packet;
    packet.sender_id = sender_id;
    packet.sequence = sequence;
    packet.latitude = 47.5;
    packet.longitude = -122.3;
    packet.elevation_m = 100.0;
    packet.heading_deg = 90.0f;
    std::memcpy(packet.icao_type, "C172", 4);
    return packet;
}

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
    // --- SetRole: swapping roles forgets the old master state, so swapping
    // back never applies a stale pose ---
    {
        SharedCockpitSync sync;
        sync.SetRole(SharedCockpitRole::kClient);
        const AircraftStatePacket packet = MakePacket(/*sender_id=*/7, /*sequence=*/1);
        sync.IngestRelayedPacket(&packet, sizeof(packet), 1.0);
        assert(sync.HasMasterState());
        assert(sync.LatestMasterPacket() && sync.LatestMasterPacket()->sender_id == 7);
        sync.SetRole(SharedCockpitRole::kMaster);
        sync.SetRole(SharedCockpitRole::kClient);
        assert(!sync.HasMasterState());
        assert(sync.LatestMasterPacket() == nullptr);
        std::printf("SetRole forgets master state across a swap: OK\n");
    }

    // --- CLIENT: direct-socket receive, staleness, role-gated SendOwnState ---
    {
        SharedCockpitSync client;
        assert(client.Start(SharedCockpitRole::kClient, {}));
        assert(client.role() == SharedCockpitRole::kClient);
        assert(!client.HasMasterState());
        assert(client.MasterIcaoType().empty()); // nothing received yet
        assert(!client.HasMasterLinkQualityData());

        // A client never sends anything of its own - SendOwnState is a
        // no-op unless role() == kMaster.
        client.SendOwnState(MakePacket(1, 1)); // must not crash / must do nothing observable

        UdpSocket fake_master;
        assert(fake_master.Open());
        const AircraftStatePacket packet = MakePacket(/*sender_id=*/42, /*sequence=*/1);
        assert(fake_master.SendTo("127.0.0.1", kSharedCockpitUdpPort, &packet, sizeof(packet)));

        bool received = false;
        for (int i = 0; i < 50 && !received; ++i) {
            client.PollIncoming(/*now_s=*/1.0);
            received = client.HasMasterState();
            if (!received) std::this_thread::sleep_for(5ms);
        }
        assert(received);
        assert(!client.IsMasterStale(/*now_s=*/1.0));
        assert(client.IsMasterStale(/*now_s=*/100.0)); // default timeout_s = 5.0
        assert(client.MasterIcaoType() == "C172"); // MakePacket() sets icao_type to "C172"
        // One received packet already gives LinkQualityTracker a baseline
        // sequence to measure the next gap against (see net/link_quality.h) -
        // HasMasterLinkQualityData() is true from here, even though
        // MasterLinkLossRatio() has no delta to report yet (still reads
        // 0.0, the same starting value a genuinely loss-free link has -
        // see the next check below).
        assert(client.HasMasterLinkQualityData());
        assert(client.MasterLinkLossRatio() < 1e-6);

        const AircraftPose pose = client.ComputeMasterPose(1.0);
        assert(pose.latitude == packet.latitude);
        assert(pose.longitude == packet.longitude);
        std::printf("CLIENT: direct-socket receive, staleness and role-gated SendOwnState: OK\n");

        // A third packet with a deliberate sequence gap (skipping 2) should
        // move MasterLinkLossRatio() off zero - proof it's actually wired
        // to the master link, not just always reading its zero-loss
        // starting value (already checked above).
        const AircraftStatePacket packet3 = MakePacket(/*sender_id=*/42, /*sequence=*/4);
        assert(fake_master.SendTo("127.0.0.1", kSharedCockpitUdpPort, &packet3, sizeof(packet3)));
        double loss_ratio = 0.0;
        for (int i = 0; i < 50 && loss_ratio < 1e-6; ++i) {
            client.PollIncoming(/*now_s=*/1.0);
            loss_ratio = client.MasterLinkLossRatio();
            if (loss_ratio < 1e-6) std::this_thread::sleep_for(5ms);
        }
        assert(loss_ratio > 1e-6);
        std::printf("CLIENT: MasterIcaoType/MasterLinkLossRatio report the master link's state: OK\n");

        // A malformed relayed packet (wrong magic) must not be applied.
        // sequence=5: must stay newer than packet3's sequence=4 above, or
        // RemoteAircraft::OnPacketReceived would reject it as stale
        // regardless of the magic check this is actually testing.
        AircraftStatePacket bad = packet;
        bad.magic = 0xDEADBEEF;
        bad.sequence = 5;
        client.IngestRelayedPacket(&bad, sizeof(bad), 1.0);
        assert(client.ComputeMasterPose(1.0).heading_deg == packet.heading_deg); // unchanged

        // A well-formed relayed packet is applied exactly like a direct one.
        // ComputeMasterPose() now blends orientation toward a new packet
        // over time (RemoteAircraft's rendered-pose catch-up, see
        // sync/remote_aircraft.h) rather than jumping to it instantly, so
        // this checks it a couple of seconds later (comfortably past the
        // SLERP's convergence time constant) instead of at the exact same
        // instant the packet arrived - a real flight-loop tick always
        // advances `now_s` between receiving and rendering, unlike calling
        // ComputeMasterPose() three times in a row at a literal 1.0 the
        // way this test body does.
        AircraftStatePacket relayed = MakePacket(/*sender_id=*/42, /*sequence=*/6);
        relayed.heading_deg = 270.0f;
        client.IngestRelayedPacket(&relayed, sizeof(relayed), 1.0);
        const float converged_heading = client.ComputeMasterPose(3.0).heading_deg;
        assert(std::fabs(converged_heading - 270.0f) < 0.01f);
        std::printf("CLIENT: IngestRelayedPacket applies valid, rejects malformed: OK\n");

        client.Stop();
    }

    // --- CLIENT: forward/backward-compatible on packet size (both the
    // direct-socket and relay paths) - see aircraft_state.h's
    // kAircraftStateMinSize comment. Fresh instance, isolated from the
    // block above.
    {
        SharedCockpitSync client;
        assert(client.Start(SharedCockpitRole::kClient, {}));

        UdpSocket fake_master;
        assert(fake_master.Open());

        // Direct-socket path: a packet with extra trailing bytes
        // (simulating a newer master build) is still accepted.
        const AircraftStatePacket bigger = MakePacket(/*sender_id=*/50, /*sequence=*/1);
        std::vector<uint8_t> padded(sizeof(bigger) + 16, 0xCD);
        std::memcpy(padded.data(), &bigger, sizeof(bigger));
        assert(fake_master.SendTo("127.0.0.1", kSharedCockpitUdpPort, padded.data(), padded.size()));

        bool received = false;
        for (int i = 0; i < 50 && !received; ++i) {
            client.PollIncoming(/*now_s=*/1.0);
            received = client.HasMasterState();
            if (!received) std::this_thread::sleep_for(5ms);
        }
        assert(received);
        // First-ever ComputeMasterPose() call on this instance always
        // snaps exactly to the target (RemoteAircraft's rendered-pose
        // blend only engages from the second call onward) - see
        // sync/remote_aircraft.h's class comment.
        assert(client.ComputeMasterPose(1.0).latitude == bigger.latitude);
        std::printf("CLIENT: PollIncoming accepts a larger-than-known direct packet: OK\n");

        // Relay path: same tolerance, and a genuinely truncated payload
        // (below kAircraftStateMinSize) must still be rejected there too.
        // Each check below evaluates at a later now_s than the previous
        // one: the rendered-pose blend needs nonzero elapsed time to
        // react to a newly-ingested packet (a same-instant re-query just
        // returns the previously-held pose, unrelated to this test) - the
        // ~55 km jump to latitude 48.0 is far beyond the blend's 50 m
        // hard-reset threshold, so any nonzero elapsed time snaps to it
        // exactly rather than needing to wait out a gradual catch-up.
        AircraftStatePacket relay_bigger = MakePacket(/*sender_id=*/50, /*sequence=*/2);
        relay_bigger.latitude = 48.0;
        std::vector<uint8_t> relay_padded(sizeof(relay_bigger) + 16, 0xCD);
        std::memcpy(relay_padded.data(), &relay_bigger, sizeof(relay_bigger));
        client.IngestRelayedPacket(relay_padded.data(), relay_padded.size(), 1.0);
        assert(client.ComputeMasterPose(1.1).latitude == 48.0);
        std::printf("CLIENT: IngestRelayedPacket accepts a larger-than-known relayed packet: OK\n");

        std::vector<uint8_t> truncated(kAircraftStateMinSize - 1, 0);
        std::memcpy(truncated.data(), &relay_bigger, truncated.size());
        client.IngestRelayedPacket(truncated.data(), truncated.size(), 1.1);
        assert(client.ComputeMasterPose(1.2).latitude == 48.0); // unchanged - truncated one rejected
        std::printf("CLIENT: IngestRelayedPacket rejects a packet shorter than kAircraftStateMinSize: OK\n");

        client.Stop();
    }

    // --- MASTER: direct send to configured peers + relay fan-out ---
    {
        // Peers must be known up front for a master (SendOwnState iterates
        // this list directly, unlike a client which only ever receives) -
        // bind the fake client to a fixed test port rather than relying on
        // ephemeral-port discovery.
        constexpr uint16_t kFakeClientPort = 49060;
        UdpSocket fake_client;
        assert(fake_client.Open());
        assert(fake_client.Bind(kFakeClientPort));
        fake_client.SetNonBlocking(true);

        SharedCockpitSync master;
        assert(master.Start(SharedCockpitRole::kMaster, {Peer{"127.0.0.1", kFakeClientPort}}));
        assert(master.role() == SharedCockpitRole::kMaster);

        std::vector<uint8_t> relayed_bytes;
        master.SetRelaySender([&](const void* data, size_t len) {
            relayed_bytes.assign(static_cast<const uint8_t*>(data),
                                  static_cast<const uint8_t*>(data) + len);
        });

        const AircraftStatePacket packet = MakePacket(/*sender_id=*/7, /*sequence=*/5);
        master.SendOwnState(packet);

        const std::string raw = RecvOne(fake_client, 500ms);
        assert(raw.size() == sizeof(AircraftStatePacket));
        AircraftStatePacket received;
        std::memcpy(&received, raw.data(), sizeof(received));
        assert(received.sender_id == 7);
        assert(received.sequence == 5);
        std::printf("MASTER: SendOwnState reaches the configured peer directly: OK\n");

        assert(relayed_bytes.size() == sizeof(AircraftStatePacket));
        AircraftStatePacket via_relay;
        std::memcpy(&via_relay, relayed_bytes.data(), sizeof(via_relay));
        assert(via_relay.sender_id == 7);
        std::printf("MASTER: SendOwnState also hands the packet to the relay sender: OK\n");

        // A master never applies incoming packets to itself - PollIncoming
        // is a no-op unless role() == kClient.
        master.PollIncoming(1.0);
        assert(!master.HasMasterState());
        std::printf("MASTER: PollIncoming is a no-op (master never tracks a 'master state' of its own): OK\n");

        master.Stop();
    }

    // --- SetCrypto: master's direct+relay sends are both encrypted, and
    // a client with the same key can decrypt both paths ---
    {
        constexpr uint16_t kFakeClientPort2 = 49061;
        UdpSocket fake_client;
        assert(fake_client.Open());
        assert(fake_client.Bind(kFakeClientPort2));
        fake_client.SetNonBlocking(true);

        std::array<uint8_t, SessionCrypto::kSaltSize> salt{};
        salt.fill(0x55);
        SessionCrypto crypto("ABC123", salt);

        SharedCockpitSync master;
        assert(master.Start(SharedCockpitRole::kMaster, {Peer{"127.0.0.1", kFakeClientPort2}}));
        master.SetCrypto(&crypto);
        std::vector<uint8_t> relayed_bytes;
        master.SetRelaySender([&](const void* data, size_t len) {
            relayed_bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
        });

        const AircraftStatePacket packet = MakePacket(/*sender_id=*/9, /*sequence=*/1);
        master.SendOwnState(packet);

        const std::string raw = RecvOne(fake_client, 500ms);
        assert(raw.size() == sizeof(AircraftStatePacket) + SessionCrypto::kEnvelopeOverhead);
        assert(relayed_bytes.size() == sizeof(AircraftStatePacket) + SessionCrypto::kEnvelopeOverhead);
        assert(relayed_bytes ==
               std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()),
                                     reinterpret_cast<const uint8_t*>(raw.data()) + raw.size()));
        std::printf("SetCrypto(): MASTER's direct and relay sends are both encrypted and identical: OK\n");

        // master's socket must free kSharedCockpitUdpPort before a client
        // below can bind to it - both roles bind the same fixed port (fine
        // across two real machines, not within one test process).
        master.Stop();

        {
            SharedCockpitSync client;
            assert(client.Start(SharedCockpitRole::kClient, {}));
            client.SetCrypto(&crypto);

            // Direct path.
            assert(fake_client.SendTo("127.0.0.1", kSharedCockpitUdpPort, raw.data(), raw.size()));
            bool received_direct = false;
            for (int i = 0; i < 50 && !received_direct; ++i) {
                client.PollIncoming(1.0);
                received_direct = client.HasMasterState();
                if (!received_direct) std::this_thread::sleep_for(5ms);
            }
            assert(received_direct);
            std::printf("SetCrypto(): CLIENT decrypts a direct-UDP envelope: OK\n");
            client.Stop();
        }

        {
            // Relay path: IngestRelayedPacket takes already-decrypted
            // plaintext (see its own comment for why - the real caller is
            // plugin_main.cpp's relay dispatcher, which has to decrypt
            // once up front before it can even peek the magic byte to
            // route between message shapes sharing the relay stream), so
            // this test decrypts relayed_bytes itself first, standing in
            // for that dispatcher.
            const auto relay_plaintext = crypto.Open(relayed_bytes);
            assert(relay_plaintext.has_value());

            SharedCockpitSync client2; // fresh instance so HasMasterState() starts over
            assert(client2.Start(SharedCockpitRole::kClient, {}));
            client2.IngestRelayedPacket(relay_plaintext->data(), relay_plaintext->size(), 1.0);
            assert(client2.HasMasterState());
            std::printf("SetCrypto(): CLIENT applies a relayed message the dispatcher already decrypted: OK\n");
            client2.Stop();
        }

        {
            // A DIFFERENT session code can't even decrypt the relayed
            // envelope in the first place - this is where that failure
            // actually happens now (at the dispatcher, before any
            // IngestRelayedPacket call), not inside SharedCockpitSync.
            SessionCrypto wrong_crypto("WRONG1", salt);
            assert(!wrong_crypto.Open(relayed_bytes).has_value());
            std::printf("SetCrypto(): a different session key can't decrypt the relayed envelope: OK\n");
        }
    }

    std::printf("\nALL SHARED COCKPIT SYNC CHECKS PASSED\n");
    return 0;
}
