#include "game_state.hpp"

#include <windows.h>

#include <cstring>

namespace lc {
namespace {

using IsPausedFn = bool (*)(void* game_speed);

template <typename T>
T At(std::uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// No C++ objects with destructors here: the body runs under __try.
void ReadUnguarded(std::uintptr_t base, const GameLayout& layout, StateSnapshot& out) {
    const auto root = At<std::uintptr_t>(base + layout.root_global_rva);
    if (root == 0) {
        out.reason = "no game loaded (root object is null)";
        return;
    }
    const auto session = At<std::uintptr_t>(root + layout.session_offset);
    if (session == 0) {
        out.reason = "no game loaded (session is null)";
        return;
    }
    if (At<std::uintptr_t>(session) != base + layout.session_vtable_rva) {
        out.reason = "session object has an unexpected vtable";
        return;
    }
    const auto game_speed = At<std::uintptr_t>(session + layout.game_speed_offset);
    if (game_speed == 0) {
        out.reason = "no game loaded (game-speed object is null)";
        return;
    }
    out.date_raw = At<std::int32_t>(root + layout.date_offset);
    out.speed = At<std::int32_t>(root + layout.speed_offset);
    out.paused_flag = At<std::uint8_t>(game_speed + layout.pause_flag_offset) != 0;
    out.paused = reinterpret_cast<IsPausedFn>(base + layout.is_paused_rva)(reinterpret_cast<void*>(game_speed));
    out.in_game = true;
}

}  // namespace

StateSnapshot ReadState(std::uintptr_t image_base, const GameLayout& layout) {
    StateSnapshot snapshot;
    __try {
        ReadUnguarded(image_base, layout, snapshot);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        snapshot = StateSnapshot{};
        snapshot.reason = "access violation while reading game state (game state changing?)";
    }
    return snapshot;
}

}  // namespace lc
