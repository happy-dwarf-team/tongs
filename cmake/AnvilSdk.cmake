# The Anvil SDK (anvil_add_plugin, anvil.exe, the API 2 headers), design §16
# of Anvil. Two ways to get it:
#
#   (default)                    the SDK archive of the Anvil release below,
#                                downloaded once into build/_deps/anvil_sdk-src
#                                and checked against its SHA-256 (the release's
#                                SHA256SUMS.txt);
#   -DTONGS_ANVIL_SDK=<folder>   an unpacked SDK (bin/anvil.exe, include/, cmake/),
#                                e.g. from `cmake --install <anvil build> --component sdk`
#                                of a local Anvil checkout.
#
# A newer Anvil release: change both values below.
set(TONGS_ANVIL_VERSION 1.0.0)
set(TONGS_ANVIL_SDK_SHA256 e8ca738da4ead81f17e8e548447057c8f2b1df0529c7b00185a2f531d4ea89fe)

set(TONGS_ANVIL_SDK "" CACHE PATH "An unpacked Anvil SDK; empty downloads the SDK archive of the pinned release")

if(TONGS_ANVIL_SDK)
    find_package(AnvilPlugin REQUIRED PATHS "${TONGS_ANVIL_SDK}/cmake" NO_DEFAULT_PATH)
else()
    include(FetchContent)
    FetchContent_Declare(anvil_sdk
        URL https://github.com/happy-dwarf-team/anvil/releases/download/v${TONGS_ANVIL_VERSION}/anvil-sdk-windows-x64.zip
        URL_HASH SHA256=${TONGS_ANVIL_SDK_SHA256})
    FetchContent_MakeAvailable(anvil_sdk)
endif()

# The archive's CMakeLists.txt runs in its own directory scope; anvil.exe is
# published as a global property (used by the anvil_check test).
get_property(ANVIL_SDK_TOOL GLOBAL PROPERTY ANVIL_SDK_TOOL)
