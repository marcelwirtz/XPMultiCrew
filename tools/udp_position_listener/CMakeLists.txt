cmake_minimum_required(VERSION 3.16)
project(udp_position_listener CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(udp_position_listener main.cpp)

target_include_directories(udp_position_listener PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../netcore/include"
)

if(WIN32)
    target_link_libraries(udp_position_listener PRIVATE ws2_32)
endif()
