# Simple terminal example
add_executable(simple_terminal simple_terminal.cpp)

target_link_libraries(simple_terminal
    PRIVATE
        spiritty
)

# Set C++ standard
target_compile_features(simple_terminal PRIVATE cxx_std_17)

# Add compiler flags
target_compile_options(simple_terminal PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Install example
install(TARGETS simple_terminal
    RUNTIME DESTINATION bin
)