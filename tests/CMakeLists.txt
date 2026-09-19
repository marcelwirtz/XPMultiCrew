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

add_executable(remote_aircraft_test
    remote_aircraft_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/sync/remote_aircraft.cpp
)

target_include_directories(remote_aircraft_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_compile_features(remote_aircraft_test PRIVATE cxx_std_17)

add_test(NAME remote_aircraft_test COMMAND remote_aircraft_test)

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

find_package(Threads REQUIRED)

add_executable(control_listener_test
    control_listener_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/control/control_listener.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/rendezvous_protocol.cpp
)

target_include_directories(shared_cockpit_config_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
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

add_executable(rendezvous_config_test
    rendezvous_config_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/rendezvous_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/rendezvous_protocol.cpp
)

target_include_directories(rendezvous_config_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_compile_features(rendezvous_config_test PRIVATE cxx_std_17)

add_test(NAME rendezvous_config_test COMMAND rendezvous_config_test)

add_executable(formation_sync_test
    formation_sync_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/formation_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/formation/peer_list.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/sync/remote_aircraft.cpp
)

target_include_directories(formation_sync_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_link_libraries(formation_sync_test PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(formation_sync_test PRIVATE ws2_32)
endif()
target_compile_features(formation_sync_test PRIVATE cxx_std_17)

add_test(NAME formation_sync_test COMMAND formation_sync_test)

add_executable(shared_cockpit_sync_test
    shared_cockpit_sync_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/shared_cockpit/shared_cockpit_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/src/sync/remote_aircraft.cpp
)

target_include_directories(shared_cockpit_sync_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugin/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include
)

target_link_libraries(shared_cockpit_sync_test PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(shared_cockpit_sync_test PRIVATE ws2_32)
endif()
target_compile_features(shared_cockpit_sync_test PRIVATE cxx_std_17)

add_test(NAME shared_cockpit_sync_test COMMAND shared_cockpit_sync_test)
