# Spiritty C11 tests.
# All tests use the header-only framework in `test_framework.h`. Each test
# file's translation unit registers its tests at static-init time; the main
# entry point is in `test_main.c`.

set(SPIRITTY_TEST_SOURCES
    test_main.c
    test_buffer.c
    test_parser.c
    test_selection.c
    test_terminal.c
    test_renderer.c
)

add_executable(spiritty_tests ${SPIRITTY_TEST_SOURCES})

set_target_properties(spiritty_tests PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS        OFF
)

target_include_directories(spiritty_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src   # for internal.h when needed
)

target_link_libraries(spiritty_tests PRIVATE spiritty)

if(SPIRITTY_BUILD_GL_RENDERER)
    target_compile_definitions(spiritty_tests PRIVATE SPIRITTY_HAVE_GL=1)
endif()

target_compile_options(spiritty_tests PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic -Wshadow
    >
)

add_test(NAME spiritty_tests COMMAND spiritty_tests)
