cmake_minimum_required(VERSION 3.25)
# version.txt holds the version; plugin/plugin.toml must carry the same one.
file(STRINGS version.txt LC_VERSION_TEXT LIMIT_COUNT 1 REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+$")
if(NOT LC_VERSION_TEXT)
    message(FATAL_ERROR "version.txt must hold a MAJOR.MINOR.PATCH version")
endif()
file(STRINGS plugin/plugin.toml LC_MANIFEST_VERSION REGEX "^version = \"[^\"]*\"$")
if(NOT LC_MANIFEST_VERSION STREQUAL "version = \"${LC_VERSION_TEXT}\"")
    message(FATAL_ERROR "plugin/plugin.toml (${LC_MANIFEST_VERSION}) does not match version.txt (${LC_VERSION_TEXT})")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS version.txt plugin/plugin.toml)
project(eu4_lab_control VERSION ${LC_VERSION_TEXT} LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(LC_BUILD_TESTS "Build unit tests" ON)

# Platform-independent parts (protocol, config, date), shared with the tests.
add_library(lc_core STATIC src/protocol.cpp src/config.cpp src/game_date.cpp)
target_include_directories(lc_core PUBLIC third_party src)
target_compile_definitions(lc_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX LC_VERSION="${LC_VERSION_TEXT}")
if(MSVC)
    target_compile_options(lc_core PUBLIC /W4 /utf-8 /permissive- /EHsc)
endif()

add_library(eu4_lab_control SHARED src/plugin.cpp src/game_state.cpp src/pipe_server.cpp)
target_link_libraries(eu4_lab_control PRIVATE lc_core advapi32)

if(LC_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE lc_core)
    if(MSVC)
        # /UNDEBUG: the tests use assert(), which Release would compile out.
        target_compile_options(test_core PRIVATE /UNDEBUG)
    endif()
    add_test(NAME core COMMAND test_core)
endif()

# Plugin folder layout: <prefix>/eu4-lab-control/{plugin.toml, eu4_lab_control.dll, LICENSE}
install(TARGETS eu4_lab_control RUNTIME DESTINATION eu4-lab-control COMPONENT plugin)
install(FILES plugin/plugin.toml LICENSE DESTINATION eu4-lab-control COMPONENT plugin)
