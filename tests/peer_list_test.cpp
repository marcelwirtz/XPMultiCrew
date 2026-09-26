// Pure-logic tests for formation/peer_list.h's file parsing and env-var
// path override. No XPLM/network dependency - real temp files, same
// "real I/O over mocks" approach as shared_cockpit_config_test.cpp.

#include "formation/peer_list.h"

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
    const fs::path tmp_dir = fs::temp_directory_path() / "xpmulticrew_peer_list_test";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);
    const fs::path old_cwd = fs::current_path();
    fs::current_path(tmp_dir);

    // 1. Missing file: empty list, not an error.
    assert(LoadPeerList("does_not_exist.txt").empty());
    std::printf("Missing file returns an empty list: OK\n");

    // 2. Blank lines and #comments are ignored; a trailing comment on a
    // PEER-bearing line is stripped before parsing.
    WriteFile("peers.txt",
              "# comment line\n"
              "\n"
              "127.0.0.1:49002\n"
              "   \n"
              "192.168.1.5:49003   # co-pilot's laptop\n");
    const auto peers = LoadPeerList("peers.txt");
    assert(peers.size() == 2);
    assert(peers[0].host == "127.0.0.1");
    assert(peers[0].port == 49002);
    assert(peers[1].host == "192.168.1.5");
    assert(peers[1].port == 49003);
    std::printf("Parses host:port lines, skipping blanks/comments: OK\n");

    // 3. Malformed lines (no colon, non-numeric or out-of-range port,
    // empty host) are silently skipped, not fatal to the rest of the file.
    WriteFile("malformed.txt",
              "no-colon-here\n"
              "127.0.0.1:not-a-number\n"
              "127.0.0.1:0\n"
              "127.0.0.1:99999\n"
              ":49002\n"
              "127.0.0.1:49002\n"); // the one valid line
    const auto malformed_peers = LoadPeerList("malformed.txt");
    assert(malformed_peers.size() == 1);
    assert(malformed_peers[0].host == "127.0.0.1");
    assert(malformed_peers[0].port == 49002);
    std::printf("Skips malformed lines without aborting the whole file: OK\n");

    // 4. ResolvePeerListPath: env var wins over the default when set.
    assert(ResolvePeerListPath("default.txt") == "default.txt");
    SetTestEnv("XPMULTICREW_PEERS_FILE", "override.txt");
    assert(ResolvePeerListPath("default.txt") == "override.txt");
    UnsetTestEnv("XPMULTICREW_PEERS_FILE");
    assert(ResolvePeerListPath("default.txt") == "default.txt");
    std::printf("ResolvePeerListPath: env var override wins, falls back otherwise: OK\n");

    // PORT line: overrides the LAN listen port and isn't mistaken for a peer.
    WriteFile("port.txt", "# comment\nPORT 49102\n192.168.1.5:49102\n");
    assert(LoadPeerListListenPort("port.txt", 49002) == 49102);
    assert(LoadPeerList("port.txt").size() == 1);
    WriteFile("badport.txt", "PORT 99999\nPORT abc\n");
    assert(LoadPeerListListenPort("badport.txt", 49002) == 49002);
    assert(LoadPeerListListenPort("missing.txt", 49002) == 49002);
    std::printf("LoadPeerListListenPort: PORT line honoured, invalid/missing falls back: OK\n");

    fs::current_path(old_cwd);
    fs::remove_all(tmp_dir);

    std::printf("\nALL PEER LIST CHECKS PASSED\n");
    return 0;
}
