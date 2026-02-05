cmake_minimum_required(VERSION 3.10)
project(wheel-hid-emulator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.cpp
    src/config.cpp
    src/wheel_device.cpp
    src/hid/hid_device.cpp
    src/hid/vjoy_loader.cpp
    src/logging/logger.cpp
    src/input/device_scanner.cpp
    src/input/input_manager.cpp
)

include_directories(src/vjoy_sdk/inc)

add_executable(wheel-emulator ${SOURCES})

target_link_libraries(wheel-emulator winmm)
