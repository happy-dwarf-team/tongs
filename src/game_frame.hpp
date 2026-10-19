#pragma once

#include "dispatcher.hpp"
#include "frame_stats.hpp"

#include <string>

namespace tongs {

// Outer frames come from anvil-game (docs/design.md, "Dispatcher"): it hooks
// the frame function 0x14C25A0(app, outer) and, after each frame of the main
// loop on the main thread while the game is not quitting, publishes
// anvil-game.frame with the game's frame counter. Tongs runs the dispatcher's
// queue in that event and counts the frame.
//
// Returns an error text, or an empty string on success. Call while loading.
std::string SubscribeOuterFrames(Dispatcher* dispatcher, FrameStats* stats);

// Nested frames (outer = 0, drawn from inside a game update or a save load)
// are not reported by anvil-game. Tongs counts them with a mid hook at their
// only call site (symbols.toml `nested_frame`), for `frames`.
//
// Returns an error text, or an empty string on success. Call while loading.
std::string CountNestedFrames(FrameStats* stats);

}  // namespace tongs
