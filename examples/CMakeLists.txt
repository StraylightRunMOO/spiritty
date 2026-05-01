add_executable(simple_terminal simple_terminal.c)
target_link_libraries(simple_terminal PRIVATE spiritty)
set_target_properties(simple_terminal PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS        OFF
)
target_compile_options(simple_terminal PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
install(TARGETS simple_terminal RUNTIME DESTINATION bin)
