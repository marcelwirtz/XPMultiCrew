// Pure-logic test for shared_cockpit/command_sync_protocol.h.

#include "shared_cockpit/command_sync_protocol.h"

#include <cassert>
#include <cstdio>

using namespace flytogether;

int main() {
    CommandMessage msg;
    msg.sender_id = 42;
    msg.sequence = 7;
    msg.phase = CommandPhase::kEnd;
    msg.name = "sim/autopilot/heading";
    const auto bytes = EncodeCommandMessage(msg);
    const auto back = DecodeCommandMessage(bytes.data(), bytes.size());
    assert(back && back->sender_id == 42 && back->sequence == 7 && back->phase == CommandPhase::kEnd &&
           back->name == "sim/autopilot/heading");
    std::printf("Encode/decode round trip: OK\n");

    assert(!DecodeCommandMessage(bytes.data(), bytes.size() - 1)); // truncated name
    auto bad = bytes;
    bad[12] = 9; // unknown phase
    assert(!DecodeCommandMessage(bad.data(), bad.size()));
    bad = bytes;
    bad[20] = '\n'; // control character in the name
    assert(!DecodeCommandMessage(bad.data(), bad.size()));
    std::printf("Malformed messages are rejected: OK\n");

    CommandDedup dedup;
    assert(dedup.Accept(1, 100));
    assert(!dedup.Accept(1, 100)); // same message via the second path
    assert(dedup.Accept(2, 100));  // other sender, own sequence space
    for (uint32_t s = 101; s < 101 + CommandDedup::kWindow; ++s) assert(dedup.Accept(1, s));
    assert(dedup.Accept(1, 100)); // fell out of the window long ago
    std::printf("Dedup drops the duplicate, keeps senders apart: OK\n");

    std::printf("\nALL COMMAND SYNC PROTOCOL CHECKS PASSED\n");
    return 0;
}
