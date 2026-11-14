#pragma once

#include "config.hpp"

#include <filesystem>

namespace tongs {

// Tongs' switches as Anvil read them from plugin/settings.toml, the player's
// anvil/data/tongs/settings.toml and ANVIL_TONGS_<NAME> variables. Call while
// loading.
//
// The first start after an update from Tongs 0.12 or older finds the old
// anvil/data/tongs/config.toml and no settings.toml: it writes the config's
// values to settings.toml, renames config.toml to config.toml.migrated, and
// uses those values already in this session (Anvil read settings.toml before
// it existed), except where an ANVIL_TONGS_<NAME> variable overrides one.
// Game calls are the exception: config.toml's game_calls, game_calls_ironman
// and game_calls_multiplayer go to settings.toml as the retired settings of
// 0.34, and Anvil moves them into the game_calls module at the next start.
// `game_calls.enabled` stays false here; plugin.cpp sets it from the module.
Config LoadSettings(const std::filesystem::path& data_dir);

}  // namespace tongs
