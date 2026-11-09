#include "game_map_mode.hpp"

#include "game_symbols.hpp"

#include <windows.h>

#include <cstring>

namespace tongs {
namespace {

using SetMapModeFn = void (*)(void* session, std::int32_t mode, std::uint8_t a, std::uint8_t b);

// No C++ objects with destructors here: the body runs under __try.
bool Call(SetMapModeFn fn, std::uintptr_t session, std::int32_t number, unsigned long* code) {
    __try {
        fn(reinterpret_cast<void*>(session), number, 0, 0);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

std::int32_t PluginMapModeNumber(const MapModesService* service, std::string_view id) {
    if (service == nullptr || service->mode_id == nullptr) return -1;
    // The game's enum has 85 values; anvil-map lends some of them.
    for (std::int32_t n = 0; n < 85; ++n) {
        const char* m = service->mode_id(n);
        if (m != nullptr && id == m) return n;
    }
    return -1;
}

bool CallSetMapMode(std::uintptr_t base, std::uintptr_t session, std::int32_t number, unsigned long& fault_code) {
    if (session == 0) return true;
    const auto fn = reinterpret_cast<SetMapModeFn>(base + RvaOf(sym::set_current_map_mode));
    return Call(fn, session, number, &fault_code);
}

}  // namespace tongs
