#pragma once

#include "console.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// anvil/data/tongs/config.toml. Only flat `key = value` lines are
// understood; the pipe stays closed unless `enabled = true`, and the input
// commands (key, pause, resume, speed) answer an error unless `input = true`,
// and game calls (console) unless `game_calls = true` (ADR 0001).
struct Config {
    bool enabled = false;
    bool input = false;
    GameCallPolicy game_calls;  // game_calls, game_calls_ironman, game_calls_multiplayer, console_allow
    std::vector<std::string> notes;  // problems found while parsing, for the log
};

Config ParseConfig(std::string_view text);

extern const char kDefaultConfig[];

}  // namespace tongs
