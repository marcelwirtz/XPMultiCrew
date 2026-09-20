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
#include <set>
#include <sstream>
#include <string>

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

// Re-parses a bundled profile file completely independently of
// LoadSharedCockpitConfig, checking things that parser deliberately lets
// slide (see its own "forward compatible, don't hard-fail on the unknown"
// comment) but that are still real authoring mistakes in a file nobody
// else is round-tripping through a real X-Plane session yet: an
// unrecognized CATEGORY name (silently falls back to the "systems"
// default rather than erroring) and a stray line that isn't blank, a "#"
// comment, or a DATAREF line at all (silently ignored the same way ROLE/
// PEER already are). `expected_dataref_count` is a hardcoded literal per
// file precisely so a keyword typo (e.g. "DATAERF") that would make
// LoadSharedCockpitConfig silently skip a line - and therefore parse a
// too-small count without any other symptom - still fails this test.
void ValidateBundledProfileFile(const std::string& path, size_t expected_dataref_count) {
    std::ifstream file(path);
    assert(file.is_open());

    std::set<std::string> seen_names;
    size_t dataref_lines = 0;
    std::string line;
    while (std::getline(file, line)) {
        // Trim trailing \r the same way the real parser's TrimConfigLine
        // does, so this doesn't false-positive on a CRLF-saved file.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            continue; // blank/whitespace-only line
        }
        if (line[start] == '#') {
            continue; // comment
        }
        std::istringstream tokens(line.substr(start));
        std::string keyword;
        tokens >> keyword;
        if (keyword != "DATAREF") {
            std::printf("  UNEXPECTED non-DATAREF, non-comment line in %s: '%s'\n", path.c_str(),
                        line.c_str());
            assert(false);
        }
        ++dataref_lines;

        std::string name, token;
        tokens >> name;
        assert(!name.empty());
        assert(seen_names.insert(name).second && "duplicate DATAREF line (copy-paste mistake?)");

        while (tokens >> token) {
            if (token == "STREAM") {
                continue;
            }
            if (token == "CATEGORY") {
                std::string category_name;
                tokens >> category_name;
                if (category_name != "engine" && category_name != "avionics" &&
                    category_name != "systems") {
                    std::printf("  UNRECOGNIZED CATEGORY '%s' for %s in %s (silently defaults to "
                                "'systems' - is this a typo?)\n",
                                category_name.c_str(), name.c_str(), path.c_str());
                    assert(false);
                }
                continue;
            }
            std::printf("  UNRECOGNIZED token '%s' for %s in %s\n", token.c_str(), name.c_str(),
                        path.c_str());
            assert(false);
        }
    }
    assert(dataref_lines == expected_dataref_count);

    // Also confirm the real parser agrees on the count - catches a
    // divergence between this test's independent re-parse above and
    // LoadSharedCockpitConfig's own logic, rather than just trusting them
    // to always agree.
    const SharedCockpitConfig config = LoadSharedCockpitConfig(path);
    assert(config.datarefs.size() == expected_dataref_count);

    std::printf("  %s: %zu datarefs, all well-formed - OK\n", path.c_str(), dataref_lines);
}

