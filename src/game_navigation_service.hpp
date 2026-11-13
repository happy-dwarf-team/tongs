#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tongs {

class ActivityGate;  // activity_gate.hpp

// Registers tongs.navigation version 1 (include/tongs/tongs_navigation.h,
// docs/adr/0004): the selection and province open of game_navigation.cpp for
// other plugins, on the main thread anvil-game reports. It needs neither the
// pipe nor input, so Tongs registers it on every load.
//
// `suspended` tells whether Anvil suspended Tongs; `gate` is the plugin's
// activity gate, whose navigation breaker the pipe shares. Returns an error
// text, or an empty string on success. Call while loading.
std::string RegisterNavigationService(std::uintptr_t image_base, std::function<bool()> suspended,
                                      std::shared_ptr<ActivityGate> gate);

// With the pipe on: counts every open_province that called the game as a client
// call of the pipe (ADR 0003's counts), through the gate. Call while loading,
// after RegisterNavigationService.
void CountNavigationServiceCalls(std::function<void()> count);

}  // namespace tongs
