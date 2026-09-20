#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flytogether {

// Encrypts/authenticates Shared Cockpit and Formation payloads with a
// per-session key, so a message intercepted on the rendezvous server's
// relay path (or sniffed on an untrusted direct-UDP path, e.g. shared
// airport WiFi) can't be read or tampered with by anyone who doesn't
// already know the session code - see docs/plan.md's Phase 4 (Session-
// Auth/Verschlüsselung). Uses XChaCha20-Poly1305 (via the vendored
// Monocypher library, third_party/monocypher) for the AEAD itself.
//
// Key derivation: BLAKE2b_keyed(key=code, message=salt) - `salt` is a
// random value the rendezvous server mints once per session (see
// server/session.go) and hands out in the session_created response
// (rendezvous_protocol.h), alongside the human-typed session `code` both
// peers already know. Being honest about what this salt does and doesn't
// buy: it does NOT raise the session code's own ~31 bits of guessable
// entropy (the salt is sent in the clear, so an attacker who can already
// guess/brute-force the code learns the salt just as easily as either
// peer does) - guessing resistance is still purely server.go's per-IP
// rate limiting, same as before this feature. What the salt DOES buy is
// defeating precomputation: without it, an attacker could precompute a
// "code -> key" dictionary once and reuse it against every future session
// instantly; with a fresh salt every session, that dictionary is useless
// and the ~31-bit search has to be redone (and re-rate-limited) from
// scratch for each session. Never describe this to a user as account-
// level authentication - it isn't one.
//
// This class holds the already-derived key and does encryption/
// decryption only - the code+salt -> key derivation happens once, in the
// constructor. Pure logic, no networking/XPLM dependency - see
// tests/session_crypto_test.cpp.
class SessionCrypto {
public:
    static constexpr size_t kSaltSize = 16;
    static constexpr size_t kKeySize = 32;
    static constexpr size_t kNonceSize = 24;
    static constexpr size_t kMacSize = 16;
    // Every Seal()'d envelope is exactly this much larger than the
    // plaintext it wraps (nonce + mac, both fixed-size and prepended) -
    // exposed so a caller can size a receive buffer generously enough
    // (same "headroom above the known minimum" reasoning every other
    // wire format in this codebase already follows for forward
    // compatibility).
    static constexpr size_t kEnvelopeOverhead = kNonceSize + kMacSize;

    SessionCrypto(const std::string& code, const std::array<uint8_t, kSaltSize>& salt);

    // LAN-direct mode: no rendezvous server is involved, so there's no
    // salt-minting party either - the key is derived from `code` alone
    // (BLAKE2b_keyed(key=code, message=<fixed domain tag>), see .cpp). The
    // domain tag keeps this path's derivation distinct from the salted
    // constructor above (same code would otherwise never produce the same
    // key by coincidence, but this makes it structurally impossible rather
    // than incidental). Explicit trade-off, accepted for the LAN scenario:
    // without a per-session salt, an attacker CAN precompute a "code -> key"
    // dictionary once and reuse it against every future LAN session
    // instantly, rather than having to redo the ~31-bit search each time
    // (see the salted constructor's comment above for that contrast). Given
    // this is direct-LAN traffic - already a much smaller exposure window
    // than public-internet relay traffic - this is judged an acceptable
    // trade-off for the simplicity of not needing any out-of-band salt
    // exchange (e.g. via mDNS). Never describe this to a user as account-
    // level authentication - it isn't one, same caveat as above.
    explicit SessionCrypto(const std::string& code);

    // Encrypts+authenticates `plaintext`, returning a self-contained
    // envelope: nonce(24) || mac(16) || ciphertext(len(plaintext)). A
    // fresh, unpredictable nonce is drawn from the OS CSPRNG (see
    // net/secure_random.h) on every call - XChaCha20's 192-bit nonce
    // space makes random generation safe against reuse for the lifetime
    // of any realistic session (unlike ChaCha20's plain 96-bit nonce,
    // which would need a counter to stay safe at this message volume).
    std::vector<uint8_t> Seal(const uint8_t* plaintext, size_t len) const;
    std::vector<uint8_t> Seal(const std::vector<uint8_t>& plaintext) const {
        return Seal(plaintext.data(), plaintext.size());
    }

    // Verifies+decrypts an envelope produced by a peer's Seal() with the
    // SAME code+salt (i.e. the same session). std::nullopt for anything
    // that doesn't check out: too short to even hold nonce+mac, or a
    // failed authentication tag (wrong key - different/stale session - or
    // the bytes were corrupted/tampered with in transit). Never partially
    // decrypts: a failed Open() has no observable side effect a caller
    // could accidentally treat as partial plaintext.
    std::optional<std::vector<uint8_t>> Open(const uint8_t* envelope, size_t len) const;
    std::optional<std::vector<uint8_t>> Open(const std::vector<uint8_t>& envelope) const {
        return Open(envelope.data(), envelope.size());
    }

private:
    std::array<uint8_t, kKeySize> key_{};
};

} // namespace flytogether
