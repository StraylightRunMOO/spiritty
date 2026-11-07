# Spiritty WASM bindings — only built when SPIRITTY_BUILD_WASM=ON, which
# in turn requires the Emscripten toolchain. The output is a single ES6
# module (spiritty.mjs + spiritty.wasm + spiritty.d.ts) that the TS
# project in ts/ imports directly.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "SPIRITTY_BUILD_WASM=ON but the configured toolchain is not "
        "Emscripten. Use -DCMAKE_TOOLCHAIN_FILE=cmake/EmscriptenToolchain.cmake")
endif()

add_executable(spiritty_wasm embind.cpp)

set_target_properties(spiritty_wasm PROPERTIES
    OUTPUT_NAME              "spiritty"
    SUFFIX                   ".mjs"
    CXX_STANDARD             17
    CXX_STANDARD_REQUIRED    ON
    CXX_EXTENSIONS           OFF
)

target_link_libraries(spiritty_wasm PRIVATE spiritty)
target_include_directories(spiritty_wasm PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Embind needs RTTI; the rest of the project is -fno-rtti / -fno-exceptions.
# We also bind embind.cpp so it can throw across the C++/JS boundary if it
# needs to surface a TypeError or RangeError to the JS side.
target_compile_options(spiritty_wasm PRIVATE
    -frtti
    -fexceptions
    -O3
    -flto
    -Wall -Wextra -Wpedantic
)

# Emscripten link flags. Each flag is its own list element so CMake passes
# them through unmodified — packing them as one string would mangle
# everything past the first space.
target_link_options(spiritty_wasm PRIVATE
    "-O3"
    "-flto"
    "-fexceptions"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=Spiritty"
    "-sUSE_WEBGL2=1"
    "-sFULL_ES3=1"
    "-sMIN_WEBGL_VERSION=2"
    "-sMAX_WEBGL_VERSION=2"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=33554432"  # 32 MB; growth handles spikes.
    "-sFILESYSTEM=0"
    "-sENVIRONMENT=web,worker"
    # 'GL' lets the TS wrapper call GL.createContext + makeContextCurrent
    # on the <canvas>. Without that, renderer_webgl.c's first glBindFramebuffer
    # throws "Cannot read properties of undefined (reading 'bindFramebuffer')"
    # because Emscripten's GLctx is only set once a context is made current.
    "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','HEAPF32','wasmMemory','GL']"
)

# `--emit-tsd` would auto-generate spiritty.d.ts but it requires `tsc` in
# PATH (it shells out for type emission). We hand-write spiritty.d.ts in
# ts/src/wasm/spiritty.d.ts instead — kept in sync with embind.cpp by
# convention, and reviewed in PR. Add a TODO once the TS project has a
# devDependency on typescript:
#
#   "--emit-tsd=spiritty.d.ts"

# Drop output into ts/src/wasm/ so Vite resolves spiritty.mjs as a real
# module (with transforms + sourcemaps + bundling) instead of a public
# asset. Files in /public are intentionally rejected by Vite when imported
# from source code — the dev middleware refuses to load them, citing
# "should not be imported from source code; can only be referenced via
# HTML tags." Putting the .mjs in src/ resolves that and lets the .wasm
# sit next to it for `new URL("./spiritty.wasm", import.meta.url)`
# resolution from the Embind glue.
set_target_properties(spiritty_wasm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/ts/src/wasm"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/ts/src/wasm"
)
