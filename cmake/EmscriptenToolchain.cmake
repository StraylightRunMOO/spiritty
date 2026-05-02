# EmscriptenToolchain.cmake — small convenience wrapper that locates the
# upstream Emscripten.cmake from $EMSDK and chains to it. Use as:
#
#   cmake -S . -B build-wasm \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/EmscriptenToolchain.cmake \
#       -DSPIRITTY_BUILD_WASM=ON
#
# We do this so consumers don't have to type the full upstream path; they
# just need EMSDK on their environment (which `emsdk_env.sh` provides).

if(DEFINED ENV{EMSDK})
    set(_emsdk_root "$ENV{EMSDK}")
elseif(DEFINED ENV{EMSCRIPTEN_ROOT})
    set(_emsdk_root "$ENV{EMSCRIPTEN_ROOT}/../..")
else()
    message(FATAL_ERROR
        "EmscriptenToolchain.cmake requires the EMSDK environment variable. "
        "Source emsdk_env.sh from your emsdk install before running cmake.")
endif()

set(_upstream_toolchain
    "${_emsdk_root}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

if(NOT EXISTS "${_upstream_toolchain}")
    message(FATAL_ERROR
        "Upstream Emscripten.cmake not found at:\n  ${_upstream_toolchain}\n"
        "Check your emsdk install or set EMSCRIPTEN_ROOT manually.")
endif()

include("${_upstream_toolchain}")
