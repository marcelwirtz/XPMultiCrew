// Sends a synthetic "ghost" aircraft flying a slow circle, as
// flytogether::AircraftStatePacket UDP packets, to a running XPMultiCrew
// plugin on the same machine (127.0.0.1, kFormationUdpPort). Lets you test
// Formation mode's dead reckoning and CSL/TCAS rendering solo, without a
// second pilot or PC: the plugin's receive socket isn't gated by its peer
// list, so it will pick this up as soon as it's running.
//
// Usage: formation_fake_peer [center_lat] [center_lon] [center_elev_m]
// Defaults put the circle a few hundred meters from Boeing Field (KBFI) -
// pass your actual aircraft's position (visible in Log.txt or the map) so
// the ghost appears somewhere you'll actually look.

#include "flytogether/aircraft_state.h"
#include "flytogether/udp_socket.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char** argv) {
    double center_lat = 47.5301;
    double center_lon = -122.3009;
    double center_elev_m = 30.0;

    if (argc >= 4) {
        center_lat = std::atof(argv[1]);
        center_lon = std::atof(argv[2]);
        center_elev_m = std::atof(argv[3]);
    } else {
        std::printf("No position given, using default near KBFI (%.4f, %.4f).\n"
                     "Usage: %s <lat> <lon> <elev_m>  (pass your own aircraft's "
                     "position to see the ghost nearby)\n\n",
                     center_lat, center_lon, argv[0]);
    }

    flytogether::UdpSocket socket;
    if (!socket.Open()) {
        std::fprintf(stderr, "Failed to open UDP socket\n");
        return 1;
    }

    constexpr double kEarthRadiusM = 6378137.0;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kRadiusM = 400.0;   // circle radius around the center point
    constexpr double kPeriodS = 60.0;    // seconds per full orbit
    constexpr double kSendHz = 10.0;

    const uint32_t sender_id = 0xF4CE0001u; // fixed, recognizable id ("FAKE0001"-ish)
    uint32_t sequence = 0;

    std::printf("Sending a ghost aircraft orbiting (%.4f, %.4f) at UDP port %u. "
                "Ctrl+C to stop.\n",
                center_lat, center_lon, flytogether::kFormationUdpPort);

    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double angle_rad = 2.0 * kPi * (t / kPeriodS);

        const double north_m = kRadiusM * std::cos(angle_rad);
        const double east_m = kRadiusM * std::sin(angle_rad);
        const double lat = center_lat + (north_m / kEarthRadiusM) * (180.0 / kPi);
        const double lon = center_lon +
            (east_m / (kEarthRadiusM * std::cos(center_lat * kPi / 180.0))) * (180.0 / kPi);

        // Heading tangent to the circle (direction of travel).
        double heading_deg = std::fmod((angle_rad + kPi / 2.0) * (180.0 / kPi), 360.0);
        if (heading_deg < 0.0) heading_deg += 360.0;

        flytogether::AircraftStatePacket packet;
        packet.sender_id = sender_id;
        packet.sequence = sequence++;
        packet.latitude = lat;
        packet.longitude = lon;
        packet.elevation_m = center_elev_m;
        packet.heading_deg = static_cast<float>(heading_deg);
        packet.pitch_deg = 0.0f;
        packet.roll_deg = 15.0f; // banked into the turn, so it's visibly animated
        packet.gear_ratio = 1.0f;
        packet.flap_ratio = 0.0f;
        packet.speedbrake_ratio = 0.0f;
        packet.engine_ratio = 0.6f;
        packet.light_bits = flytogether::LightBits::kBeacon | flytogether::LightBits::kNav |
                             flytogether::LightBits::kStrobe;
        std::memcpy(packet.icao_type, "C172", 4);

        socket.SendTo("127.0.0.1", flytogether::kFormationUdpPort, &packet, sizeof(packet));

        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(1000.0 / kSendHz)));
    }
}
