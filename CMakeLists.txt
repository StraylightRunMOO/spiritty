cmake_minimum_required(VERSION 3.16)
project(Spiritty
    VERSION   0.1.0
    LANGUAGES C CXX
)

# ---- Standards ------------------------------------------------------------
set(CMAKE_C_STANDARD              11)
set(CMAKE_C_STANDARD_REQUIRED     ON)
set(CMAKE_C_EXTENSIONS            OFF)
set(CMAKE_CXX_STANDARD            17)
set(CMAKE_CXX_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_EXTENSIONS          OFF)

# Default visibility: hidden — public API uses SPIRITTY_API explicitly.
set(CMAKE_C_VISIBILITY_PRESET   hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Options --------------------------------------------------------------
option(SPIRITTY_BUILD_EXAMPLES   "Build C example programs"          ON)
option(SPIRITTY_BUILD_TESTS      "Build the test suite"              OFF)
option(SPIRITTY_BUILD_IMGUI_DEMO "Build the GLFW + ImGui demo"       OFF)
option(SPIRITTY_BUILD_WASM       "Build Embind bindings + WASM core" OFF)
option(SPIRITTY_FETCH_SHADERS    "Fetch the ghostty-shaders pack"    OFF)
option(SPIRITTY_FUZZ             "Build libFuzzer harnesses"         OFF)

# ---- Dependencies ---------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDependencies.cmake)

# ---- Subdirectories -------------------------------------------------------
add_subdirectory(src)

if(SPIRITTY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SPIRITTY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(SPIRITTY_BUILD_IMGUI_DEMO)
    add_subdirectory(apps/imgui_demo)
endif()

if(SPIRITTY_BUILD_WASM)
    add_subdirectory(bindings)
endif()

# ---- Headers + config -----------------------------------------------------
install(DIRECTORY include/ DESTINATION include)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spiritty_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/spiritty_config.h"
)
