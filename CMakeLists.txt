cmake_minimum_required(VERSION 3.25)
# version.txt holds the version; plugin/plugin.toml must carry the same one.
file(STRINGS version.txt TONGS_VERSION_TEXT LIMIT_COUNT 1 REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+$")
if(NOT TONGS_VERSION_TEXT)
    message(FATAL_ERROR "version.txt must hold a MAJOR.MINOR.PATCH version")
endif()
file(STRINGS plugin/plugin.toml TONGS_MANIFEST_VERSION REGEX "^version = \"[^\"]*\"$")
if(NOT TONGS_MANIFEST_VERSION STREQUAL "version = \"${TONGS_VERSION_TEXT}\"")
    message(FATAL_ERROR "plugin/plugin.toml (${TONGS_MANIFEST_VERSION}) does not match version.txt (${TONGS_VERSION_TEXT})")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS version.txt plugin/plugin.toml)
project(tongs VERSION ${TONGS_VERSION_TEXT} LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(TONGS_BUILD_TESTS "Build unit tests" ON)

# Platform-independent parts (protocol, config, date, dispatcher, selectors), shared with the tests.
add_library(tongs_core STATIC src/protocol.cpp src/config.cpp src/game_date.cpp src/keys.cpp src/input.cpp src/dispatcher.cpp src/frame_stats.cpp src/ui_tree.cpp src/events.cpp)
target_include_directories(tongs_core PUBLIC third_party src)
target_compile_definitions(tongs_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX TONGS_VERSION="${TONGS_VERSION_TEXT}")
if(MSVC)
    target_compile_options(tongs_core PUBLIC /W4 /utf-8 /permissive- /EHsc)
endif()

add_library(tongs SHARED src/plugin.cpp src/game_state.cpp src/game_input.cpp src/game_window.cpp src/game_cursor.cpp src/game_frame.cpp src/game_threads.cpp src/game_ui.cpp src/game_events.cpp src/game_memory.cpp src/pipe_server.cpp src/screenshot.cpp)
target_link_libraries(tongs PRIVATE tongs_core advapi32 user32 gdi32 ole32 windowscodecs)

if(TONGS_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE tongs_core)
    if(MSVC)
        # /UNDEBUG: the tests use assert(), which Release would compile out.
        target_compile_options(test_core PRIVATE /UNDEBUG)
    endif()
    add_test(NAME core COMMAND test_core)
endif()

# Plugin folder layout: <prefix>/tongs/{plugin.toml, tongs.dll, LICENSE}
install(TARGETS tongs RUNTIME DESTINATION tongs COMPONENT plugin)
install(FILES plugin/plugin.toml LICENSE DESTINATION tongs COMPONENT plugin)
