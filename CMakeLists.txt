cmake_minimum_required(VERSION 3.13)
set(CMAKE_CXX_STANDARD 20)

include(pico_sdk_import.cmake)

project(pico_gc_bridge C CXX ASM)

pico_sdk_init()

add_subdirectory(examples/bridge)
add_subdirectory(examples/measure)
