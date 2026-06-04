add_executable(pipette_pi main.cpp framebuffer.cpp)
target_link_libraries(pipette_pi PRIVATE pipette_common)

# The framebuffer display path decodes JPEG with libjpeg, and is Linux-only.
# On the macOS host build, framebuffer.cpp compiles to a stub, so libjpeg
# isn't needed there.
if(NOT APPLE)
    find_package(JPEG REQUIRED)
    target_link_libraries(pipette_pi PRIVATE JPEG::JPEG)
endif()
