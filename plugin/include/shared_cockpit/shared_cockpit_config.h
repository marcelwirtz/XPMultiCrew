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
// "<ICAO type>.txt" (e.g. "C172.txt"), checked next to X-Plane itself -
// see ResolveSharedCockpitConfigPath's comment. Exposed so callers/tests
// don't have to hardcode the path string themselves.
constexpr const char* kSharedCockpitProfilesDir = "XPMultiCrew_shared_cockpit_profiles";

// Filename (under a plugin-provided directory - see below) of a
// per-aircraft profile bundled *with the plugin itself*, e.g.
// plugin/Resources/shared_cockpit_profiles/C172.txt, so it comes
// pre-populated on every install (companion app's Install/Update Plugin,
// or a manual copy) instead of every user having to figure out and
// maintain their own DATAREF list from scratch.
constexpr const char* kSharedCockpitProfilesSubdir = "shared_cockpit_profiles";

// Resolves the config file path to load, in order:
//   1. $XPMULTICREW_SHARED_COCKPIT_FILE, if set - explicit override, wins
//      over everything else (e.g. for scripted/test setups).
//   2. kSharedCockpitProfilesDir/<icao_type>.txt, next to X-Plane, if
//      icao_type isn't empty and that file exists - a user's own
//      customization/override, checked before the bundled default so it
//      always wins if present.
//   3. <plugin_resources_path>/kSharedCockpitProfilesSubdir/<icao_type>.txt,
//      if plugin_resources_path isn't empty and that file exists - the
//      profile bundled with and auto-installed alongside the plugin
//      itself (see plugin/Resources/shared_cockpit_profiles/), so a
//      correct DATAREF list for common aircraft (currently just the
//      C172) works with zero manual setup; only one person ever has to
//      figure one out and everyone installing the plugin benefits.
//   4. `default_path` - the original single flat file, kept as a fallback
//      for aircraft that don't have a profile anywhere above.
// `icao_type`/`plugin_resources_path` default to empty (skipping steps 2
// and/or 3) so existing callers without that context keep working
// unchanged.
std::string ResolveSharedCockpitConfigPath(const std::string& default_path,
                                            const std::string& icao_type = "",
                                            const std::string& plugin_resources_path = "");

} // namespace flytogether
