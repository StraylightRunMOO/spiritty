# Spiritty external dependencies, all consumed via FetchContent.
# Pin to commit SHAs (not tags) so builds are reproducible.

include(FetchContent)

# ---------------------------------------------------------------------------
# nlohmann/json — config files (kept from original).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

# ---------------------------------------------------------------------------
# Halyard — single-header C11 rope, backs scrollback + active screen.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    halyard
    GIT_REPOSITORY https://github.com/StraylightRunMOO/halyard.git
    GIT_TAG        cd5b9dc820d1fe2a0f72be54cbdb4c908f196cf2 # master @ 2026-05-01
)

# ---------------------------------------------------------------------------
# Memento — multi-strategy allocator (arena/pool/slab) for hot paths.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    memento
    GIT_REPOSITORY https://github.com/StraylightRunMOO/memento.git
    GIT_TAG        c11c45ff38d06f3023923a157a8d1bd0bd350b4b # master @ 2026-05-01
)

# ---------------------------------------------------------------------------
# Telnetty — TELNET protocol parser (ImGui demo network session).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    telnetty
    GIT_REPOSITORY https://github.com/StraylightRunMOO/telnetty.git
    GIT_TAG        0effaaa93f150aca477b5e62fbfa75c4c3a14c07 # master @ 2026-05-01
)

# ---------------------------------------------------------------------------
# Dear ImGui — native GUI demo. No upstream CMakeLists, we build the sources
# manually in apps/imgui_demo/CMakeLists.txt.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
)

# ---------------------------------------------------------------------------
# GLFW — window/GL context for the native demo. macOS universal handled
# via CMAKE_OSX_ARCHITECTURES at configure time.
# ---------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)

# ---------------------------------------------------------------------------
# ghostty-shaders — runtime-loadable Ghostty-style fragment shaders.
# License unverified upstream; we vendor sources read-only and never
# modify them. If license is non-permissive we can swap to download-on-
# demand without touching the Spiritty core.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    ghostty_shaders
    GIT_REPOSITORY https://github.com/0xhckr/ghostty-shaders.git
    GIT_TAG        aa6121ba2ddd5251ac75b92729c758fe41256e55 # main @ 2026-05-01
)

# ---------------------------------------------------------------------------
# nlohmann/json builds cleanly via MakeAvailable.
# ---------------------------------------------------------------------------
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# Halyard / Memento / Telnetty are all single-header style libs whose own
# CMakeLists pull in tests, examples, and benchmarks we don't want — and
# their `BUILD_TESTS` option names collide with ours. We Populate them
# without MakeAvailable, then expose include dirs through interface targets.
# ---------------------------------------------------------------------------
foreach(_dep IN ITEMS halyard memento telnetty)
    FetchContent_GetProperties(${_dep})
    if(NOT ${_dep}_POPULATED)
        FetchContent_Populate(${_dep})
    endif()
endforeach()

add_library(spiritty_halyard INTERFACE)
target_include_directories(spiritty_halyard INTERFACE
    "${halyard_SOURCE_DIR}/include"
)

add_library(spiritty_memento INTERFACE)
target_include_directories(spiritty_memento INTERFACE
    "${memento_SOURCE_DIR}/include"
)

add_library(spiritty_telnetty INTERFACE)
target_include_directories(spiritty_telnetty INTERFACE
    "${telnetty_SOURCE_DIR}/include"
)

# ---------------------------------------------------------------------------
# Optional deps — only fetched when their feature is enabled.
# ---------------------------------------------------------------------------
if(SPIRITTY_BUILD_IMGUI_DEMO)
    FetchContent_MakeAvailable(glfw)
    # ImGui has no upstream CMakeLists; just populate and let the demo
    # compile the sources directly.
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()
    set(SPIRITTY_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}" CACHE INTERNAL "")
endif()

if(SPIRITTY_FETCH_SHADERS)
    FetchContent_GetProperties(ghostty_shaders)
    if(NOT ghostty_shaders_POPULATED)
        FetchContent_Populate(ghostty_shaders)
    endif()
    set(SPIRITTY_GHOSTTY_SHADERS_DIR "${ghostty_shaders_SOURCE_DIR}" CACHE INTERNAL "")
endif()
