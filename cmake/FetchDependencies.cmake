# Spiritty external dependencies, all consumed via FetchContent.
# Pin to commit SHAs (not tags) so builds are reproducible.

include(FetchContent)

# ---------------------------------------------------------------------------
# nlohmann/json — config files (kept from original).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
)

# ---------------------------------------------------------------------------
# Halyard — single-header C11 rope, backs scrollback + active screen.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    halyard
    GIT_REPOSITORY https://github.com/StraylightRunMOO/halyard.git
    GIT_TAG        1c0f82cce9fc3e7540d8f36fa146aa416a24dde6 # master @ 2026-09-23
)

# ---------------------------------------------------------------------------
# Memento — multi-strategy allocator (arena/pool/slab) for hot paths.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    memento
    GIT_REPOSITORY https://github.com/StraylightRunMOO/memento.git
    GIT_TAG        5dbb9e1976bdf250ea31c4b096b9cea4b99163b4 # master @ 2026-11-04
)

# ---------------------------------------------------------------------------
# Telnetty — TELNET protocol parser (ImGui demo network session).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    telnetty
    # Upstream split: the original StraylightRunMOO/telnetty repo was
    # rewritten in Go (io.ReadWriter API). The C single-header fork lives
    # at StraylightRunMOO/telnetty.h, matching the suspenders.h / halyard
    # single-header-lib naming.
    GIT_REPOSITORY https://github.com/StraylightRunMOO/telnetty.h.git
    GIT_TAG        cea57bd  # Release Telnetty 1.2.1 @ 2026-11-xx
)

# ---------------------------------------------------------------------------
# suspenders.h — Linux-only stackful coroutine runtime + io_uring + zero-copy
# channels + QoS scheduler. Drives the ImGui demo's network loop on Linux.
# In-house library; macOS support (kqueue backend) deferred post-v1.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    suspenders
    GIT_REPOSITORY https://github.com/StraylightRunMOO/suspenders.h.git
    GIT_TAG        333f294cad12d2e81e6669c2d935ad7f067b2972 # master @ 2026-11-03
)

# ---------------------------------------------------------------------------
# Dear ImGui — native GUI demo. No upstream CMakeLists, we build the sources
# manually in apps/imgui_demo/CMakeLists.txt.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.93.0
)

# ---------------------------------------------------------------------------
# GLFW — window/GL context for the native demo. macOS universal handled
# via CMAKE_OSX_ARCHITECTURES at configure time.
# ---------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
# Wayland needs libxkbcommon-dev; some target distros (Jetson L4T) ship
# Wayland headers without xkbcommon, which fails GLFW's configure. Pin
# to X11 — works on every Linux desktop GLFW supports. Override on the
# CLI with -DGLFW_BUILD_WAYLAND=ON if your environment has xkbcommon.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11     ON  CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.5.1
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
    GIT_TAG        6b3e4d1c88e62989e5fa626a9fa59b4a8664ce6f # main @ 2026-11-03
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

    # suspenders.h is Linux-only and gated by SPIRITTY_USE_SUSPENDERS. We
    # populate without MakeAvailable for the same reason as halyard/memento:
    # its CMakeLists pulls examples/tests we don't want.
    if(SPIRITTY_USE_SUSPENDERS)
        FetchContent_GetProperties(suspenders)
        if(NOT suspenders_POPULATED)
            FetchContent_Populate(suspenders)
        endif()
        add_library(spiritty_suspenders INTERFACE)
        target_include_directories(spiritty_suspenders INTERFACE
            "${suspenders_SOURCE_DIR}/include"
        )
        # liburing is a hard requirement on Linux; surface a clear error
        # rather than failing deep inside the demo build.
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing>=2.0)
        target_link_libraries(spiritty_suspenders INTERFACE PkgConfig::URING)
    endif()
endif()

if(SPIRITTY_FETCH_SHADERS)
    FetchContent_GetProperties(ghostty_shaders)
    if(NOT ghostty_shaders_POPULATED)
        FetchContent_Populate(ghostty_shaders)
    endif()
    set(SPIRITTY_GHOSTTY_SHADERS_DIR "${ghostty_shaders_SOURCE_DIR}" CACHE INTERNAL "")
endif()
