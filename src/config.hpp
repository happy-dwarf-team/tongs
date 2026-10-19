#pragma once

#include "console.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// Tongs' switches (plugin/settings.toml): the pipe stays closed unless
// `enabled`, the input commands (key, pause, resume, speed) answer an error
// unless `input`, and game calls (console) unless `game_calls` (ADR 0001).
struct Config {
    bool enabled = false;
    bool input = false;
    GameCallPolicy game_calls;  // game_calls, game_calls_ironman, game_calls_multiplayer, console_allow
    std::vector<std::string> notes;  // problems found while reading, for the log
};

// console_allow as settings.toml holds it: command names separated by commas
// or white space. Unknown or always-refused names are notes, as in the 0.12
// config. Returns false (and an empty list) when a name has a character
// outside letters, digits, _ and .
bool ParseConsoleAllow(std::string_view text, std::vector<std::string>& out, std::vector<std::string>& notes);

// --- anvil/data/tongs/config.toml of Tongs 0.12 and older -------------------
// Only flat `key = value` lines; read once, to move it into the player's
// settings.toml (anvil/data/tongs/settings.toml).
Config ParseConfig(std::string_view text);

// The player's settings.toml for a parsed config.toml: the values that differ
// from the defaults of plugin/settings.toml, as `name = value` lines (empty
// when every value is a default).
std::string SettingsFromConfig(const Config& config);

}  // namespace tongs
