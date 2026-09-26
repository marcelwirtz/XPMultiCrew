cmake_minimum_required(VERSION 3.16)

# Every test in this directory verifies behavior with assert(). The
# project's default build type is RelWithDebInfo (see the root
# CMakeLists.txt / README build instructions), and CMake's own
# CMAKE_CXX_FLAGS_RELWITHDEBINFO defines NDEBUG by default - which
# silently compiles every assert() into a no-op, so a test "passes" by
# running to completion regardless of whether any of its assertions
# actually held. Found this the hard way: a genuinely broken assertion in
# a new test kept "passing" until this line was added. Force NDEBUG
# undefined for every target below, regardless of CMAKE_BUILD_TYPE, so a
# broken assertion actually fails the test from now on.
add_compile_options(-UNDEBUG)

if(WIN32)
    # Same fix, same reasoning, as plugin/CMakeLists.txt's own
    # WIN32_LEAN_AND_MEAN/NOMINMAX compile definitions (see that file's
    # comment for the full explanation): any test target here that pulls
    # in <windows.h> - directly (net/secure_random.cpp) or transitively
    # (netcore/udp_socket.h's <winsock2.h> path) - needs NOMINMAX defined
    # before the preprocessor opens ANY header in that translation unit,
    # or windows.h's own min/max macros mangle every std::min/std::max
    # call in that same file into invalid syntax (MSVC error C2589). A
    # directory-scoped add_compile_definitions is the only way to
    # guarantee "before any header" regardless of a given .cpp's own
    # #include order, and applying it to every target here (not just the
    # ones that need it today) means a future test gaining a windows.h
    # dependency doesn't silently reintroduce this.
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_executable(remote_aircraft_test
    remote_aircraft_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/sync/remote_aircraft.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/quaternion.cpp
)

