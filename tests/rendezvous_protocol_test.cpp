// Pure-logic tests for the plugin's rendezvous client protocol codec
// (base64 + the hand-rolled JSON de/encoder in rendezvous_protocol.cpp).
// No XPLM or networking involved.

#include "formation/base64.h"
#include "formation/rendezvous_protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace flytogether;

namespace {

void TestBase64RoundTripsArbitraryLengths() {
    for (size_t len = 0; len <= 16; ++len) {
        std::vector<uint8_t> data(len);
        for (size_t i = 0; i < len; ++i) data[i] = static_cast<uint8_t>(i * 37 + 5);

        const std::string encoded = Base64Encode(data.data(), data.size());
        const std::vector<uint8_t> decoded = Base64Decode(encoded);

        assert(decoded.size() == data.size());
        assert(decoded == data);
    }
    std::printf("TestBase64RoundTripsArbitraryLengths: OK\n");
}

void TestBase64KnownVector() {
    // "hello-world" -> base64, verified against Python's base64.b64encode.
    const std::string encoded = Base64Encode("hello-world", 11);
    assert(encoded == "aGVsbG8td29ybGQ=");
    std::printf("TestBase64KnownVector: OK\n");
}

void TestEncodeClientMessageShapes() {
    RendezvousClientMessage create;
    create.type = "create_session";
    assert(EncodeClientMessage(create) == "{\"type\":\"create_session\"}");

    RendezvousClientMessage join;
    join.type = "join_session";
    join.code = "2K2SYJ";
    assert(EncodeClientMessage(join) == "{\"type\":\"join_session\",\"code\":\"2K2SYJ\"}");

    RendezvousClientMessage relay;
    relay.type = "relay";
    relay.payload = "aGVsbG8=";
    assert(EncodeClientMessage(relay) == "{\"type\":\"relay\",\"payload\":\"aGVsbG8=\"}");

    std::printf("TestEncodeClientMessageShapes: OK\n");
}

void TestDecodeServerMessageShapes() {
    // Exact shapes server/protocol.go's compact encoding/json.Marshal
    // produces (verified against the real server binary's output).
    {
        RendezvousServerMessage msg;
        const bool ok = DecodeServerMessage(
            R"({"type":"session_created","code":"2K2SYJ","your_id":1})", msg);
        assert(ok);
        assert(msg.type == "session_created");
        assert(msg.code == "2K2SYJ");
        assert(msg.your_id == 1);
    }
    {
        RendezvousServerMessage msg;
        const bool ok = DecodeServerMessage(
            R"({"type":"peer_joined","peer_id":2,"peer_addr":"203.0.113.1:47273"})", msg);
        assert(ok);
        assert(msg.type == "peer_joined");
        assert(msg.peer_id == 2);
        assert(msg.peer_addr == "203.0.113.1:47273");
    }
    {
        RendezvousServerMessage msg;
        const bool ok = DecodeServerMessage(R"({"type":"peer_left","peer_id":2})", msg);
        assert(ok);
        assert(msg.type == "peer_left");
        assert(msg.peer_id == 2);
    }
    {
        RendezvousServerMessage msg;
        const bool ok = DecodeServerMessage(
            R"({"type":"relay","from_peer_id":1,"payload":"aGVsbG8td29ybGQ="})", msg);
        assert(ok);
        assert(msg.type == "relay");
        assert(msg.from_peer_id == 1);
        assert(msg.payload == "aGVsbG8td29ybGQ=");
    }
    {
        RendezvousServerMessage msg;
        const bool ok =
            DecodeServerMessage(R"({"type":"error","message":"no such session: NOPE99"})", msg);
        assert(ok);
        assert(msg.type == "error");
        assert(msg.message == "no such session: NOPE99");
    }
    {
        RendezvousServerMessage msg;
        assert(!DecodeServerMessage("", msg));
        assert(!DecodeServerMessage("not json at all", msg));
    }
    std::printf("TestDecodeServerMessageShapes: OK\n");
}

void TestSplitHostPort() {
    std::string host;
    uint16_t port = 0;

    assert(SplitHostPort("203.0.113.1:47273", host, port));
    assert(host == "203.0.113.1");
    assert(port == 47273);

    assert(SplitHostPort("[::1]:1234", host, port));
    assert(host == "::1");
    assert(port == 1234);

    assert(!SplitHostPort("", host, port));
    assert(!SplitHostPort("no-colon-here", host, port));
    assert(!SplitHostPort("host:", host, port));
    assert(!SplitHostPort("host:0", host, port));
    assert(!SplitHostPort("host:99999", host, port));

    std::printf("TestSplitHostPort: OK\n");
}

} // namespace

int main() {
    TestBase64RoundTripsArbitraryLengths();
    TestBase64KnownVector();
    TestEncodeClientMessageShapes();
    TestDecodeServerMessageShapes();
    TestSplitHostPort();
    std::printf("All rendezvous_protocol tests passed.\n");
    return 0;
}
