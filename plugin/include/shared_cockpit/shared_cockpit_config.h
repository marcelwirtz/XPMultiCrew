#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flytogether {

// Which "who's allowed to touch this" bucket a watched dataref falls into -
// see dataref_sync.h's ownership enforcement and the CATEGORY grammar
// below. A small, fixed set rather than per-dataref ownership: coarse
// enough for a companion-app UI to offer as a couple of buttons, matching
// the profile file's pre-existing comment groupings (Engine/fuel; Radios,
// autopilot bugs, OBS/audio -> Avionics; everything else -> Systems).
enum class DatarefCategory : uint8_t {
    kSystems = 0, // default - electrical, lights, trim, flaps, parking brake
    kEngine = 1,  // throttle, mixture, ignition, fuel pump/selector, primer
    kAvionics = 2, // radios, transponder, OBS, audio panel, autopilot bugs
};
constexpr int kDatarefCategoryCount = 3;

// Parses a category name as spelled in the DATAREF grammar's CATEGORY
// token ("engine", "avionics", "systems") - also reused by
// control/control_listener.h's CLAIM_OWNERSHIP command, so the two
// stay spelled the same way. False (and `out` left untouched) for
// anything else.
bool ParseDatarefCategoryName(const std::string& name, DatarefCategory& out);

// Inverse of ParseDatarefCategoryName - used for the SHARED_COCKPIT_OWNERSHIP
// status line control/control_listener.h pushes to the companion app.
std::string DatarefCategoryName(DatarefCategory category);

// One "systems" dataref DatarefSync should watch, plus how it should be
// synced - see the DATAREF line grammar below, DatarefSync::Poll()'s use
// of `stream`, and its ownership enforcement's use of `category`.
struct DatarefSyncSpec {
    std::string name;
    // false (default, "CHANGE" in the profile file): only broadcast when
    // the value differs from what was last sent, like every other watched
    // dataref.
    // true ("STREAM" in the profile file): broadcast every Poll() tick
    // regardless of change, same as AircraftPosition already is. Needed
    // for a handful of X-Plane datarefs that are documented as
    // "spring-loaded" - e.g. sim/cockpit2/engine/actuators/ignition_key's
    // own doc string: "You have to keep setting it to start in order to
    // keep cranking." Change-detection only broadcasts the single 3->4
    // transition, which isn't enough to keep a starter cranking on the
    // peer that receives it - see plugin/Resources/shared_cockpit_profiles/
    // C172.txt for the concrete example this was added for.
    bool stream = false;
    // See DatarefCategory above. Defaults to kSystems (the "CATEGORY"
    // token's own default when omitted from a DATAREF line).
    DatarefCategory category = DatarefCategory::kSystems;
};

// File-based "systems" DATAREF list for Shared Cockpit - see
// ResolveSharedCockpitConfigPath below for how the right file is picked
// per aircraft. ROLE/server/session-code/peer discovery always come from
// the companion app (which goes through the rendezvous server, see
// formation/rendezvous_client.h) - these files never set them; the
// companion app is the only way to start Shared Cockpit.
//
// Expected file contents, one directive per line (# comments, blank lines
// ignored):
//   DATAREF <dataref/path> [STREAM] [CATEGORY <engine|avionics|systems>]
//     One line per "systems" dataref to keep in sync between both
//     cockpits; same list on both sides, since Shared Cockpit assumes an
//     identical aircraft. STREAM and CATEGORY are both optional and may
//     appear in either order after the dataref path; omitting either
//     keeps today's defaults (CHANGE / systems), so every profile file
//     written before these were added still parses unchanged. See
//     DatarefSyncSpec::stream/::category above for what each one does.
struct SharedCockpitConfig {
    std::vector<DatarefSyncSpec> datarefs; // "systems" datarefs to sync, see DatarefSync
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
