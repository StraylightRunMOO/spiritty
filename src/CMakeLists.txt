# Spiritty core — C11. The C++ legacy files are excluded from the build
# during the Phase 1/2 transition; once the C port is feature-complete
# they get deleted.

include(${CMAKE_SOURCE_DIR}/cmake/EmbedShaders.cmake)

set(SPIRITTY_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SPIRITTY_SHADER_FILES
    ${SPIRITTY_SHADER_DIR}/common.glsl
    ${SPIRITTY_SHADER_DIR}/cell_bg.v.glsl
    ${SPIRITTY_SHADER_DIR}/cell_bg.f.glsl
    ${SPIRITTY_SHADER_DIR}/cell_text.v.glsl
    ${SPIRITTY_SHADER_DIR}/cell_text.f.glsl
    ${SPIRITTY_SHADER_DIR}/cursor.v.glsl
    ${SPIRITTY_SHADER_DIR}/cursor.f.glsl
    ${SPIRITTY_SHADER_DIR}/post.v.glsl
    ${SPIRITTY_SHADER_DIR}/post.f.glsl
    ${SPIRITTY_SHADER_DIR}/shadertoy_prefix.glsl
)
set(SPIRITTY_SHADERS_EMBED_H ${CMAKE_CURRENT_BINARY_DIR}/shaders_embed.h)

spiritty_embed_shaders(
    OUTPUT  ${SPIRITTY_SHADERS_EMBED_H}
    SHADERS ${SPIRITTY_SHADER_FILES}
)

set(SPIRITTY_BUILTIN_TTF_H ${CMAKE_CURRENT_BINARY_DIR}/builtin_ttf.h)
add_custom_command(
    OUTPUT ${SPIRITTY_BUILTIN_TTF_H}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/embed_ttf.py
            ${CMAKE_SOURCE_DIR}/third_party/ibm-plex-mono/IBMPlexMono-Regular.ttf
            ${SPIRITTY_BUILTIN_TTF_H}
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/embed_ttf.py
        ${CMAKE_SOURCE_DIR}/third_party/ibm-plex-mono/IBMPlexMono-Regular.ttf
    COMMENT "Embedding IBM Plex Mono TTF"
)

set(SPIRITTY_CORE_SOURCES
    buffer.c
    parser.c
    selection.c
    terminal.c
    renderer_null.c
    font.c
    atlas.c
)

if(SPIRITTY_BUILD_GL_RENDERER)
    list(APPEND SPIRITTY_CORE_SOURCES renderer_gl.c)
endif()

# WebGL2 backend is compiled under Emscripten only. The .c file guards
# itself with #ifdef __EMSCRIPTEN__ so listing it elsewhere is harmless,
# but we keep the gate explicit to make the dependency obvious.
if(EMSCRIPTEN)
    list(APPEND SPIRITTY_CORE_SOURCES renderer_webgl.c)
endif()

add_library(spiritty_core STATIC
    ${SPIRITTY_CORE_SOURCES}
    ${SPIRITTY_SHADERS_EMBED_H}
    ${SPIRITTY_BUILTIN_TTF_H}
)
set_source_files_properties(font.c PROPERTIES OBJECT_DEPENDS ${SPIRITTY_BUILTIN_TTF_H})

set_target_properties(spiritty_core PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS        OFF
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
)

target_compile_definitions(spiritty_core
    PRIVATE SPIRITTY_BUILDING=1
    $<$<BOOL:${SPIRITTY_BUILD_GL_RENDERER}>:SPIRITTY_HAVE_GL=1>
    $<$<BOOL:${EMSCRIPTEN}>:SPIRITTY_HAVE_WEBGL=1>
)

if(SPIRITTY_BUILD_GL_RENDERER AND NOT EMSCRIPTEN)
    find_package(OpenGL REQUIRED)
    target_link_libraries(spiritty_core PRIVATE OpenGL::GL)
endif()

target_include_directories(spiritty_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(UNIX AND NOT EMSCRIPTEN)
    target_link_libraries(spiritty_core PRIVATE m)
endif()

target_link_libraries(spiritty_core
    PRIVATE
        spiritty_halyard
        spiritty_memento
)

target_compile_options(spiritty_core PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic -Wshadow
        -Wstrict-aliasing=2 -Wcast-align -Wnull-dereference
        -fno-plt
    >
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# Public umbrella target. Consumers link `spiritty`.
add_library(spiritty INTERFACE)
target_link_libraries(spiritty INTERFACE spiritty_core)
target_include_directories(spiritty INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>
)

install(TARGETS spiritty_core spiritty
    EXPORT  spirittyTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/spiritty
    DESTINATION include
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
)
