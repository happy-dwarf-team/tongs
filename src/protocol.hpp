#pragma once

#include "keys.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tongs {

// One snapshot of what the plugin can read from the game (game_state.cpp).
struct StateSnapshot {
    bool in_game = false;        // a game is loaded (root object and session present)
    const char* reason = nullptr;  // why in_game is false, or why a read failed
    std::int32_t date_raw = 0;
    std::int32_t speed = 0;      // game value 0..4; the UI shows speed + 1
    bool paused = false;         // the game's own "is paused" answer (0x7E01B0)
    bool paused_flag = false;    // the player pause byte (+0x48) alone
};

// Protocol: UTF-8 lines. A request is one command line; the answer is one
// JSON object on one line. Commands: ping, state, help, and with input
// enabled key, pause, resume, speed. Input commands answer with the state
// read after the action.
struct Handlers {
    std::function<StateSnapshot()> read_state;
    // Queues a key press (down, character, up) for the game; returns an error text, or
    // an empty string on success. Left empty when input is disabled.
    std::function<std::string(const KeyPress&)> press_key;
    std::function<void(int ms)> sleep_ms;
    std::function<std::uint64_t()> now_ms;  // monotonic clock
    std::uint32_t process_id = 0;
    // How long pause/resume/speed wait for the game to react. Right after a save
    // loads, the main loop handled a key press only 2-7 s later.
    int wait_timeout_ms = 10000;
    int poll_interval_ms = 10;
    int key_settle_ms = 100;     // `key` waits this long before reading the state
};

std::string HandleRequest(std::string_view line, const Handlers& handlers);

std::string JsonEscape(std::string_view text);

}  // namespace tongs
