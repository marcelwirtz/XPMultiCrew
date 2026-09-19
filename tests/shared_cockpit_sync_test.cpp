// Verifies SharedCockpitSync's master/client networking glue end-to-end
// over real loopback UDP sockets - same approach as
// formation_sync_test.cpp/rendezvous_client_test.cpp. RemoteAircraft's own
// dead-reckoning math is already covered by remote_aircraft_test.cpp; this
// focuses on what's specific to this class: role-gating (master-only send,
// client-only receive), the direct-socket path, the relay fallback path,
// and master-staleness detection.

#include "shared_cockpit/shared_cockpit_sync.h"

#include <cassert>
#include <chrono>
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
    // --- CLIENT: direct-socket receive, staleness, role-gated SendOwnState ---
    {
        SharedCockpitSync client;
        assert(client.Start(SharedCockpitRole::kClient, {}));
        assert(client.role() == SharedCockpitRole::kClient);
        assert(!client.HasMasterState());

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

        const AircraftPose pose = client.ComputeMasterPose(1.0);
        assert(pose.latitude == packet.latitude);
        assert(pose.longitude == packet.longitude);
        std::printf("CLIENT: direct-socket receive, staleness and role-gated SendOwnState: OK\n");

        // A malformed relayed packet (wrong magic) must not be applied.
        AircraftStatePacket bad = packet;
        bad.magic = 0xDEADBEEF;
        bad.sequence = 2;
        client.IngestRelayedPacket(&bad, sizeof(bad), 1.0);
        assert(client.ComputeMasterPose(1.0).heading_deg == packet.heading_deg); // unchanged

        // A well-formed relayed packet is applied exactly like a direct one.
        AircraftStatePacket relayed = MakePacket(/*sender_id=*/42, /*sequence=*/2);
        relayed.heading_deg = 270.0f;
        client.IngestRelayedPacket(&relayed, sizeof(relayed), 1.0);
        assert(client.ComputeMasterPose(1.0).heading_deg == 270.0f);
        std::printf("CLIENT: IngestRelayedPacket applies valid, rejects malformed: OK\n");

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

    std::printf("\nALL SHARED COCKPIT SYNC CHECKS PASSED\n");
    return 0;
}
