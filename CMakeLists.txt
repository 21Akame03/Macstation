cmake_minimum_required(VERSION 3.20)
project(Pipette VERSION 0.1.0 LANGUAGES CXX C)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(common)

# The host target links macOS-only frameworks (ScreenCaptureKit, etc.),
# so only build it on Apple platforms. The Pi target always builds.
if(APPLE)
    add_subdirectory(host)
endif()

add_subdirectory(pi)
add_subdirectory(test)
