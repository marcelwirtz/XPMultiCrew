#pragma once

#include <cstddef>
#include <cstdint>

namespace flytogether {

// Fills `out[0..len)` with cryptographically-secure random bytes from the
// OS's own CSPRNG (getrandom()/`/dev/urandom` on Linux, BCryptGenRandom on
// Windows) - NOT std::random_device, whose quality the C++ standard
// explicitly does not guarantee (some implementations fall back to a
// deterministic PRNG). Used only for SessionCrypto's AEAD nonces
// (net/session_crypto.h) - the one place in this codebase where predictable
// "randomness" would be a real security failure (a repeated XChaCha20
// nonce under the same key breaks confidentiality), unlike e.g. this
// plugin's peer sender_id, which just needs to avoid accidental collisions,
// not resist a deliberate adversary.
//
// Aborts the process (via XPLMDebugString + std::abort) if the OS call
// itself fails - not something a caller should ever try to recover from or
// silently fall back past, since any fallback here would mean quietly
// downgrading to non-cryptographic randomness for a security-critical
// nonce.
void FillSecureRandom(uint8_t* out, size_t len);

} // namespace flytogether
