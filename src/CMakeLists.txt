# Spiritty core library
add_library(spiritty_core STATIC
    terminal.cpp
    buffer.cpp
    ansi_parser.cpp
    webgl_renderer.cpp
    selection_manager.cpp
)

target_include_directories(spiritty_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(spiritty_core
    PRIVATE
        nlohmann_json::nlohmann_json
)

# Set C++ standard
target_compile_features(spiritty_core PUBLIC cxx_std_17)

# Add compiler flags
target_compile_options(spiritty_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Create main spiritty library
add_library(spiritty INTERFACE)

target_link_libraries(spiritty INTERFACE
    spiritty_core
)

target_include_directories(spiritty INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>
)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/spiritty
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Install libraries
install(TARGETS spiritty_core spiritty
    EXPORT spiritty-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# Export targets
install(EXPORT spiritty-targets
    FILE spiritty-targets.cmake
    NAMESPACE spiritty::
    DESTINATION lib/cmake/spiritty
)