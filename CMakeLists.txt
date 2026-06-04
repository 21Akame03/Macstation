cmake_minimum_required(VERSION 3.20)
project(Pipette VERSION 0.1.0 LANGUAGES CXX C)

# Default to an optimized build. Without this, a plain `cmake -S . -B build`
# configures with no build type => no -O flags => -O0, which makes the Pi's
# per-pixel framebuffer blit ~10-30x slower (it was the whole bottleneck).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

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
