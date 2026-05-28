add_library(pipette_common INTERFACE) # Create a library
target_include_directories(pipette_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}) # binds the library to the directory
