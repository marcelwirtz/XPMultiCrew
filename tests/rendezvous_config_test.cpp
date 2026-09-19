// Pure-logic tests for formation/rendezvous_config.h's file parsing and
// env-var path override - the same file format/precedence pattern as
// shared_cockpit_config_test.cpp, for the sibling CREATE/JOIN config.

#include "formation/rendezvous_config.h"

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

void SetTestEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, /*overwrite=*/1);
#endif
}

void UnsetTestEnv(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

int main() {
    const fs::path tmp_dir = fs::temp_directory_path() / "xpmulticrew_rendezvous_config_test";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);
    const fs::path old_cwd = fs::current_path();
    fs::current_path(tmp_dir);

    // 1. Missing file: disabled config, not an error.
    {
        const RendezvousConfig config = LoadRendezvousConfig("does_not_exist.txt");
        assert(!config.enabled);
    }
    std::printf("Missing file returns a disabled config: OK\n");

    // 2. SERVER + CREATE enables the config for creating a session.
    WriteFile("create.txt",
              "# comment\n"
              "SERVER example.com:45000\n"
              "CREATE\n");
    {
        const RendezvousConfig config = LoadRendezvousConfig("create.txt");
        assert(config.enabled);
        assert(config.server_host == "example.com");
        assert(config.server_port == 45000);
        assert(config.create_session);
        assert(config.join_code.empty());
    }
    std::printf("SERVER + CREATE parses correctly: OK\n");

    // 3. SERVER + JOIN <code> enables the config for joining instead.
    WriteFile("join.txt",
              "SERVER 203.0.113.7:45000\n"
              "JOIN ABC123   # trailing comment stripped\n");
    {
        const RendezvousConfig config = LoadRendezvousConfig("join.txt");
        assert(config.enabled);
        assert(config.server_host == "203.0.113.7");
        assert(config.server_port == 45000);
        assert(!config.create_session);
        assert(config.join_code == "ABC123");
    }
    std::printf("SERVER + JOIN parses correctly, comment stripped: OK\n");

    // 4. SERVER alone (neither CREATE nor JOIN) stays disabled - there's
    // nothing to act on.
    WriteFile("server_only.txt", "SERVER example.com:45000\n");
    {
        const RendezvousConfig config = LoadRendezvousConfig("server_only.txt");
        assert(!config.enabled);
    }
    std::printf("SERVER alone (no CREATE/JOIN) stays disabled: OK\n");

    // 5. CREATE/JOIN without a SERVER line also stays disabled - nowhere
    // to connect to.
    WriteFile("no_server.txt", "CREATE\n");
    {
        const RendezvousConfig config = LoadRendezvousConfig("no_server.txt");
        assert(!config.enabled);
    }
    std::printf("CREATE without SERVER stays disabled: OK\n");

    // 6. A malformed SERVER line (no port) is silently ignored, same as
    // SplitHostPort's own contract.
    WriteFile("bad_server.txt", "SERVER no-port-here\nCREATE\n");
    {
        const RendezvousConfig config = LoadRendezvousConfig("bad_server.txt");
        assert(!config.enabled); // have_server never got set
    }
    std::printf("Malformed SERVER line is ignored, leaving the config disabled: OK\n");

    // 7. ResolveRendezvousConfigPath: env var wins over the default.
    assert(ResolveRendezvousConfigPath("default.txt") == "default.txt");
    SetTestEnv("XPMULTICREW_RENDEZVOUS_FILE", "override.txt");
    assert(ResolveRendezvousConfigPath("default.txt") == "override.txt");
    UnsetTestEnv("XPMULTICREW_RENDEZVOUS_FILE");
    assert(ResolveRendezvousConfigPath("default.txt") == "default.txt");
    std::printf("ResolveRendezvousConfigPath: env var override wins, falls back otherwise: OK\n");

    fs::current_path(old_cwd);
    fs::remove_all(tmp_dir);

    std::printf("\nALL RENDEZVOUS CONFIG CHECKS PASSED\n");
    return 0;
}
