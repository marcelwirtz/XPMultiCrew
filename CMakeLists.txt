cmake_minimum_required(VERSION 3.16)
project(XPMultiCrew)

enable_testing()

add_subdirectory(plugin)
add_subdirectory(tools/udp_position_listener)
add_subdirectory(tools/formation_fake_peer)
add_subdirectory(tests)
