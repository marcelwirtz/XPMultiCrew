cmake_minimum_required(VERSION 3.16)
project(sc_fake_peer CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XPMC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Same wire code as the plugin (rendezvous, crypto, Shared Cockpit
# protocols) - all XPLM-free, like the tests that cover it.
add_executable(sc_fake_peer
    main.cpp
    ${XPMC_ROOT}/plugin/src/formation/rendezvous_client.cpp
    ${XPMC_ROOT}/plugin/src/formation/rendezvous_protocol.cpp
    ${XPMC_ROOT}/plugin/src/net/session_crypto.cpp
    ${XPMC_ROOT}/plugin/src/net/secure_random.cpp
    ${XPMC_ROOT}/plugin/src/shared_cockpit/dataref_sync_protocol.cpp
    ${XPMC_ROOT}/plugin/src/shared_cockpit/controls_sync_protocol.cpp
    ${XPMC_ROOT}/plugin/src/shared_cockpit/command_sync_protocol.cpp
    ${XPMC_ROOT}/plugin/src/shared_cockpit/shared_cockpit_config.cpp
    ${XPMC_ROOT}/third_party/monocypher/monocypher.c
)

target_include_directories(sc_fake_peer PRIVATE
    ${XPMC_ROOT}/plugin/include
    ${XPMC_ROOT}/netcore/include
    ${XPMC_ROOT}/third_party/monocypher
)

find_package(Threads REQUIRED)
target_link_libraries(sc_fake_peer PRIVATE Threads::Threads)

if(WIN32)
    target_compile_definitions(sc_fake_peer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(sc_fake_peer PRIVATE ws2_32 bcrypt)
endif()
