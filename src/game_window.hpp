#pragma once

#include "protocol.hpp"

namespace tongs {

// Finds the game's SDL window (class "SDL_app") among the visible top-level
// windows of this process and reads its client area. Any thread may call it.
WindowSnapshot ReadGameWindow();

}  // namespace tongs
