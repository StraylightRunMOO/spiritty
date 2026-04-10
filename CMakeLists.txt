cmake_minimum_required(VERSION 3.16)
project(Spiritty VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(SPIRITTY_BUILD_EXAMPLES "Build Spiritty examples" ON)
option(SPIRITTY_BUILD_TESTS "Build Spiritty tests" OFF)

# FetchContent for dependencies
include(FetchContent)

# Three.js (we'll use it via npm for the web build)
# For C++ part, we need a JSON library
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Add subdirectories
add_subdirectory(src)

if(SPIRITTY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SPIRITTY_BUILD_TESTS)
    add_subdirectory(test)
endif()

# Install headers
install(DIRECTORY include/ DESTINATION include)

# Create config file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spiritty_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/spiritty_config.h"
)

# Export compile commands for development
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)