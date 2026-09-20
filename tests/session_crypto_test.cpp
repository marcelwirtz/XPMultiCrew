// Pure-logic regression test for SessionCrypto's key derivation and
// AEAD envelope round trip. No XPLM SDK or networking involved.

#include "net/session_crypto.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using flytogether::SessionCrypto;

namespace {

std::array<uint8_t, SessionCrypto::kSaltSize> MakeSalt(uint8_t fill) {
    std::array<uint8_t, SessionCrypto::kSaltSize> salt{};
    salt.fill(fill);
    return salt;
}

std::vector<uint8_t> ToBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

void TestSealOpenRoundTrip() {
    SessionCrypto crypto("ABC123", MakeSalt(0x11));
    const auto plaintext = ToBytes("hello shared cockpit");

    const auto envelope = crypto.Seal(plaintext);
    assert(envelope.size() == plaintext.size() + SessionCrypto::kEnvelopeOverhead);

    const auto opened = crypto.Open(envelope);
    assert(opened.has_value());
    assert(*opened == plaintext);
    std::printf("TestSealOpenRoundTrip: OK\n");
}

void TestEmptyPlaintextRoundTrips() {
    SessionCrypto crypto("ABC123", MakeSalt(0x11));
    const auto envelope = crypto.Seal(nullptr, 0);
    assert(envelope.size() == SessionCrypto::kEnvelopeOverhead);
    const auto opened = crypto.Open(envelope);
    assert(opened.has_value());
    assert(opened->empty());
    std::printf("TestEmptyPlaintextRoundTrips: OK\n");
}

void TestTwoSealsOfSameMessageProduceDifferentEnvelopes() {
    // Different random nonces each call - proof this isn't accidentally
    // deterministic (which would make nonce reuse an ever-present risk).
    SessionCrypto crypto("ABC123", MakeSalt(0x11));
    const auto plaintext = ToBytes("same message, twice");
    const auto envelope1 = crypto.Seal(plaintext);
    const auto envelope2 = crypto.Seal(plaintext);
    assert(envelope1 != envelope2);
    // Both still decrypt to the same plaintext.
    assert(*crypto.Open(envelope1) == plaintext);
    assert(*crypto.Open(envelope2) == plaintext);
    std::printf("TestTwoSealsOfSameMessageProduceDifferentEnvelopes: OK\n");
}

void TestSameCodeAndSaltDeriveTheSameKey() {
    // Two independently-constructed instances with identical inputs (the
    // real-world "both peers derive the key locally" scenario) must be
    // interoperable - one seals, the other opens.
    SessionCrypto sender("XYZ789", MakeSalt(0x42));
    SessionCrypto receiver("XYZ789", MakeSalt(0x42));
    const auto plaintext = ToBytes("cross-instance message");
    const auto opened = receiver.Open(sender.Seal(plaintext));
    assert(opened.has_value());
    assert(*opened == plaintext);
    std::printf("TestSameCodeAndSaltDeriveTheSameKey: OK\n");
}

void TestDifferentCodeCannotDecrypt() {
    SessionCrypto sender("ABC123", MakeSalt(0x11));
    SessionCrypto eavesdropper("WRONG1", MakeSalt(0x11)); // right salt, wrong code
    const auto envelope = sender.Seal(ToBytes("secret"));
    assert(!eavesdropper.Open(envelope).has_value());
    std::printf("TestDifferentCodeCannotDecrypt: OK\n");
}

void TestDifferentSaltCannotDecrypt() {
    SessionCrypto sender("ABC123", MakeSalt(0x11));
    SessionCrypto other_session("ABC123", MakeSalt(0x22)); // right code, different session's salt
    const auto envelope = sender.Seal(ToBytes("secret"));
    assert(!other_session.Open(envelope).has_value());
    std::printf("TestDifferentSaltCannotDecrypt: OK\n");
}

void TestTamperedCiphertextIsRejected() {
    SessionCrypto crypto("ABC123", MakeSalt(0x11));
    auto envelope = crypto.Seal(ToBytes("do not modify me"));
    envelope.back() ^= 0xFF; // flip a bit in the ciphertext
    assert(!crypto.Open(envelope).has_value());
    std::printf("TestTamperedCiphertextIsRejected: OK\n");
}

void TestTamperedMacIsRejected() {
    SessionCrypto crypto("ABC123", MakeSalt(0x11));
    auto envelope = crypto.Seal(ToBytes("do not modify me either"));
    envelope[SessionCrypto::kNonceSize] ^= 0xFF; // flip a bit in the mac
    assert(!crypto.Open(envelope).has_value());
    std::printf("TestTamperedMacIsRejected: OK\n");
}

void TestTruncatedEnvelopeIsRejected() {
    SessionCrypto crypto("ABC123", MakeSalt(0x11));
    const auto envelope = crypto.Seal(ToBytes("normal message"));
    const std::vector<uint8_t> truncated(envelope.begin(), envelope.begin() + SessionCrypto::kEnvelopeOverhead - 1);
    assert(!crypto.Open(truncated).has_value());
    assert(!crypto.Open(nullptr, 0).has_value());
    std::printf("TestTruncatedEnvelopeIsRejected: OK\n");
}

} // namespace

int main() {
    TestSealOpenRoundTrip();
    TestEmptyPlaintextRoundTrips();
    TestTwoSealsOfSameMessageProduceDifferentEnvelopes();
    TestSameCodeAndSaltDeriveTheSameKey();
    TestDifferentCodeCannotDecrypt();
    TestDifferentSaltCannotDecrypt();
    TestTamperedCiphertextIsRejected();
    TestTamperedMacIsRejected();
    TestTruncatedEnvelopeIsRejected();
    std::printf("All session_crypto tests passed.\n");
    return 0;
}
