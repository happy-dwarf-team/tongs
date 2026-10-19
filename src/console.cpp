#include "console.hpp"

namespace tongs {
namespace {

constexpr std::size_t kMaxLine = 1024;
constexpr std::size_t kMaxTokens = 32;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) return false;
    }
    return true;
}

bool StartsNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && EqualNoCase(text.substr(0, prefix.size()), prefix);
}

// ADR 0001: never run, whatever the config says. enable_all_commands opens the
// game's developer and multiplayer/ironman gates; the others act on the
// user's account outside the game. social_* is matched as a prefix.
constexpr std::string_view kAlwaysRefused[] = {
    "enable_all_commands", "debug_achievements_clear", "test_achievement", "debug_achievements",
    "CrashReporter.UploadCrash",
};

// The `danger` class of the research table (crashes, leaks, desync, rewriting
// the map or the world, install and save rules), the engine's simulated crash,
// and the two commands that run a file of console commands, which would pass
// lines past these refusals.
constexpr std::string_view kDanger[] = {
    "oos", "startup_screen", "reinitchat", "disable_dlc", "spawn_all_units", "bearhaslanded", "leak", "leak_gpu",
    "low_memory", "map_random", "debug_crash", "set_achievements_ok", "purge_mem", "deiron", "syntheticdawn",
    "CrashReporter.SimulateCrash", "run_commands", "runyear",
};

}  // namespace

ConsoleClass ClassifyConsoleCommand(std::string_view name) {
    if (StartsNoCase(name, "social_")) return ConsoleClass::AlwaysRefused;
    for (const std::string_view refused : kAlwaysRefused) {
        if (EqualNoCase(name, refused)) return ConsoleClass::AlwaysRefused;
    }
    for (const std::string_view danger : kDanger) {
        if (EqualNoCase(name, danger)) return ConsoleClass::Danger;
    }
    return ConsoleClass::Normal;
}

std::string SplitConsoleLine(std::string_view line, std::vector<std::string>& tokens) {
    tokens.clear();
    if (line.size() > kMaxLine) return "the line is longer than " + std::to_string(kMaxLine) + " characters";
    for (const char c : line) {
        const auto u = static_cast<unsigned char>(c);
        if (u != '\t' && (u < 0x20 || u > 0x7E)) return "only printable ASCII is accepted";
    }
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) tokens.emplace_back(line.substr(start, i - start));
    }
    if (tokens.empty()) return "usage: console COMMAND [ARGUMENTS]";
    if (tokens.size() > kMaxTokens) return "more than " + std::to_string(kMaxTokens) + " words";
    return {};
}

const ConsoleEntry* FindConsoleEntry(const std::vector<ConsoleEntry>& entries, std::string_view token) {
    for (const ConsoleEntry& entry : entries) {
        if (EqualNoCase(entry.name, token)) return &entry;
        for (const std::string& alias : entry.aliases) {
            if (EqualNoCase(alias, token)) return &entry;
        }
    }
    return nullptr;
}

ConsoleRefusal CheckConsoleName(const GameCallPolicy& policy, std::string_view name) {
    switch (ClassifyConsoleCommand(name)) {
        case ConsoleClass::AlwaysRefused:
            return {"refused", std::string(name) + " is always refused (ADR 0001)"};
        case ConsoleClass::Danger:
            for (const std::string& allowed : policy.allow) {
                if (EqualNoCase(allowed, name)) return {};
            }
            return {"refused", std::string(name) + " is a danger command; console_allow = [\"" + std::string(name) +
                                   "\"] in the config allows it"};
        case ConsoleClass::Normal:
            break;
    }
    return {};
}

ConsoleRefusal CheckConsoleMode(const GameCallPolicy& policy, const ConsoleRaw& raw) {
    if (raw.ironman && !policy.ironman) {
        return {"ironman", "game calls are refused in ironman (game_calls_ironman = true lifts this)"};
    }
    if (raw.multiplayer && !policy.multiplayer) {
        return {"multiplayer", "game calls are refused in multiplayer (game_calls_multiplayer = true lifts this)"};
    }
    return {};
}

std::vector<std::string> CheckConsoleAllow(const std::vector<std::string>& allow) {
    std::vector<std::string> notes;
    for (const std::string& name : allow) {
        const ConsoleClass c = ClassifyConsoleCommand(name);
        if (c == ConsoleClass::AlwaysRefused) {
            notes.push_back("console_allow: " + name + " is always refused; ignored");
        } else if (c == ConsoleClass::Normal) {
            notes.push_back("console_allow: " + name + " is not a danger command; it needs no entry");
        }
    }
    return notes;
}

}  // namespace tongs
