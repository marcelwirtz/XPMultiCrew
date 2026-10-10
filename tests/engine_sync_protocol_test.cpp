// EngineState wire format (shared_cockpit/engine_sync_protocol.h).

#include "shared_cockpit/engine_sync_protocol.h"

#include <cassert>
#include <cstdio>
#include <limits>

using namespace flytogether;

namespace {

void TestRoundTrip() {
    EngineState in;
    in.sequence = 99;
    in.engines = 2;
    in.engine[0].prop_rad_s = 250.0f;
    in.engine[0].engine_rad_s = 250.0f;
    in.engine[0].egt_c = 700.0f;
    in.engine[0].cht_c = 180.0f;
    in.engine[0].oil_temp = 190.0f;
    in.engine[0].oil_press_psi = 60.0f;
    in.engine[0].fuel_flow_kg_s = 0.012f;
    in.engine[0].manifold_inhg = 23.0f;
    in.engine[1].n1_percent = 97.0f;
    in.engine[1].itt_c = 650.0f;
    in.engine[1].torque_nm = 1800.0f;
    in.tanks = 2;
    in.fuel_kg[0] = 40.0f;
    in.fuel_kg[1] = 38.5f;
    const auto bytes = EncodeEngineState(in);
    const auto out = DecodeEngineState(bytes.data(), bytes.size());
    assert(out);
    assert(out->sequence == 99 && out->engines == 2 && out->tanks == 2);
    assert(out->engine[0].prop_rad_s == 250.0f && out->engine[0].egt_c == 700.0f);
    assert(out->engine[0].cht_c == 180.0f && out->engine[0].oil_temp == 190.0f);
    assert(out->engine[0].oil_press_psi == 60.0f && out->engine[0].fuel_flow_kg_s == 0.012f);
    assert(out->engine[0].manifold_inhg == 23.0f);
    assert(out->engine[1].n1_percent == 97.0f && out->engine[1].itt_c == 650.0f);
    assert(out->engine[1].torque_nm == 1800.0f);
    assert(out->fuel_kg[0] == 40.0f && out->fuel_kg[1] == 38.5f);
    std::printf("TestRoundTrip: OK\n");
}

void TestRejectsMalformed() {
    EngineState in;
    in.engines = 1;
    in.tanks = 1;
    auto bytes = EncodeEngineState(in);
    assert(!DecodeEngineState(bytes.data(), bytes.size() - 1)); // truncated
    assert(!DecodeEngineState(bytes.data(), 3));
    assert(!DecodeEngineState(nullptr, 0));
    auto wrong_magic = bytes;
    wrong_magic[0] ^= 0xff;
    assert(!DecodeEngineState(wrong_magic.data(), wrong_magic.size()));
    auto too_many = bytes;
    too_many[5] = kEngineSyncMaxEngines + 1;
    assert(!DecodeEngineState(too_many.data(), too_many.size()));
    auto too_many_tanks = bytes;
    too_many_tanks[6] = kEngineSyncMaxTanks + 1;
    assert(!DecodeEngineState(too_many_tanks.data(), too_many_tanks.size()));
    std::printf("TestRejectsMalformed: OK\n");
}

void TestClampsHostileValues() {
    EngineState in;
    in.engines = 1;
    in.engine[0].prop_rad_s = std::numeric_limits<float>::quiet_NaN();
    in.engine[0].oil_press_psi = -50.0f;
    in.engine[0].egt_c = 1.0e9f;
    const auto bytes = EncodeEngineState(in);
    const auto out = DecodeEngineState(bytes.data(), bytes.size());
    assert(out);
    assert(out->engine[0].prop_rad_s == 0.0f);
    assert(out->engine[0].oil_press_psi == 0.0f);
    assert(out->engine[0].egt_c == 2000.0f);
    std::printf("TestClampsHostileValues: OK\n");
}

} // namespace

int main() {
    TestRoundTrip();
    TestRejectsMalformed();
    TestClampsHostileValues();
    std::printf("\nALL ENGINE SYNC PROTOCOL CHECKS PASSED\n");
    return 0;
}
