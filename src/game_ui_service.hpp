#pragma once

#include "game_ui.hpp"

#include <cstdint>
#include <string>

namespace tongs {

// Registers tongs.ui version 1 (include/tongs/tongs_ui.h): the widget tree of
// game_ui.cpp for other plugins, on the main thread anvil-game reports. It
// needs neither the pipe nor a hook, so Tongs registers it on every load.
//
// Returns an error text, or an empty string on success. Call while loading.
std::string RegisterUiService(std::uintptr_t image_base, const UiLayout& layout);

}  // namespace tongs
