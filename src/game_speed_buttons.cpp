#include "game_speed_buttons.hpp"

#include "game_memory.hpp"

#include <windows.h>

namespace tongs {
namespace {

using SessionFn = void (*)(void* session);
using TogglePauseFn = void (*)(void* session, bool from_button);

// No C++ objects with destructors here: the body runs under __try.
bool Call(std::uintptr_t base, const SpeedButtonLayout& l, SpeedButton button, std::uintptr_t session,
          unsigned long* code) {
    __try {
        auto* s = reinterpret_cast<void*>(session);
        switch (button) {
        case SpeedButton::Faster: reinterpret_cast<SessionFn>(base + l.speed_up_rva)(s); break;
        case SpeedButton::Slower: reinterpret_cast<SessionFn>(base + l.speed_down_rva)(s); break;
        case SpeedButton::Pause: reinterpret_cast<TogglePauseFn>(base + l.toggle_pause_rva)(s, true); break;
        }
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

SpeedButtonPress PressSpeedButton(std::uintptr_t base, const SpeedButtonLayout& l, SpeedButton button) {
    SpeedButtonPress out;
    std::uintptr_t root = 0, session = 0, blocker = 0;
    std::uint8_t blocked = 0;
    if (!Read(base + l.root_global_rva, root) || root == 0 || !Read(root + l.session_offset, session) || session == 0 ||
        !Read(session + l.blocker, blocker) || blocker == 0 || !Read(blocker + l.blocked, blocked)) {
        out.error = "the session is not readable";
        return out;
    }
    // The pause button checks this byte before it calls; the speed functions check it themselves.
    if (blocked != 0) {
        out.blocked = true;
        return out;
    }
    if (!Call(base, l, button, session, &out.fault_code)) return out;
    out.pressed = true;
    return out;
}

}  // namespace tongs
