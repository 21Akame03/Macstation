add_executable(pipette_host
    main.cpp
    capture_mac.mm
)

target_link_libraries(pipette_host PRIVATE
    pipette_common
    /opt/homebrew/opt/jpeg-turbo/lib/libturbojpeg.dylib
    "-framework ScreenCaptureKit"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework Foundation"
)

target_include_directories(pipette_host PRIVATE
    /opt/homebrew/jpeg-turbo/include
)

set_source_files_properties(capture_mac.mm PROPERTIES
    COMPILE_FLAGS "-x objective-c++"
)
