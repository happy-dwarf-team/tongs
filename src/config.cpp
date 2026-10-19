#include "config.hpp"

namespace tongs {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool IsNameCharacter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
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
        for (const char c : name) {
            if (!IsNameCharacter(c)) return false;
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

bool ParseConsoleAllow(std::string_view text, std::vector<std::string>& out, std::vector<std::string>& notes) {
    out.clear();
    std::size_t at = 0;
    while (at < text.size()) {
        const char c = text[at];
        if (c == ',' || c == ' ' || c == '\t') {
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < text.size() && text[end] != ',' && text[end] != ' ' && text[end] != '\t') ++end;
        const std::string_view name = text.substr(at, end - at);
        for (const char n : name) {
            if (!IsNameCharacter(n)) {
                out.clear();
                notes.push_back("console_allow must be command names separated by commas or spaces, e.g. \"leak\"; "
                                "ignored");
                return false;
            }
        }
        out.emplace_back(name);
        at = end;
    }
    for (const std::string& note : CheckConsoleAllow(out)) notes.push_back(note);
    return true;
}

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

std::string SettingsFromConfig(const Config& config) {
    std::string out;
    const auto flag = [&out](const char* name, bool value) {
        if (value) out += std::string(name) + " = true\n";  // every default is false
    };
    flag("enabled", config.enabled);
    flag("input", config.input);
    flag("game_calls", config.game_calls.enabled);
    if (!config.game_calls.allow.empty()) {
        std::string names;
        for (const std::string& name : config.game_calls.allow) names += (names.empty() ? "" : ", ") + name;
        out += "console_allow = \"" + names + "\"\n";  // names hold no quote or backslash
    }
    flag("game_calls_ironman", config.game_calls.ironman);
    flag("game_calls_multiplayer", config.game_calls.multiplayer);
    return out;
}

}  // namespace tongs
