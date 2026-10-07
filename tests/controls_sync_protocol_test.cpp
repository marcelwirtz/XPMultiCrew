// ControlsState wire format (shared_cockpit/controls_sync_protocol.h).

#include "shared_cockpit/controls_sync_protocol.h"

#include <cassert>
#include <cstdio>
#include <limits>

using namespace flytogether;

namespace {

void TestRoundTrip() {
    ControlsState in;
    in.sequence = 4711;
    in.yoke_pitch = -0.25f;
    in.yoke_roll = 0.5f;
    in.yoke_heading = 0.1f;
    in.left_brake = 0.3f;
    in.right_brake = 1.0f;
    in.engines = 2;
    in.throttle[0] = 0.8f;
    in.throttle[1] = -0.5f; // beta
    in.mixture[0] = 1.0f;
    in.mixture[1] = 0.7f;
    in.prop[1] = 0.9f;
    const auto bytes = EncodeControlsState(in);
    const auto out = DecodeControlsState(bytes.data(), bytes.size());
    assert(out);
    assert(out->sequence == 4711);
    assert(out->yoke_pitch == -0.25f && out->yoke_roll == 0.5f && out->yoke_heading == 0.1f);
    assert(out->left_brake == 0.3f && out->right_brake == 1.0f);
    assert(out->engines == 2);
    assert(out->throttle[0] == 0.8f && out->throttle[1] == -0.5f);
    assert(out->mixture[1] == 0.7f && out->prop[1] == 0.9f);
    std::printf("TestRoundTrip: OK\n");
}

void TestRejectsMalformed() {
    ControlsState in;
    in.engines = 1;
    auto bytes = EncodeControlsState(in);
    assert(!DecodeControlsState(bytes.data(), bytes.size() - 1)); // truncated engine block
    assert(!DecodeControlsState(bytes.data(), 3));
    assert(!DecodeControlsState(nullptr, 0));
    auto wrong_magic = bytes;
    wrong_magic[0] ^= 0xff;
    assert(!DecodeControlsState(wrong_magic.data(), wrong_magic.size()));
    auto too_many = bytes;
    too_many[5] = kControlsMaxEngines + 1;
    assert(!DecodeControlsState(too_many.data(), too_many.size()));
    std::printf("TestRejectsMalformed: OK\n");
}

void TestClampsHostileValues() {
    ControlsState in;
    in.engines = 1;
    in.yoke_pitch = 7.0f;
    in.left_brake = -3.0f;
    in.throttle[0] = std::numeric_limits<float>::quiet_NaN();
    const auto bytes = EncodeControlsState(in);
    const auto out = DecodeControlsState(bytes.data(), bytes.size());
    assert(out);
    assert(out->yoke_pitch == 1.0f);
    assert(out->left_brake == 0.0f);
    assert(out->throttle[0] == 0.0f);
    std::printf("TestClampsHostileValues: OK\n");
}

} // namespace

int main() {
    TestRoundTrip();
    TestRejectsMalformed();
    TestClampsHostileValues();
    std::printf("\nALL CONTROLS SYNC PROTOCOL CHECKS PASSED\n");
    return 0;
}
