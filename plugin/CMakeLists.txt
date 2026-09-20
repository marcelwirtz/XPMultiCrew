cmake_minimum_required(VERSION 3.16)
project(XPMultiCrewPlugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Version ------------------------------------------------------------
# Derived from git so it always matches the actual release tag (see
# .github/workflows/release.yml) instead of a manually-bumped number that
# can drift - shown in the companion app's Setup panel (installed vs.
# available) and pushed live over the control listener as PLUGIN_VERSION.
# "unknown" if this isn't a git checkout at all (e.g. a source tarball).
find_package(Git QUIET)
set(XPMULTICREW_VERSION "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_DESCRIBE_RESULT
    )
    if(GIT_DESCRIBE_RESULT EQUAL 0 AND GIT_DESCRIBE_OUTPUT)
        set(XPMULTICREW_VERSION "${GIT_DESCRIBE_OUTPUT}")
    endif()
endif()
message(STATUS "XPMultiCrew version: ${XPMULTICREW_VERSION}")
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${XPMULTICREW_VERSION}")

# --- X-Plane SDK ------------------------------------------------------------
# Not redistributed here (Laminar Research license). Download it from
# https://developer.x-plane.com/sdk/plugin-sdk-downloads/ and either extract
# it to third_party/XPSDK (so third_party/XPSDK/CHeaders/... exists) or pass
# -DXPSDK_ROOT=/path/to/SDK to CMake.
if(NOT DEFINED XPSDK_ROOT)
    set(XPSDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XPSDK" CACHE PATH
        "Path to the extracted X-Plane SDK (folder containing CHeaders/ and Libraries/)")
endif()

if(NOT EXISTS "${XPSDK_ROOT}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR
        "X-Plane SDK not found at '${XPSDK_ROOT}'.\n"
        "Download it from https://developer.x-plane.com/sdk/plugin-sdk-downloads/ and either:\n"
        "  - extract it so that '${XPSDK_ROOT}/CHeaders/XPLM/XPLMDefs.h' exists, or\n"
        "  - reconfigure with -DXPSDK_ROOT=/path/to/extracted/SDK")
endif()

# --- XPMP2 (TCAS override + XPLMInstance + CSL model handling) --------------
# MIT licensed, https://github.com/TwinFan/XPMP2. Not committed here (it's a
# multi-MB third-party checkout); fetch it with:
#   git clone https://github.com/TwinFan/XPMP2.git third_party/XPMP2
if(NOT DEFINED XPMP2_ROOT)
    set(XPMP2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XPMP2" CACHE PATH
        "Path to a checkout of https://github.com/TwinFan/XPMP2")
endif()

if(NOT EXISTS "${XPMP2_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "XPMP2 not found at '${XPMP2_ROOT}'.\n"
        "Fetch it with: git clone https://github.com/TwinFan/XPMP2.git '${XPMP2_ROOT}'\n"
        "or reconfigure with -DXPMP2_ROOT=/path/to/XPMP2/checkout")
endif()

set(XPlaneSDK_DIR "${XPSDK_ROOT}" CACHE PATH "" FORCE)
add_subdirectory("${XPMP2_ROOT}" "${CMAKE_BINARY_DIR}/XPMP2")

# --- Monocypher (XChaCha20-Poly1305 AEAD + BLAKE2b, for net/session_crypto) --
# Public-domain/CC0 (dual BSD-2-Clause), https://github.com/LoupVaillant/Monocypher.
# Vendored (2 files: monocypher.c/.h) rather than fetched at configure time -
# same reasoning as XPMP2 being a full checkout instead: a security-relevant
# dependency's exact, reviewed source should be committed, not re-downloaded
# on every fresh clone.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher/monocypher.h")
    message(FATAL_ERROR
        "Monocypher not found at third_party/monocypher.\n"
        "This should be committed to the repo - if it's missing, your checkout is incomplete.")
endif()

# --- Target ------------------------------------------------------------------
add_library(XPMultiCrew SHARED
    src/core/plugin_main.cpp
    src/sync/remote_aircraft.cpp
    src/net/link_quality.cpp
    src/net/secure_random.cpp
    src/net/session_crypto.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher/monocypher.c"
    src/formation/peer_list.cpp
    src/formation/formation_sync.cpp
    src/formation/csl_aircraft.cpp
    src/formation/rendezvous_protocol.cpp
    src/formation/rendezvous_client.cpp
    src/shared_cockpit/quaternion.cpp
    src/shared_cockpit/shared_cockpit_sync.cpp
    src/shared_cockpit/shared_cockpit_config.cpp
    src/shared_cockpit/dataref_sync_protocol.cpp
    src/shared_cockpit/dataref_sync.cpp
    src/shared_cockpit/ownership_tracker.cpp
    src/shared_cockpit/weather_sync.cpp
    src/control/control_listener.cpp
)

target_include_directories(XPMultiCrew PRIVATE
    "${XPSDK_ROOT}/CHeaders/XPLM"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher"
)

if(WIN32)
    # net/secure_random.cpp's Windows path (BCryptGenRandom).
    target_link_libraries(XPMultiCrew PRIVATE bcrypt)
endif()

# XPLM API revision and IBM/APL/LIN platform macros come from XPMP2's PUBLIC
# compile definitions (see third_party/XPMP2/CMakeLists.txt) via the
# target_link_libraries(... XPMP2) call below - not redefined here to avoid
# clashing with them.
target_link_libraries(XPMultiCrew PRIVATE XPMP2)

set_target_properties(XPMultiCrew PROPERTIES PREFIX "")

if(WIN32)
    set_target_properties(XPMultiCrew PROPERTIES OUTPUT_NAME "win" SUFFIX ".xpl")

    # dataref_sync.h includes XPLMDataAccess.h (which pulls in <windows.h>,
    # and thus the legacy <winsock.h>) before it includes our own
    # udp_socket.h (which needs <winsock2.h>) - windows.h only skips
    # <winsock.h> if WIN32_LEAN_AND_MEAN is already defined by then. A
    # per-header #define in udp_socket.h can't fix this: by the time that
    # file is reached, XPLMDataAccess.h has already been processed. Define
    # it as a compile flag instead so it's set before the preprocessor
    # opens *any* header, regardless of a given .cpp's include order.
    # NOMINMAX for the same reason: windows.h's own max/min macros mangle
    # dataref_sync.cpp's std::min(...) calls into invalid syntax otherwise
    # (MSVC error C2589).
    target_compile_definitions(XPMultiCrew PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

    set(XPSDK_WIN_LIB_DIR "${XPSDK_ROOT}/Libraries/Win")
    target_link_libraries(XPMultiCrew PRIVATE
        "${XPSDK_WIN_LIB_DIR}/XPLM_64.lib"
        ws2_32
        # XPMP2's Network.cpp needs GetAdaptersAddresses/if_indextoname
        # (IP Helper API). XPMP2 links iphlpapi itself, but only PRIVATE,
        # and as a plain (non-imported-target) library name that doesn't
        # reliably propagate to our final link step - link it directly too.
        iphlpapi
    )
elseif(APPLE)
    set_target_properties(XPMultiCrew PROPERTIES OUTPUT_NAME "mac" SUFFIX ".xpl")
    target_link_options(XPMultiCrew PRIVATE -undefined dynamic_lookup)
else()
    set_target_properties(XPMultiCrew PROPERTIES OUTPUT_NAME "lin" SUFFIX ".xpl")
    # XPLM symbols are resolved at load time by the X-Plane process itself;
    # do not link against an import library and do not pass --no-undefined.
endif()

target_compile_options(XPMultiCrew PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

target_compile_definitions(XPMultiCrew PRIVATE XPMULTICREW_VERSION="${XPMULTICREW_VERSION}")

# Needed for XPLMWeather.h's XPLMGetWeatherAtLocation/XPLMSetWeatherAtLocation
# (Shared Cockpit weather sync, shared_cockpit/weather_sync.h) - requires
# X-Plane 12.1+. XPMP2's own CMakeLists (third_party/XPMP2) already defines
# XPLM200/210/300/301/303/400 as PUBLIC, which propagate here via linking
# against it; XPLM420 isn't in that list, so it's set explicitly for just
# this target rather than patching the vendored XPMP2 build. This raises
# the plugin's minimum X-Plane version to 12.1 - not just for weather sync,
# since it's one binary - confirmed to still load correctly against a real
# X-Plane 12 install before this shipped (see the version bump this
# accompanies).
target_compile_definitions(XPMultiCrew PRIVATE XPLM420=1)

# Place the built .xpl in a layout matching X-Plane's plugin directory
# (<X-Plane>/Resources/plugins/XPMultiCrew/<lin|mac|win>.xpl) for easy testing.
set_target_properties(XPMultiCrew PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/XPMultiCrew"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/XPMultiCrew"
)

# Copy the bundled Resources (XPMP2's own support files plus our generic CSL
# model, see plugin/Resources) next to the built .xpl, so the whole
# build/XPMultiCrew folder can be dropped straight into
# <X-Plane>/Resources/plugins/.
add_custom_command(TARGET XPMultiCrew POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources"
            "$<TARGET_FILE_DIR:XPMultiCrew>/Resources"
)

# Also drop version.txt next to the .xpl - the companion app reads this
# straight off disk (via the chosen X-Plane path) to show "installed
# version" without needing X-Plane to actually be running, and its own
# embedded copy (staged the same way, see companion/plugin_install.go) to
# show "available version" - see .github/workflows/{ci,release}.yml's
# "Stage plugin for embedding" step.
add_custom_command(TARGET XPMultiCrew POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/version.txt"
            "$<TARGET_FILE_DIR:XPMultiCrew>/version.txt"
)
