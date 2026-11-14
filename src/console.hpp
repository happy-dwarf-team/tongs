#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// Console commands as game calls (docs/design.md, "Game calls (0.7.0)", and
// docs/adr/0001-game-calls-in-tongs.md). This part decides; game_console.cpp
// only reads the console manager and makes the call, so the tests build the
// registry by hand.

// What Anvil said about the game_calls module in the session in progress
// (Anvil ADR 0011 and 0013): whether the player allows it in ironman and
// multiplayer is Anvil's, and Tongs learns it only from its gameplay policy.
// The policy hears of a session before any plugin and suspends the module
// there when the player did not allow it, so a session Tongs heard of with the
// module not suspended is one the player allowed game calls in.
struct GameCallSession {
    bool started = false;      // anvil-game.session started (and the policy has judged it)
    bool ironman = false;      // that session's mode
    bool multiplayer = false;
    bool suspended = false;    // anvil.suspended reached the module's handle

    // The module acts in the session in progress, in a mode Anvil judged.
    bool Acts() const { return started && !suspended; }
};

// Keeps GameCallSession from the module's events: anvil.suspended and
// anvil.resumed through the module's handle, anvil-game.session through the
// plugin's. Any thread may read it while the main thread updates it.
class GameCallSessionWatch {
public:
    void Suspended(bool suspended);
    void Session(bool started, bool ironman, bool multiplayer);
    GameCallSession Read() const;

private:
    std::atomic<std::uint32_t> bits_{0};
};

// ADR 0001's rules of game calls. The switch and the opt-ins are the
// game_calls module's (plugin.toml); console_allow is its setting.
struct GameCallPolicy {
    bool enabled = false;  // the game_calls module is on at this start (Anvil gave its handle)
    // The module in the session in progress; null: never judged, so game calls
    // are refused in every ironman and multiplayer game.
    std::function<GameCallSession()> session;
    std::vector<std::string> allow;  // console_allow: danger commands allowed by name

    GameCallSession Session() const { return session ? session() : GameCallSession{}; }
    // Anvil lets the module act in the ironman (multiplayer) session in progress.
    bool ActsInIronman() const;
    bool ActsInMultiplayer() const;
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

// Refusals by the game's mode, checked before each call on the main thread:
// in ironman (multiplayer) unless Anvil lets the game_calls module act in that
// session, which needs allow_ironman (allow_multiplayer) = ["tongs.game_calls"].
ConsoleRefusal CheckConsoleMode(const GameCallPolicy& policy, const ConsoleRaw& raw);

// Notes for the log about console_allow entries that cannot take effect.
std::vector<std::string> CheckConsoleAllow(const std::vector<std::string>& allow);

}  // namespace tongs
