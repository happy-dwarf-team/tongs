#pragma once

#include "game_symbols.hpp"
#include "input.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tongs {

// SDL 2.0.4 is linked statically into eu4.exe 1.37.5 with its dynamic API
// (dynapi): every SDL_* call jumps through a table of function pointers that
// SDL fills when it starts. symbols.toml takes both RVAs from the code that
// fills the slot (sdl_dynapi_init) and checks SDL_PushEvent itself.
struct InputLayout {
    std::uint32_t push_event_slot_rva = U32(sym::sdl_dynapi_init.push_event_slot);  // dynapi slot 88: SDL_PushEvent
    std::uint32_t push_event_rva = U32(sym::sdl_dynapi_init.push_event);            // the game's own SDL_PushEvent
};

// Queues the events in order in the game's SDL event queue, as SDL would for
// the player's keyboard and mouse. SDL_PushEvent locks the queue, so any
// thread may call this; the game's main loop handles the events on its next
// frame. Returns an error text, or an empty string on success.
std::string PushInput(std::uintptr_t image_base, const InputLayout& layout, const std::vector<InputEvent>& events);

}  // namespace tongs
