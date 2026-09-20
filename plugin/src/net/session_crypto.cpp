#include "net/session_crypto.h"

#include "net/secure_random.h"

#include "monocypher.h"

#include <cstring>

namespace flytogether {

SessionCrypto::SessionCrypto(const std::string& code, const std::array<uint8_t, kSaltSize>& salt) {
    // Keyed BLAKE2b as the KDF: `code` is the key, `salt` is the message -
    // see this class's header comment for exactly what security property
    // that salt does (and doesn't) provide. Both sides derive the
    // identical key this way without either ever sending it over the
    // network.
    crypto_blake2b_keyed(key_.data(), key_.size(),
                          reinterpret_cast<const uint8_t*>(code.data()), code.size(), salt.data(),
                          salt.size());
}

std::vector<uint8_t> SessionCrypto::Seal(const uint8_t* plaintext, size_t len) const {
    std::vector<uint8_t> envelope(kEnvelopeOverhead + len);
    uint8_t* nonce = envelope.data();
    uint8_t* mac = envelope.data() + kNonceSize;
    uint8_t* cipher_text = envelope.data() + kNonceSize + kMacSize;

    FillSecureRandom(nonce, kNonceSize);
    crypto_aead_lock(cipher_text, mac, key_.data(), nonce, /*ad=*/nullptr, /*ad_size=*/0, plaintext, len);
    return envelope;
}

std::optional<std::vector<uint8_t>> SessionCrypto::Open(const uint8_t* envelope, size_t len) const {
    if (len < kEnvelopeOverhead) {
        return std::nullopt; // too short to even hold nonce+mac
    }
    const uint8_t* nonce = envelope;
    const uint8_t* mac = envelope + kNonceSize;
    const uint8_t* cipher_text = envelope + kNonceSize + kMacSize;
    const size_t cipher_len = len - kEnvelopeOverhead;

    std::vector<uint8_t> plaintext(cipher_len);
    if (crypto_aead_unlock(plaintext.data(), mac, key_.data(), nonce, /*ad=*/nullptr, /*ad_size=*/0,
                            cipher_text, cipher_len) != 0) {
        return std::nullopt; // authentication failed - wrong key or tampered/corrupted bytes
    }
    return plaintext;
}

} // namespace flytogether
