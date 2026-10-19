# The Anvil SDK (anvil_add_plugin, anvil.exe, the API 2 headers), design §16
# of Anvil. Two ways to get it:
#
#   -DTONGS_ANVIL_SDK=<folder>   an unpacked SDK (bin/anvil.exe, include/, cmake/),
#                                e.g. from `cmake --install <anvil build> --component sdk`;
#   (default)                    Anvil's sources at ANVIL_SOURCE_COMMIT, built
#                                once into <build>/anvil-sdk. FetchContent's
#                                FETCHCONTENT_SOURCE_DIR_ANVIL=<anvil checkout>
#                                builds a local checkout instead (into this
#                                build tree; the checkout's own build is not
#                                touched).
#
# TODO(anvil 1.0): Anvil 1.0 publishes anvil-sdk-windows-x64.zip with every
# release. Replace the source build below with
#   FetchContent_Declare(anvil_sdk
#       URL https://github.com/happy-dwarf-team/anvil/releases/download/v1.0.0/anvil-sdk-windows-x64.zip
#       URL_HASH SHA256=<hash from the release's SHA256SUMS.txt>)
#   FetchContent_MakeAvailable(anvil_sdk)
# and drop ANVIL_SOURCE_COMMIT.

# Anvil main with plugin API 2, anvil-game, settings and the SDK package (anvil#76).
set(ANVIL_SOURCE_COMMIT ab8f7e699b408e94f117f2b8d5f89e19773d0114)

set(TONGS_ANVIL_SDK "" CACHE PATH "An unpacked Anvil SDK; empty builds one from Anvil's sources")

if(TONGS_ANVIL_SDK)
    set(_tongs_sdk "${TONGS_ANVIL_SDK}")
else()
    include(FetchContent)
    # SOURCE_SUBDIR names no CMakeLists.txt, so MakeAvailable only fetches:
    # the SDK is built in its own tree below, not as part of Tongs.
    FetchContent_Declare(anvil
        GIT_REPOSITORY https://github.com/happy-dwarf-team/anvil.git
        GIT_TAG ${ANVIL_SOURCE_COMMIT}
        SOURCE_SUBDIR fetch-only)
    FetchContent_MakeAvailable(anvil)

    set(_tongs_sdk "${CMAKE_BINARY_DIR}/anvil-sdk")
    set(_tongs_sdk_build "${CMAKE_BINARY_DIR}/anvil-sdk-build")
    set(_tongs_sdk_stamp "${_tongs_sdk}/tongs-source.txt")
    # Rebuilt when the source changes: another commit, or another local checkout.
    set(_tongs_sdk_source "${anvil_SOURCE_DIR} ${ANVIL_SOURCE_COMMIT}")
    if(EXISTS "${_tongs_sdk_stamp}")
        file(READ "${_tongs_sdk_stamp}" _tongs_sdk_built)
    else()
        set(_tongs_sdk_built "")
    endif()
    if(NOT EXISTS "${_tongs_sdk}/bin/anvil.exe" OR NOT _tongs_sdk_built STREQUAL _tongs_sdk_source)
        message(STATUS "Building the Anvil SDK from ${anvil_SOURCE_DIR}")
        set(_tongs_generator -G "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            list(APPEND _tongs_generator -A "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        if(CMAKE_GENERATOR_TOOLSET)
            list(APPEND _tongs_generator -T "${CMAKE_GENERATOR_TOOLSET}")
        endif()
        foreach(_step IN ITEMS configure build install)
            if(_step STREQUAL "configure")
                set(_command ${CMAKE_COMMAND} -S "${anvil_SOURCE_DIR}" -B "${_tongs_sdk_build}" ${_tongs_generator}
                    -DCMAKE_BUILD_TYPE=Release -DANVIL_BUILD_TESTS=OFF -DANVIL_BUILD_EXAMPLES=OFF -DANVIL_BUILD_BENCH=OFF)
            elseif(_step STREQUAL "build")
                set(_command ${CMAKE_COMMAND} --build "${_tongs_sdk_build}" --config Release --target anvil)
            else()
                set(_command ${CMAKE_COMMAND} --install "${_tongs_sdk_build}" --config Release --component sdk
                    --prefix "${_tongs_sdk}")
            endif()
            execute_process(COMMAND ${_command} RESULT_VARIABLE _result)
            if(NOT _result EQUAL 0)
                message(FATAL_ERROR "Building the Anvil SDK failed at the ${_step} step (${_result})")
            endif()
        endforeach()
        file(WRITE "${_tongs_sdk_stamp}" "${_tongs_sdk_source}")
    endif()
endif()

find_package(AnvilPlugin REQUIRED PATHS "${_tongs_sdk}/cmake" NO_DEFAULT_PATH)
