#pragma once

#include "keys.hpp"

#include <cstdint>
#include <string>

namespace tongs {

// SDL 2.0.4 is linked statically into eu4.exe 1.37.5 with its dynamic API
// (dynapi): every SDL_* call jumps through a table of function pointers that
// SDL fills when it starts. plugin.toml checks the bytes behind these RVAs.
struct InputLayout {
    std::uint32_t push_event_slot_rva = 0x1FB02C0;  // dynapi slot 88: SDL_PushEvent
    std::uint32_t push_event_rva = 0x1746B90;       // the game's own SDL_PushEvent
};

// Queues SDL_KEYDOWN, SDL_TEXTINPUT (for keys with a character) and SDL_KEYUP
// for `key` in the game's SDL event queue, as a key press would.
// SDL_PushEvent locks the queue, so any thread may call this; the game's main
// loop handles the events on its next frame.
// Returns an error text, or an empty string on success.
std::string PressKey(std::uintptr_t image_base, const InputLayout& layout, const KeyPress& key);

}  // namespace tongs
