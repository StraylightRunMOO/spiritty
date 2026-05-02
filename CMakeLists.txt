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

if(EMSCRIPTEN)
    set(_default_gl OFF)
else()
    set(_default_gl ON)
endif()
option(SPIRITTY_BUILD_GL_RENDERER "Build the OpenGL 3.3 native renderer backend" ${_default_gl})

# suspenders.h is Linux-only (io_uring). Default ON when we're on Linux and
# building the ImGui demo; OFF everywhere else. macOS path stays on the
# blocking-socket reader thread until we add a kqueue backend to suspenders.h.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
    set(_default_suspenders ON)
else()
    set(_default_suspenders OFF)
endif()
option(SPIRITTY_USE_SUSPENDERS
    "Use suspenders.h coroutines + io_uring for the ImGui demo's network loop (Linux only)"
    ${_default_suspenders})

if(SPIRITTY_USE_SUSPENDERS AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR
        "SPIRITTY_USE_SUSPENDERS=ON requires Linux (io_uring). "
        "Disable with -DSPIRITTY_USE_SUSPENDERS=OFF on this platform.")
endif()

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
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "SPIRITTY_BUILD_WASM=ON requires the Emscripten toolchain. "
            "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/EmscriptenToolchain.cmake")
    endif()
    add_subdirectory(bindings)
endif()

# ---- Headers + config -----------------------------------------------------
install(DIRECTORY include/ DESTINATION include)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spiritty_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/spiritty_config.h"
)
