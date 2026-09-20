#include "net/secure_random.h"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
// Belt-and-suspenders alongside tests/CMakeLists.txt's/plugin/CMakeLists.txt's
// own compile-definition versions of these two (see either one's comment
// for the full explanation) - defined here too so this file is safe to
// compile on its own in any future target that forgets that flag, not
// just the ones that happen to set it today.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#else
#include <fstream>
#endif

namespace flytogether {

namespace {
[[noreturn]] void AbortWithMessage(const char* what) {
    std::fprintf(stderr, "XPMultiCrew: fatal: secure random generation failed (%s)\n", what);
    std::abort();
}
} // namespace

#if defined(_WIN32)

void FillSecureRandom(uint8_t* out, size_t len) {
    // BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG needs no
    // algorithm handle to open/close - the simplest correct way to reach
    // Windows' CSPRNG (CNG), and Microsoft's own recommended replacement
    // for the older, now-informally-deprecated RtlGenRandom/
    // CryptGenRandom.
    const NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0 /* STATUS_SUCCESS */) {
        AbortWithMessage("BCryptGenRandom");
    }
}

#else

void FillSecureRandom(uint8_t* out, size_t len) {
    // /dev/urandom rather than the getrandom() syscall directly: never
    // blocks (even very early at boot, unlike a strict getrandom() call
    // without GRND_INSECURE), and needs no syscall-number/glibc-version
    // plumbing - this plugin only ever generates nonces long after the
    // OS/game engine is fully up, so the "early boot, not enough entropy
    // yet" case getrandom() specifically guards against doesn't apply
    // here anyway. Linux, macOS and every other POSIX X-Plane target all
    // support this device identically.
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.is_open()) {
        AbortWithMessage("opening /dev/urandom");
    }
    urandom.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(len));
    if (!urandom) {
        AbortWithMessage("reading /dev/urandom");
    }
}

#endif

} // namespace flytogether
