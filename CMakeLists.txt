cmake_minimum_required(VERSION 3.25)
# version.txt holds the version; plugin/plugin.toml must carry the same one.
file(STRINGS version.txt TONGS_VERSION_TEXT LIMIT_COUNT 1 REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+$")
if(NOT TONGS_VERSION_TEXT)
    message(FATAL_ERROR "version.txt must hold a MAJOR.MINOR.PATCH version")
endif()
file(STRINGS plugin/plugin.toml TONGS_MANIFEST_VERSION REGEX "^version = \"[^\"]*\"$")
# The first one is the plugin's; [[dependencies]] tables below have their own.
list(GET TONGS_MANIFEST_VERSION 0 TONGS_MANIFEST_VERSION)
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

# anvil_add_plugin and anvil.exe from the Anvil SDK (cmake/AnvilSdk.cmake).
include(cmake/AnvilSdk.cmake)

# Platform-independent parts (protocol, settings, date, dispatcher, selectors), shared with the tests.
add_library(tongs_core STATIC src/protocol.cpp src/config.cpp src/game_date.cpp src/keys.cpp src/input.cpp src/dispatcher.cpp src/frame_stats.cpp src/ui_tree.cpp src/ui_definitions.cpp src/ui_structure.cpp src/ui_service.cpp src/events.cpp src/console.cpp src/notifications.cpp src/notify_sources.cpp src/load_watch.cpp src/world.cpp src/navigation.cpp)
# include/: the tongs.ui header for consumers, which needs Anvil's plugin header.
get_property(TONGS_ANVIL_INCLUDE_DIR GLOBAL PROPERTY ANVIL_SDK_INCLUDE_DIR)
target_include_directories(tongs_core PUBLIC src include "${TONGS_ANVIL_INCLUDE_DIR}")
target_compile_definitions(tongs_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX TONGS_VERSION="${TONGS_VERSION_TEXT}")
if(MSVC)
    target_compile_options(tongs_core PUBLIC /W4 /utf-8 /permissive- /EHsc)
endif()

# The plugin folder <build>/plugin/tongs/{plugin.toml, symbols.toml, settings.toml, tongs.dll}:
# anvil gen writes symbols.gen.hpp and settings.gen.hpp from plugin/, and
# tongs-check runs `anvil check plugin` (with -DEU4_EXE=<eu4.exe> it also
# verifies the symbols and rewrites plugin/symbols.lock).
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
anvil_add_plugin(tongs PLUGIN_DIR plugin SOURCES src/plugin.cpp src/settings.cpp src/game_state.cpp src/game_input.cpp src/game_window.cpp src/game_cursor.cpp src/game_frame.cpp src/game_threads.cpp src/game_ui.cpp src/game_ui_service.cpp src/game_events.cpp src/game_console.cpp src/game_world.cpp src/game_navigation.cpp src/game_files.cpp src/game_memory.cpp src/pipe_server.cpp src/screenshot.cpp)
unset(CMAKE_COMPILE_WARNING_AS_ERROR)
target_link_libraries(tongs PRIVATE tongs_core advapi32 user32 gdi32 ole32 shell32 windowscodecs)

if(TONGS_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE tongs_core)
    if(MSVC)
        # /UNDEBUG: the tests use assert(), which Release would compile out.
        target_compile_options(test_core PRIVATE /UNDEBUG)
    endif()
    add_test(NAME core COMMAND test_core)
    # symbols.lock matches symbols.toml and says "ok" (anvil check without the game).
    add_test(NAME anvil_check COMMAND "${ANVIL_SDK_TOOL}" check "${CMAKE_CURRENT_SOURCE_DIR}/plugin")
endif()

# Plugin folder layout: <prefix>/tongs/{plugin.toml, symbols.toml, settings.toml, tongs.dll, LICENSE, include/}
install(DIRECTORY ${CMAKE_BINARY_DIR}/plugin/tongs/ DESTINATION tongs COMPONENT plugin)
install(FILES LICENSE DESTINATION tongs COMPONENT plugin)
# The tongs.ui header for consumers (include/tongs/tongs_ui.h), inside the plugin folder:
# a release zip extracts into anvil/plugins/, where any other folder would be one more plugin.
install(FILES include/tongs/tongs_ui.h DESTINATION tongs/include/tongs COMPONENT plugin)