void TestBundledProfilesParseCleanly() {
#ifndef SHARED_COCKPIT_PROFILES_DIR
#error "SHARED_COCKPIT_PROFILES_DIR must be set by tests/CMakeLists.txt"
#endif
    const std::string dir = SHARED_COCKPIT_PROFILES_DIR;
    // Counts are literal `grep -c '^DATAREF '` results at authoring time,
    // not derived from the files themselves - see this function's comment
    // for why that's the point.
    ValidateBundledProfileFile(dir + "/C172.txt", 43);
    ValidateBundledProfileFile(dir + "/BE58.txt", 47);
    ValidateBundledProfileFile(dir + "/BE9L.txt", 45);
    std::printf("TestBundledProfilesParseCleanly: OK\n");
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
    assert(config.datarefs[0].name == "sim/flightmodel/controls/parkbrake");
    assert(config.datarefs[0].stream == false);
    std::printf("Resolved profile path loads correctly\n");

    // 5b. An optional trailing STREAM token opts a dataref out of
    // change-detection; a line without it (including every case above)
    // keeps defaulting to false, so old profile files parse unchanged.
    const std::string stream_path = "stream_test.txt";
    WriteFile(stream_path,
              "DATAREF sim/cockpit2/engine/actuators/ignition_key STREAM\n"
              "DATAREF sim/cockpit2/engine/actuators/mixture_ratio_all\n");
    const SharedCockpitConfig stream_config = LoadSharedCockpitConfig(stream_path);
    assert(stream_config.datarefs.size() == 2);
    assert(stream_config.datarefs[0].name == "sim/cockpit2/engine/actuators/ignition_key");
    assert(stream_config.datarefs[0].stream == true);
    assert(stream_config.datarefs[0].category == DatarefCategory::kSystems); // still defaults
    assert(stream_config.datarefs[1].name == "sim/cockpit2/engine/actuators/mixture_ratio_all");
    assert(stream_config.datarefs[1].stream == false);
    std::printf("Trailing STREAM token is parsed, and is opt-in per line: OK\n");

    // 5c. CATEGORY works in either order relative to STREAM, an unknown
    // category name falls back to the kSystems default rather than
    // failing the whole line, and a line with neither modifier still
    // defaults exactly as before.
    const std::string category_path = "category_test.txt";
    WriteFile(category_path,
              "DATAREF sim/cockpit2/engine/actuators/throttle_ratio_all CATEGORY engine\n"
              "DATAREF sim/cockpit2/engine/actuators/ignition_key STREAM CATEGORY engine\n"
              "DATAREF sim/cockpit2/radios/actuators/com1_frequency_hz CATEGORY avionics STREAM\n"
              "DATAREF sim/cockpit/electrical/beacon_lights_on CATEGORY bogus\n"
              "DATAREF sim/flightmodel/controls/parkbrake\n");
    const SharedCockpitConfig category_config = LoadSharedCockpitConfig(category_path);
    assert(category_config.datarefs.size() == 5);
    assert(category_config.datarefs[0].category == DatarefCategory::kEngine);
    assert(category_config.datarefs[0].stream == false);
    assert(category_config.datarefs[1].category == DatarefCategory::kEngine);
    assert(category_config.datarefs[1].stream == true);
    assert(category_config.datarefs[2].category == DatarefCategory::kAvionics);
    assert(category_config.datarefs[2].stream == true); // CATEGORY before STREAM also works
    assert(category_config.datarefs[3].category == DatarefCategory::kSystems); // unknown name -> default
    assert(category_config.datarefs[4].category == DatarefCategory::kSystems); // no modifiers -> default
    std::printf("CATEGORY token is parsed in either order, unknown names fall back: OK\n");

    // 6. A plugin-bundled profile (3rd parameter) is used for an aircraft
    // with no user override, instead of falling all the way back to the
    // flat path.
    const std::string bundled_resources_dir = "bundled_resources";
    const std::string bundled_profiles_dir =
        bundled_resources_dir + "/" + std::string(kSharedCockpitProfilesSubdir);
    fs::create_directories(bundled_profiles_dir);
    WriteFile(bundled_profiles_dir + "/B738.txt", "DATAREF sim/cockpit/electrical/battery_on\n");
    const std::string bundled_resolved =
        ResolveSharedCockpitConfigPath(flat_path, "B738", bundled_resources_dir);
    assert(bundled_resolved == bundled_profiles_dir + "/B738.txt");
    std::printf("Falls back to the plugin-bundled profile when no user override exists: %s\n",
                 bundled_resolved.c_str());

    // 7. ...but a user override at the X-Plane root still wins over the
    // plugin-bundled default, even when a plugin_resources_path is given.
    WriteFile(profiles_dir + "/B738.txt", "DATAREF sim/flightmodel/controls/parkbrake\n");
    assert(ResolveSharedCockpitConfigPath(flat_path, "B738", bundled_resources_dir) ==
           profiles_dir + "/B738.txt");
    std::printf("User override still wins over the plugin-bundled profile: OK\n");

    // 8. With neither a user override nor a bundled profile, and a
    // plugin_resources_path given, this still falls all the way back to
    // the flat path (bundled dir present, but no file for this ICAO type).
    assert(ResolveSharedCockpitConfigPath(flat_path, "A320", bundled_resources_dir) == flat_path);
    std::printf("Falls back to flat path when neither override nor bundled profile exists: OK\n");

    fs::current_path(old_cwd);
    fs::remove_all(tmp_dir);

    TestBundledProfilesParseCleanly();

    std::printf("\nALL SHARED COCKPIT CONFIG CHECKS PASSED\n");
    return 0;
}
