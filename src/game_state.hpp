#pragma once

#include "game_symbols.hpp"
#include "protocol.hpp"

#include <cstdint>

namespace tongs {

// Where EU4 1.37.5 (Steam, win64) keeps the date, the speed and the pause
// (plugin/symbols.toml).
struct GameLayout {
    std::uint32_t root_global_rva = U32(sym::root_and_date.root);  // pointer to the game root object
    std::uint32_t date_offset = U32(sym::root_and_date.date);      // int32: hours since year -5000
    std::uint32_t speed_offset = U32(sym::speed_of_root.speed);    // int32: 0..4
    std::uint32_t session_offset = U32(sym::session_of_root.session);  // pointer to the session object
    std::uint32_t session_vtable_rva = 0;                          // SessionVtableRva(), set when loading
    std::uint32_t game_speed_offset = U32(sym::speed_object.game_speed);  // session -> game-speed object
    std::uint32_t pause_flag_offset = U32(sym::is_paused.pause_flag);     // byte: player pause
    std::uint32_t is_paused_rva = 0;                               // RvaOf(sym::is_paused), set when loading

    // The members only Anvil knows: call when the plugin is loading.
    static GameLayout Load() {
        GameLayout l;
        l.session_vtable_rva = SessionVtableRva();
        l.is_paused_rva = RvaOf(sym::is_paused);
        return l;
    }
};

// Reads a snapshot from any thread. Reads race with the game's main thread
// and are guarded by SEH; a fault is reported in `reason`, not raised.
StateSnapshot ReadState(std::uintptr_t image_base, const GameLayout& layout);

}  // namespace tongs
