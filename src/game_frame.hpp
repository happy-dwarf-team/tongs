#pragma once

#include "dispatcher.hpp"
#include "frame_stats.hpp"

#include <anvil/anvil_plugin.h>

#include <string>

namespace tongs {

// The game's frame function 0x14C25A0(app, outer) runs once per frame of the
// main loop (outer = 1) and also for frames drawn from inside a game update or
// a save load (outer = 0). Tongs hooks it: after an outer frame on the main
// thread, while the game is not quitting, it runs the dispatcher's queue
// (docs/design.md, "Dispatcher"). Static findings, checked live in 0.4.0.
struct FrameLayout {
    unsigned rva = 0x14C25A0;
    unsigned frame_counter = 0x320;  // app+0x320: qword, incremented once per frame (0x14C2A23)
    unsigned quit_flag = 0x83;       // app+0x83 and app+0x2F8: bytes the main loop checks to stop
    unsigned quit_flag_2 = 0x2F8;
};

// Installs the hook; returns an error text, or an empty string on success.
std::string InstallFrameHook(const AnvilHost* host, AnvilPlugin* self, const FrameLayout& layout,
                             Dispatcher* dispatcher, FrameStats* stats);

}  // namespace tongs
