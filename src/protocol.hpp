#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace lc {

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
// JSON object on one line. Commands: ping, state, help.
struct Handlers {
    std::function<StateSnapshot()> read_state;
    std::uint32_t process_id = 0;
};

std::string HandleRequest(std::string_view line, const Handlers& handlers);

std::string JsonEscape(std::string_view text);

}  // namespace lc
