#pragma once

#include "protocol.hpp"

#include <cstdint>

namespace tongs {

// Addresses for EU4 1.37.5 (Steam, win64). plugin.toml lists the code that
// uses them as [[expect]] entries, so Anvil skips the plugin on other bytes.
struct GameLayout {
    std::uint32_t root_global_rva = 0x233FE78;   // pointer to the game root object
    std::uint32_t date_offset = 0x1DD0;          // int32: hours since year -5000
    std::uint32_t speed_offset = 0x1E08;         // int32: 0..4
    std::uint32_t session_offset = 0x1E00;       // pointer to the session object
    std::uint32_t session_vtable_rva = 0x1CB8460;
    std::uint32_t game_speed_offset = 0x418;     // session -> game-speed object
    std::uint32_t pause_flag_offset = 0x48;      // byte: player pause
    std::uint32_t is_paused_rva = 0x7E01B0;      // bool IsPaused(game_speed*)
};

// Reads a snapshot from any thread. Reads race with the game's main thread
// and are guarded by SEH; a fault is reported in `reason`, not raised.
StateSnapshot ReadState(std::uintptr_t image_base, const GameLayout& layout);

}  // namespace tongs
