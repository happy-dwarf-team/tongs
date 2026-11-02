#pragma once

#include "game_symbols.hpp"
#include "protocol.hpp"

#include <cstdint>

namespace tongs {

// What the top bar's speed and pause buttons call (docs/design.md, "Speed
// buttons"; ADR 0005), from plugin/symbols.toml.
struct SpeedButtonLayout {
    std::uint32_t root_global_rva = U32(sym::root_and_date.root);      // pointer to the game root object
    std::uint32_t session_offset = U32(sym::session_of_root.session);  // root: the session
    std::uint32_t blocker = U32(sym::pause_button.blocker);            // session: the object whose byte blocks the buttons
    std::uint32_t blocked = U32(sym::pause_button.blocked);            // blocker: byte, set while the buttons do nothing
    std::uint32_t speed_up_rva = 0;      // RvaOf(sym::speed_up_button): (session)
    std::uint32_t speed_down_rva = 0;    // RvaOf(sym::speed_down_button): (session)
    std::uint32_t toggle_pause_rva = 0;  // RvaOf(sym::toggle_pause): (session, from_button)

    static_assert(sym::speed_up_button.blocker == sym::pause_button.blocker &&
                  sym::speed_down_button.blocker == sym::pause_button.blocker &&
                  sym::speed_up_button.blocked == sym::pause_button.blocked &&
                  sym::speed_down_button.blocked == sym::pause_button.blocked);
    static_assert(sym::toggle_pause.game_speed == sym::speed_object.game_speed &&
                  sym::toggle_pause.pause_flag == sym::is_paused.pause_flag);
    static_assert(sym::pause_button.from_button == 1);

    // The members only Anvil knows: call when the plugin is loading.
    static SpeedButtonLayout Load() {
        SpeedButtonLayout l;
        l.speed_up_rva = RvaOf(sym::speed_up_button);
        l.speed_down_rva = RvaOf(sym::speed_down_button);
        l.toggle_pause_rva = RvaOf(sym::toggle_pause);
        return l;
    }
};

// Main thread only, in game: one press of a speed or pause button, the call its
// handler makes. Nothing is called while the blocker byte is set.
SpeedButtonPress PressSpeedButton(std::uintptr_t image_base, const SpeedButtonLayout& layout, SpeedButton button);

}  // namespace tongs
