# Spiritty test suite

set(TEST_SOURCES
    test_main.cpp
    test_buffer.cpp
    test_ansi_parser.cpp
    test_terminal.cpp
    test_selection.cpp
)

add_executable(spiritty_tests ${TEST_SOURCES})

target_include_directories(spiritty_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(spiritty_tests
    PRIVATE
        spiritty
)

target_compile_features(spiritty_tests PRIVATE cxx_std_17)

target_compile_options(spiritty_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Add test
add_test(NAME spiritty_unit_tests COMMAND spiritty_tests)
