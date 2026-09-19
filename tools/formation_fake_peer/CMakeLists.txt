cmake_minimum_required(VERSION 3.16)
project(formation_fake_peer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(formation_fake_peer main.cpp)

target_include_directories(formation_fake_peer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../netcore/include"
)

find_package(Threads REQUIRED)
target_link_libraries(formation_fake_peer PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(formation_fake_peer PRIVATE ws2_32)
endif()
