#pragma once

#include "console.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// Tongs' switches (plugin/settings.toml): the pipe stays closed unless
// `enabled`, the input commands (key, pause, resume, speed) answer an error
// unless `input`, and game calls (console) unless the game_calls module is on
// (ADR 0001; the module's switch and opt-ins are Anvil's, plugin/plugin.toml).
struct Config {
    bool enabled = false;
    bool input = false;
    bool journal = true;
    bool bridge = false;        // also serve the pipe's commands on a loopback socket, under Wine only
    std::string bridge_folder;  // where its bridge.json goes; empty: ~/.local/state/tongs
    GameCallPolicy game_calls;  // the game_calls module and its console_allow
    // config.toml of 0.12 and older only: written to settings.toml as the retired settings
    // game_calls_ironman / game_calls_multiplayer, which Anvil moves into the module's opt-ins.
    bool legacy_game_calls_ironman = false;
    bool legacy_game_calls_multiplayer = false;
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
// when every value is a default). Game calls are written as the settings of
// 0.34 (game_calls, game_calls_ironman, game_calls_multiplayer), which Anvil
// retires into the game_calls module at the next start.
std::string SettingsFromConfig(const Config& config);

}  // namespace tongs
