# Spiritty core — C11. The C++ legacy files are excluded from the build
# during the Phase 1/2 transition; once the C port is feature-complete
# they get deleted.

set(SPIRITTY_CORE_SOURCES
    terminal.c
)

add_library(spiritty_core STATIC ${SPIRITTY_CORE_SOURCES})

set_target_properties(spiritty_core PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS        OFF
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
)

target_compile_definitions(spiritty_core
    PRIVATE SPIRITTY_BUILDING=1
)

target_include_directories(spiritty_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>
)

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
