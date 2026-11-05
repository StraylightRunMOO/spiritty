# apps/imgui_demo/CMakeLists.txt
#
# The native GUI demo: GLFW window + OpenGL 3.3 core + Dear ImGui +
# Spiritty terminal + Telnetty-driven network session.
#
# Network backend selection:
#   - SPIRITTY_USE_SUSPENDERS=ON  (Linux default) → net_suspenders.c
#       (suspenders.h coroutines + io_uring + Telnetty)
#   - otherwise                   → net_thread.cpp
#       (std::thread + blocking POSIX socket + Telnetty)
#
# Both backends define TELNETTY_IMPLEMENTATION in exactly one TU; the
# other backend's TU does *not* link in the same build. The terminal
# widget and main.cpp are network-backend-agnostic — they only see net.h.

set(_imgui ${SPIRITTY_IMGUI_SOURCE_DIR})
if(NOT _imgui)
    message(FATAL_ERROR
        "SPIRITTY_IMGUI_SOURCE_DIR is empty — FetchDependencies.cmake must "
        "populate ImGui before this subdir is added.")
endif()

# Core ImGui + the GLFW/OpenGL3 backends. We compile these directly since
# upstream ships no CMakeLists.
set(IMGUI_SOURCES
    "${_imgui}/imgui.cpp"
    "${_imgui}/imgui_demo.cpp"
    "${_imgui}/imgui_draw.cpp"
    "${_imgui}/imgui_tables.cpp"
    "${_imgui}/imgui_widgets.cpp"
    "${_imgui}/backends/imgui_impl_glfw.cpp"
    "${_imgui}/backends/imgui_impl_opengl3.cpp"
)

add_executable(spiritty_demo
    main.cpp
    terminal_widget.cpp
    ${IMGUI_SOURCES}
)

# Network backend — exactly one TU.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SPIRITTY_USE_SUSPENDERS)
    target_sources(spiritty_demo PRIVATE net_suspenders.c)
    # suspenders.h reaches into Memento's thread-local heap, so the TU
    # that defines SUSPENDERS_IMPLEMENTATION must also define
    # MEMENTO_IMPLEMENTATION and have memento.h on its include path.
    target_link_libraries(spiritty_demo PRIVATE spiritty_suspenders spiritty_memento)
    target_compile_definitions(spiritty_demo PRIVATE SPIRITTY_NET_BACKEND_SUSPENDERS=1)
else()
    target_sources(spiritty_demo PRIVATE net_thread.cpp)
    target_compile_definitions(spiritty_demo PRIVATE SPIRITTY_NET_BACKEND_THREAD=1)
endif()

target_include_directories(spiritty_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${_imgui}"
    "${_imgui}/backends"
)

# The widget needs to know which renderer factory to call. The native
# build always has the GL renderer unless the user explicitly opted out.
if(SPIRITTY_BUILD_GL_RENDERER)
    target_compile_definitions(spiritty_demo PRIVATE SPIRITTY_HAVE_GL=1)
endif()

target_link_libraries(spiritty_demo PRIVATE
    spiritty
    spiritty_telnetty
    glfw
)

# OpenGL — Linux pulls in libGL via GLFW, but we link explicitly so the
# imgui_impl_opengl3 TU resolves cleanly on platforms where GLFW links
# against a software/EGL backend.
find_package(OpenGL REQUIRED)
target_link_libraries(spiritty_demo PRIVATE OpenGL::GL)

# Threads — std::thread in net_thread.cpp + suspenders' pthread mutex.
find_package(Threads REQUIRED)
target_link_libraries(spiritty_demo PRIVATE Threads::Threads)

# ImGui's GLFW/GL3 backends use ${CMAKE_DL_LIBS} on Linux for runtime
# symbol lookup. Harmless to link unconditionally.
if(CMAKE_DL_LIBS)
    target_link_libraries(spiritty_demo PRIVATE ${CMAKE_DL_LIBS})
endif()

# Quiet a few -Wpedantic / -Wcast-align warnings that Dear ImGui's own
# sources trip — we don't want our strict flags to fail the demo build.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(${IMGUI_SOURCES} PROPERTIES
        COMPILE_FLAGS "-Wno-pedantic -Wno-cast-align -Wno-unused-parameter")
endif()
