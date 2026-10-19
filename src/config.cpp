#include "config.hpp"

namespace tongs {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// ["name", "name"] on one line; names of letters, digits, _ and .
bool ParseNameList(std::string_view value, std::vector<std::string>& out) {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
    value = Trim(value.substr(1, value.size() - 2));
    out.clear();
    while (!value.empty()) {
        if (value.front() != '"') return false;
        const std::size_t close = value.find('"', 1);
        if (close == std::string_view::npos || close == 1) return false;
        const std::string_view name = value.substr(1, close - 1);
        if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") != std::string_view::npos) {
            return false;
        }
        out.emplace_back(name);
        value = Trim(value.substr(close + 1));
        if (value.empty()) break;
        if (value.front() != ',') return false;
        value = Trim(value.substr(1));
    }
    return true;
}

}  // namespace

const char kDefaultConfig[] =
    "# Tongs\n"
    "# enabled = true opens the local pipe \\\\.\\pipe\\tongs for the current Windows user.\n"
    "enabled = false\n"
    "# input = true also accepts the keyboard and mouse commands (pause, speed,\n"
    "# run_until, key, text, click...): input queued in the game's own SDL input,\n"
    "# as a player's would be, and a hook so the interface sees the mouse pointer.\n"
    "input = false\n"
    "# game_calls = true also accepts `console LINE`: console commands run by the\n"
    "# game's own code on its main thread. They CHANGE GAME STATE; every call is\n"
    "# counted and logged. enable_all_commands and the achievement and social\n"
    "# commands are always refused; the other danger commands run only when listed\n"
    "# in console_allow, e.g. console_allow = [\"leak\"]. Ironman and multiplayer\n"
    "# games refuse every game call unless game_calls_ironman or\n"
    "# game_calls_multiplayer is true.\n"
    "game_calls = false\n"
    "console_allow = []\n"
    "game_calls_ironman = false\n"
    "game_calls_multiplayer = false\n";

Config ParseConfig(std::string_view text) {
    Config config;
    int line_number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++line_number;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            config.notes.push_back("config line " + std::to_string(line_number) + ": expected key = value");
            continue;
        }
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));
        bool* flag = key == "enabled"                  ? &config.enabled
                     : key == "input"                  ? &config.input
                     : key == "game_calls"             ? &config.game_calls.enabled
                     : key == "game_calls_ironman"     ? &config.game_calls.ironman
                     : key == "game_calls_multiplayer" ? &config.game_calls.multiplayer
                                                       : nullptr;
        if (flag) {
            bool& target = *flag;
            if (value == "true") {
                target = true;
            } else if (value == "false") {
                target = false;
            } else {
                config.notes.push_back("config line " + std::to_string(line_number) + ": " + std::string(key) +
                                       " must be true or false");
            }
        } else if (key == "console_allow") {
            if (!ParseNameList(value, config.game_calls.allow)) {
                config.game_calls.allow.clear();
                config.notes.push_back("config line " + std::to_string(line_number) +
                                       ": console_allow must be a list of quoted command names, e.g. [\"leak\"]");
            }
            for (const std::string& note : CheckConsoleAllow(config.game_calls.allow)) config.notes.push_back(note);
        } else {
            config.notes.push_back("config line " + std::to_string(line_number) + ": unknown key '" +
                                   std::string(key) + "' ignored");
        }
    }
    return config;
}

}  // namespace tongs
