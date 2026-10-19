#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// Console commands as game calls (docs/design.md, "Game calls (0.7.0)", and
// docs/adr/0001-game-calls-in-tongs.md). This part decides; game_console.cpp
// only reads the console manager and makes the call, so the tests build the
// registry by hand.

// The config switches of ADR 0001.
struct GameCallPolicy {
    bool enabled = false;      // game_calls = true
    bool ironman = false;      // game_calls_ironman = true lifts the ironman refusal
    bool multiplayer = false;  // game_calls_multiplayer = true lifts the multiplayer refusal
    std::vector<std::string> allow;  // console_allow: danger commands allowed by name
};

// One command of the game's console manager: its name and aliases, as the
// game's lookup (0x1721000) compares them.
struct ConsoleEntry {
    std::string name;
    std::vector<std::string> aliases;
};

// What the main thread read before a call (game_console.cpp).
struct ConsoleRaw {
    bool manager = false;          // the manager pointer at 0x2357B58 is set
    std::vector<ConsoleEntry> entries;  // in the manager's order; empty before the first registration
    std::uint8_t developer_lock = 0;    // manager+0x18: developer commands locked
    std::uint8_t mode_lock = 0;         // manager+0x19: multiplayer/ironman lock, set at registration
    bool ironman = false;          // root byte +0x24D0
    bool multiplayer = false;      // 0x6EAA10(session)
    std::uintptr_t session = 0;    // root+0x1E00, to tell loaded saves apart
};

// What the call returned (game_console.cpp).
struct ConsoleCallResult {
    bool faulted = false;
    std::uint32_t fault_code = 0;
    bool success = false;   // result+0: a hint only; `speed` never sets it
    std::string text;       // result+8, game text (Windows-1252)
};

enum class ConsoleClass {
    Normal,
    Danger,          // refused unless console_allow names it
    AlwaysRefused,   // refused whatever the config says
};

// By the command's own name (not an alias), ASCII case ignored.
ConsoleClass ClassifyConsoleCommand(std::string_view name);

// Splits a console line on runs of spaces and tabs. The game keeps empty
// tokens for double spaces; Tongs does not pass any. Returns an error text, or
// empty. Only printable ASCII is accepted: the game reads Windows-1252.
std::string SplitConsoleLine(std::string_view line, std::vector<std::string>& tokens);

// The first entry whose name or one of its aliases equals `token` (ASCII
// case ignored), as the game's lookup finds it; nullptr when there is none.
const ConsoleEntry* FindConsoleEntry(const std::vector<ConsoleEntry>& entries, std::string_view token);

// A refusal: `error` is the code of the answer, empty when the call may go on.
struct ConsoleRefusal {
    std::string error;
    std::string detail;

    bool refused() const { return !error.empty(); }
};

// Refusals by name: always refused, and danger commands not in console_allow.
ConsoleRefusal CheckConsoleName(const GameCallPolicy& policy, std::string_view name);

// Refusals by the game's mode, checked before each call on the main thread.
ConsoleRefusal CheckConsoleMode(const GameCallPolicy& policy, const ConsoleRaw& raw);

// Notes for the log about console_allow entries that cannot take effect.
std::vector<std::string> CheckConsoleAllow(const std::vector<std::string>& allow);

}  // namespace tongs
