#include "settings.hpp"

#include <anvil/anvil.hpp>

#include "settings.gen.hpp"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace tongs {
namespace {

Config FromAnvil() {
    Config config;
    config.enabled = cfg::enabled.get();
    config.input = cfg::input.get();
    config.game_calls.enabled = cfg::game_calls.get();
    config.game_calls.ironman = cfg::game_calls_ironman.get();
    config.game_calls.multiplayer = cfg::game_calls_multiplayer.get();
    ParseConsoleAllow(cfg::console_allow.get(), config.game_calls.allow, config.notes);
    return config;
}

// Anvil's override for one start: ANVIL_TONGS_<NAME> (docs/design.md of Anvil, §11).
template <class S> bool Overridden(const S&) {
    std::string variable = "ANVIL_TONGS_";
    for (const char* c = S::setting_name; *c != '\0'; ++c) {
        variable += static_cast<char>(*c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c);
    }
    return GetEnvironmentVariableA(variable.c_str(), nullptr, 0) != 0;
}

// The old file's values, except where a variable overrides what Anvil read.
Config Merge(const Config& legacy, const Config& current) {
    Config out = legacy;
    if (Overridden(cfg::enabled)) out.enabled = current.enabled;
    if (Overridden(cfg::input)) out.input = current.input;
    if (Overridden(cfg::game_calls)) out.game_calls.enabled = current.game_calls.enabled;
    if (Overridden(cfg::console_allow)) out.game_calls.allow = current.game_calls.allow;
    if (Overridden(cfg::game_calls_ironman)) out.game_calls.ironman = current.game_calls.ironman;
    if (Overridden(cfg::game_calls_multiplayer)) out.game_calls.multiplayer = current.game_calls.multiplayer;
    out.notes = current.notes;
    for (const std::string& note : legacy.notes) out.notes.push_back("config.toml: " + note);
    return out;
}

// Writes `text` to `file` through a temporary file, so a crash leaves the old state.
bool WriteWhole(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out.flush()) return false;
    }
    return MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace

Config LoadSettings(const std::filesystem::path& data_dir) {
    const anvil::Host host;
    Config current = FromAnvil();
    const std::filesystem::path old_file = data_dir / L"config.toml";
    const std::filesystem::path player_file = data_dir / L"settings.toml";
    std::error_code error;
    if (!std::filesystem::exists(old_file, error)) return current;
    if (std::filesystem::exists(player_file, error)) {
        current.notes.push_back("anvil/data/tongs/config.toml is no longer read: settings.toml took its place; delete it");
        return current;
    }
    std::ifstream in(old_file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    if (!in) {
        current.notes.push_back("anvil/data/tongs/config.toml cannot be read; it is not moved into settings.toml");
        return current;
    }
    const Config legacy = ParseConfig(text.str());
    if (!WriteWhole(player_file, "# Tongs' settings, moved from config.toml; see plugin/settings.toml for the names.\n" +
                                     SettingsFromConfig(legacy))) {
        current.notes.push_back("anvil/data/tongs/settings.toml cannot be written; config.toml is not moved into it");
        return current;
    }
    std::filesystem::path kept = old_file;
    kept += L".migrated";
    std::filesystem::rename(old_file, kept, error);
    host.info("moved anvil/data/tongs/config.toml into settings.toml{}",
              error ? " (config.toml could not be renamed; delete it)" : "; the old file is config.toml.migrated");
    return Merge(legacy, current);
}

}  // namespace tongs
