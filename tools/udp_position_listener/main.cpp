// Standalone Phase 0 spike tool: listens on the loopback UDP port the
// XPMultiCrew plugin sends position packets to, and prints what it
// receives. Proves the plugin -> network chain works before adding
// internet/NAT-traversal (Phase 2) or a real transport (QUIC/ENet).
//
// Usage: run this, then load the plugin into a running X-Plane instance on
// the same machine and start a flight.

#include "flytogether/position_packet.h"
#include "flytogether/udp_socket.h"

#include <cstdio>
#include <cstring>

int main() {
    flytogether::UdpSocket socket;
    if (!socket.Open()) {
        std::fprintf(stderr, "Failed to open UDP socket\n");
        return 1;
    }
    if (!socket.Bind(flytogether::kSpikeUdpPort)) {
        std::fprintf(stderr, "Failed to bind UDP port %u\n",
                     flytogether::kSpikeUdpPort);
        return 1;
    }

    std::printf("Listening for XPMultiCrew position packets on UDP port %u...\n",
                flytogether::kSpikeUdpPort);

    flytogether::PositionPacket packet;
    while (true) {
        const int received = socket.ReceiveFrom(&packet, sizeof(packet));
        if (received != static_cast<int>(sizeof(packet))) {
            continue; // partial/garbage packet, ignore
        }
        if (packet.magic != flytogether::kPositionPacketMagic) {
            continue;
        }

        std::printf("seq=%u lat=%.6f lon=%.6f elev=%.1fm\n", packet.sequence,
                    packet.latitude, packet.longitude, packet.elevation);
        std::fflush(stdout);
    }
}
