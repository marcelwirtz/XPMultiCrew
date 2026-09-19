// Verifies ResolveSharedCockpitConfigPath's precedence (env override >
// per-aircraft profile file > flat fallback file) against a real temp
// directory - same "real I/O over mocks" approach as the rest of this
// test suite.

#include "shared_cockpit/shared_cockpit_config.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace flytogether;
namespace fs = std::filesystem;

namespace {

void WriteFile(const std::string& path, const std::string& contents) {
    std::ofstream f(path);
    f << contents;
}

// setenv/unsetenv are POSIX-only - MSVC doesn't have them at all (its
// closest equivalents are _putenv_s/_dupenv_s), so this test can't call
// them directly on Windows without failing to link.
void SetTestEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, /*overwrite=*/1);
#endif
}

void UnsetTestEnv(const char* name) {
#if defined(_WIN32)
    // Per _putenv_s's documented behavior, an empty value removes the
    // variable entirely (unlike POSIX setenv, which would just set it to
    // an empty string) - this is the Windows CRT's actual unset.
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

int main() {
    const fs::path tmp_dir = fs::temp_directory_path() / "xpmulticrew_shared_cockpit_config_test";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);
    const fs::path old_cwd = fs::current_path();
    fs::current_path(tmp_dir);

    const std::string flat_path = "XPMultiCrew_shared_cockpit.txt";
    const std::string profiles_dir = std::string(kSharedCockpitProfilesDir);

    // 1. No profile, no env var, no icao_type: falls back to the flat path
    // even though nothing exists yet - just resolving a path, not loading.
    assert(ResolveSharedCockpitConfigPath(flat_path) == flat_path);
    assert(ResolveSharedCockpitConfigPath(flat_path, "C172") == flat_path);
    std::printf("Falls back to flat path when no profile exists: OK\n");

    // 2. A profile file for this ICAO type exists: preferred over the flat
    // fallback.
    fs::create_directories(profiles_dir);
    WriteFile(profiles_dir + "/C172.txt", "DATAREF sim/flightmodel/controls/parkbrake\n");
    const std::string resolved = ResolveSharedCockpitConfigPath(flat_path, "C172");
    assert(resolved == profiles_dir + "/C172.txt");
    std::printf("Prefers per-aircraft profile when it exists: %s\n", resolved.c_str());

    // 3. A different ICAO type with no profile file still falls back.
    assert(ResolveSharedCockpitConfigPath(flat_path, "B738") == flat_path);
    std::printf("Falls back for an aircraft without a profile: OK\n");

    // 4. The env var always wins, even with a matching profile present.
    SetTestEnv("XPMULTICREW_SHARED_COCKPIT_FILE", "explicit_override.txt");
    assert(ResolveSharedCockpitConfigPath(flat_path, "C172") == "explicit_override.txt");
    UnsetTestEnv("XPMULTICREW_SHARED_COCKPIT_FILE");
    std::printf("Env var override wins over the profile: OK\n");

    // 5. The resolved profile path actually loads correctly.
    const SharedCockpitConfig config = LoadSharedCockpitConfig(resolved);
    assert(config.datarefs.size() == 1);
    assert(config.datarefs[0] == "sim/flightmodel/controls/parkbrake");
    std::printf("Resolved profile path loads correctly\n");

    fs::current_path(old_cwd);
    fs::remove_all(tmp_dir);

    std::printf("\nALL SHARED COCKPIT CONFIG CHECKS PASSED\n");
    return 0;
}