target_include_directories(remote_aircraft_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_compile_features(remote_aircraft_test PRIVATE cxx_std_17)

add_test(NAME remote_aircraft_test COMMAND remote_aircraft_test)

add_executable(time_sync_test
    time_sync_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/sync/time_sync.cpp
)
target_include_directories(time_sync_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include)
target_compile_features(time_sync_test PRIVATE cxx_std_17)
add_test(NAME time_sync_test COMMAND time_sync_test)

add_executable(rendezvous_protocol_test
    rendezvous_protocol_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/rendezvous_protocol.cpp
)

target_include_directories(rendezvous_protocol_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_compile_features(rendezvous_protocol_test PRIVATE cxx_std_17)

add_test(NAME rendezvous_protocol_test COMMAND rendezvous_protocol_test)

add_executable(quaternion_test
    quaternion_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/quaternion.cpp
)

target_include_directories(quaternion_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
)

target_compile_features(quaternion_test PRIVATE cxx_std_17)

add_test(NAME quaternion_test COMMAND quaternion_test)

add_executable(dataref_sync_protocol_test
    dataref_sync_protocol_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/dataref_sync_protocol.cpp
)

target_include_directories(dataref_sync_protocol_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
)

target_compile_features(dataref_sync_protocol_test PRIVATE cxx_std_17)

add_test(NAME dataref_sync_protocol_test COMMAND dataref_sync_protocol_test)

add_executable(weather_sync_protocol_test
    weather_sync_protocol_test.cpp
)

target_include_directories(weather_sync_protocol_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
)

target_compile_features(weather_sync_protocol_test PRIVATE cxx_std_17)

add_test(NAME weather_sync_protocol_test COMMAND weather_sync_protocol_test)

add_executable(ownership_tracker_test
    ownership_tracker_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/ownership_tracker.cpp
)

target_include_directories(ownership_tracker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
)

target_compile_features(ownership_tracker_test PRIVATE cxx_std_17)

add_test(NAME ownership_tracker_test COMMAND ownership_tracker_test)

find_package(Threads REQUIRED)

add_executable(control_listener_test
    control_listener_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/control/control_listener.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/shared_cockpit_config.cpp
)

target_include_directories(control_listener_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_link_libraries(control_listener_test PRIVATE Threads::Threads)
if(WIN32)
    # control_listener.cpp pulls in netcore's UdpSocket (see
    # netcore/include/flytogether/udp_socket.h's WinSock section) - unlike
    # the other test targets above, this is the first one in this file that
    # actually opens a real socket, so it's the first one that needs this.
    target_link_libraries(control_listener_test PRIVATE ws2_32)
endif()
target_compile_features(control_listener_test PRIVATE cxx_std_17)

add_test(NAME control_listener_test COMMAND control_listener_test)

add_executable(rendezvous_client_test
    rendezvous_client_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/rendezvous_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/rendezvous_protocol.cpp
)

target_include_directories(rendezvous_client_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_link_libraries(rendezvous_client_test PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(rendezvous_client_test PRIVATE ws2_32)
endif()
target_compile_features(rendezvous_client_test PRIVATE cxx_std_17)

add_test(NAME rendezvous_client_test COMMAND rendezvous_client_test)

add_executable(shared_cockpit_config_test
    shared_cockpit_config_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/shared_cockpit_config.cpp
)

target_include_directories(shared_cockpit_config_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

# So the test can load-and-validate the REAL bundled profile files (not
# just fixtures written to a temp dir) without hardcoding a path relative
# to whatever directory ctest happens to run from - see
# TestBundledProfilesParseCleanly in shared_cockpit_config_test.cpp. This
# is this project's "lint the profile before shipping it" check (see
# docs/plan.md's Shared Cockpit profile authoring phase): a typo'd
# DATAREF/CATEGORY line here fails a build instead of only being noticed
# during a live two-instance flight test.
target_compile_definitions(shared_cockpit_config_test PRIVATE
    "SHARED_COCKPIT_PROFILES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../plugin/Resources/shared_cockpit_profiles\""
)

target_compile_features(shared_cockpit_config_test PRIVATE cxx_std_17)

add_test(NAME shared_cockpit_config_test COMMAND shared_cockpit_config_test)

add_executable(peer_list_test
    peer_list_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/peer_list.cpp
)

target_include_directories(peer_list_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_compile_features(peer_list_test PRIVATE cxx_std_17)

add_test(NAME peer_list_test COMMAND peer_list_test)

add_executable(formation_sync_test
    formation_sync_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/formation_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/peer_list.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/sync/remote_aircraft.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/quaternion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/link_quality.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/session_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/secure_random.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher/monocypher.c
)

target_include_directories(formation_sync_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher
)

target_link_libraries(formation_sync_test PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(formation_sync_test PRIVATE ws2_32 bcrypt)
endif()
target_compile_features(formation_sync_test PRIVATE cxx_std_17)

add_test(NAME formation_sync_test COMMAND formation_sync_test)

add_executable(session_crypto_test
    session_crypto_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/session_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/secure_random.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher/monocypher.c
)

target_include_directories(session_crypto_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher
)

if(WIN32)
    target_link_libraries(session_crypto_test PRIVATE bcrypt)
endif()

target_compile_features(session_crypto_test PRIVATE cxx_std_17)

add_test(NAME session_crypto_test COMMAND session_crypto_test)

add_executable(link_quality_test
    link_quality_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/link_quality.cpp
)

target_include_directories(link_quality_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
)

target_compile_features(link_quality_test PRIVATE cxx_std_17)

add_test(NAME link_quality_test COMMAND link_quality_test)

add_executable(shared_cockpit_sync_test
    shared_cockpit_sync_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/shared_cockpit_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/sync/remote_aircraft.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/quaternion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/link_quality.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/session_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/net/secure_random.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher/monocypher.c
)

target_include_directories(shared_cockpit_sync_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher
)

target_link_libraries(shared_cockpit_sync_test PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(shared_cockpit_sync_test PRIVATE ws2_32 bcrypt)
endif()
target_compile_features(shared_cockpit_sync_test PRIVATE cxx_std_17)

add_test(NAME shared_cockpit_sync_test COMMAND shared_cockpit_sync_test)
