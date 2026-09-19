#pragma once

#include "formation/peer_list.h"
#include "shared_cockpit/shared_cockpit_sync.h"

#include <string>
#include <vector>

namespace flytogether {

// File-based Shared Cockpit setup - same deliberately-manual stand-in for
// a real UI as peer_list.h/rendezvous_config.h (see docs/plan.md section 6
// and the GUI-trigger idea noted for Phase 4).
//
// Expected file contents, one directive per line (# comments, blank lines
// ignored):
//   ROLE MASTER
//   PEER <host>:<port>            # the other pilot(s); position broadcast
//                                  # (MASTER only) AND dataref sync (both
//                                  # roles - see DatarefSync) both use this
//   DATAREF <dataref/path>         # one line per "systems" dataref to keep
//                                  # in sync between both cockpits; same
//                                  # list on both sides, since Shared
//                                  # Cockpit assumes an identical aircraft
// or
//   ROLE CLIENT
//   PEER <host>:<port>            # the master, so this side's own switch
//                                  # changes have somewhere to go
//   DATAREF <dataref/path>
struct SharedCockpitConfig {
    bool enabled = false; // true only if ROLE was given
    SharedCockpitRole role = SharedCockpitRole::kNone;
    std::vector<Peer> peers; // who to send position (MASTER)/dataref changes (both) to
    std::vector<std::string> datarefs; // "systems" datarefs to sync, see DatarefSync
};

SharedCockpitConfig LoadSharedCockpitConfig(const std::string& path);

// Directory holding one DATAREF-only file per aircraft, named
// "<ICAO type>.txt" (e.g. "C172.txt") - see
// ResolveSharedCockpitConfigPath's comment. Exposed so callers/tests don't
// have to hardcode the path string themselves.
constexpr const char* kSharedCockpitProfilesDir = "XPMultiCrew_shared_cockpit_profiles";

// Resolves the config file path to load, in order:
//   1. $XPMULTICREW_SHARED_COCKPIT_FILE, if set - explicit override, wins
//      over everything else (e.g. for scripted/test setups).
//   2. kSharedCockpitProfilesDir/<icao_type>.txt, if icao_type isn't empty
//      and that file actually exists - lets each aircraft have its own
//      DATAREF list without manually swapping files between flights (the
//      plugin already knows the current aircraft's ICAO type at startup).
//   3. `default_path` - the original single flat file, kept as a fallback
//      for aircraft that don't have a profile yet.
// `icao_type` defaults to empty (skips step 2 entirely) so existing
// callers that don't have an ICAO type handy keep working unchanged.
std::string ResolveSharedCockpitConfigPath(const std::string& default_path,
                                            const std::string& icao_type = "");

} // namespace flytogether
