#pragma once

#include "world.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// Units (docs/design.md, "Units (0.31.0)"): `units [TAG]` reads a country's armies
// and navies through anvil-game.world, and `unit drill|split|merge` posts the
// commands their buttons post through anvil-game.commands (Anvil 1.17 and newer).
// Nothing here touches game memory or Anvil, so the tests fill the structs by hand
// (game_commands.cpp does the service calls).

constexpr std::uint32_t kUnitArmy = 0;  // ANVIL_GAME_UNIT_ARMY
constexpr std::uint32_t kUnitNavy = 1;  // ANVIL_GAME_UNIT_NAVY
constexpr std::size_t kMaxUnitsPerCommand = 64;  // ANVIL_GAME_COMMAND_MAX_UNITS

struct UnitInfo {
    std::uint64_t id = 0;
    std::uint32_t kind = kUnitArmy;
    std::int32_t location = 0;  // the province it is in or at; 0: none
    std::int32_t mission = -1;  // ANVIL_GAME_MISSION_*: 0 none, 15 drill, -1 unknown
    std::uint32_t subunits = 0;  // regiments or ships
    std::int32_t strength = 0;   // x 1000: men for an army
    std::uint64_t leader = 0;    // 0: no leader
    std::string name;            // UTF-8, as the game shows it
};

struct UnitsRaw {
    std::string tag;  // the country read: as asked, or the player's
    std::vector<UnitInfo> units;
};

// `units [TAG] [timeout=S]`: TAG upper case, empty for the player's country.
struct UnitsRequest {
    std::string tag;
    int timeout_ms = 0;
};

// `unit drill start|stop UNITS`, `unit split UNIT`, `unit merge UNITS`, each with an
// optional `timeout=S`. UNITS are unit ids of the player's country, or the word
// `selected` for the units the player has selected.
struct UnitRequest {
    enum class Verb { Drill, Split, Merge };
    Verb verb = Verb::Drill;
    bool start = false;      // drill: start (or stop)
    bool selected = false;   // the player's selection instead of ids
    std::vector<std::uint64_t> ids;
    int timeout_ms = 0;      // 0: the dispatcher's default
};

// What the post gave. `failure` when nothing reached the service (not in a game, no
// selection, no world service...); otherwise `result` is the AnvilResult of `post`.
struct UnitPost {
    WorldFailure failure;
    int result = 0;
    std::uint32_t units = 0;  // how many units the command named
    std::uint32_t posted = 0, skipped = 0, refused = 0;
};

// Parses the words after `units`; an empty WorldFailure on success.
WorldFailure ParseUnitsRequest(std::string_view argument, UnitsRequest& out);

// Parses the words after `unit`; an empty WorldFailure on success. `command` gets
// "unit drill", "unit split" or "unit merge" (or "unit" for an unknown verb).
WorldFailure ParseUnitRequest(std::string_view argument, UnitRequest& out, std::string& command);

// The error code of an AnvilResult `post` gave: "not_found", "invalid_argument",
// "wrong_phase", "limit", "suspended", "unsupported", "not_declared", "failed".
const char* UnitResultName(int result);

// The JSON members of one unit and of a list of units: `,"tag":...,"units":[...]`.
std::string UnitsJson(const UnitsRaw& raw);

const char* MissionName(std::int32_t mission);

}  // namespace tongs
