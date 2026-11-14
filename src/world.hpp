#pragma once

#include "ui_tree.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// World reads (docs/design.md, "World reads (0.20.0)"): one province, one country and
// the player through anvil-game.world (world_reads.cpp), and the province
// selection from the game's memory (game_world.cpp), read on the main thread
// after an outer frame. Nothing here touches game memory, so the tests fill
// the raw structs by hand. Tags are decoded by the reader: three characters as
// the game stores them, or empty for no country (`---`).

// Why a world read gave no answer: `error` is the code ("unknown_province",
// "unknown_country", "not_in_game", "read_failed", "unavailable",
// "bad_command"), `detail` a sentence for people.
struct WorldFailure {
    std::string error;
    std::string detail;
    bool failed() const { return !error.empty(); }
};

// A flag of a country or province, with the date it was set (anvil-game.world, Anvil 1.20).
struct FlagRaw {
    std::string name;
    std::int32_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;
};

// A variable of a country or province: its name and its value x 1000.
struct VariableRaw {
    std::string name;
    std::int32_t value = 0;
};

// An event waiting for a country's choice (anvil-game.world, Anvil 1.21).
struct PendingRaw {
    std::int32_t id = 0;     // the pending id the event window and `event choose` know
    std::string key;         // "flavor_tur.203"
    std::string title;       // its title's localisation key
    std::int32_t province = 0;  // 0: a country event
    std::uint32_t options = 0;
    bool hidden = false;
    std::int32_t year = 0;   // the game chooses for the player the day after
    std::uint32_t month = 0;
    std::uint32_t day = 0;
};

// A relation of a country, as the game's save writes it (anvil-game.world, Anvil 1.22).
struct RelationRaw {
    std::string first;
    std::string second;
    bool has_start = false, has_end = false;
    std::int32_t start_year = 0, end_year = 0;
    std::uint32_t start_month = 0, start_day = 0, end_month = 0, end_day = 0;
    std::string subject_type;  // a dependency the country is first in: "vassal"
};

// What one country keeps about another, with the opinion from the game's cache.
struct StatusRaw {
    bool read = false;  // false: the service answered no (a country without provinces, no cache entry yet)
    std::int32_t opinion = 0;
    std::int32_t timed_sum = 0;
    std::uint32_t trust = 0;
    std::int32_t favors = 0;  // x 1000
    bool truce = false;
};

// A country's diplomacy (Anvil 1.22): its relations by kind, rivals, overlord,
// subjects and the status toward the player and back.
struct DiplomacyRaw {
    std::vector<RelationRaw> relations[8];  // by ANVIL_GAME_RELATION_*
    std::vector<std::string> rivals;
    std::string overlord;  // empty: none
    std::vector<std::string> subjects;
    std::string player;    // empty: no player, or the country is the player's
    StatusRaw toward_player;
    StatusRaw player_toward;
};

// Flags and variables of one scope, read through anvil-game.world (world_flags.cpp),
// and for a country (`country` set) its pending events. When a list could not be
// read, `*_missing` says why and the answer has `null` for it, never an empty list.
struct ScopeExtras {
    bool country = false;
    bool pending_read = false;
    std::vector<PendingRaw> pending;
    std::string pending_missing;
    bool diplomacy_read = false;
    DiplomacyRaw diplomacy;
    std::string diplomacy_missing;
    bool flags_read = false;
    std::vector<FlagRaw> flags;
    std::string flags_missing;
    bool variables_read = false;
    std::vector<VariableRaw> variables;
    std::string variables_missing;
};

struct ProvinceRaw {
    std::int32_t id = 0;
    std::string name;  // UTF-8, the name the game displays (it follows the owner's culture)
    std::string owner;       // empty: no country
    std::string controller;  // empty: no country
    std::vector<std::string> cores;
    std::int32_t base_tax = 0;  // as stored: the value x 1000, without the capital bonus
    std::int32_t base_production = 0;
    std::int32_t base_manpower = 0;
    bool land = false;  // false for seas and lakes
    bool wasteland = false;
    ScopeExtras extras;
};

struct CountryRaw {
    std::string tag;
    bool observer = false;            // the kind byte of its tag is the observer's
    std::int32_t province_count = 0;  // > 0: the country exists on the map
    std::vector<std::int32_t> provinces;  // the IDs it owns, in the game's order
    std::int32_t capital = 0;        // as stored
    bool capital_valid = false;      // 1 <= capital < the province count
    ScopeExtras extras;
};

struct PlayerRaw {
    std::string tag;           // the country the game treats as the player's (observe mode included)
    std::string player_tag;    // the player's own tag as the game stores it (AnvilGamePlayer.own)
    std::string observed_tag;  // the observed country (AnvilGamePlayer.observed); empty: none
    bool observer = false;     // the kind byte of player_tag is the observer's
};

struct SelectionRaw {
    std::int32_t last_selected = 0;    // the province the selection holds; 0: none
    std::int32_t view_province = 0;    // the province the province view shows; 0: none
    // The widgets that can show the province, matched against the widget tree by
    // pointer; 0: none. The view's window, and the colonisation panel the game
    // shows instead for a province without an owner.
    std::uintptr_t view_widget = 0;
    std::uintptr_t colonisation_widget = 0;
};

// `province ID`, `country TAG`, `player`, `selection`, and the navigation
// `province open ID` / `province goto ID` (navigation.hpp), each with an optional `timeout=S`.
struct WorldRequest {
    enum class Kind { Province, Country, Player, Selection, Open, Goto };
    Kind kind = Kind::Province;
    std::int32_t id = 0;
    std::string tag;     // upper case
    int timeout_ms = 0;  // 0: the dispatcher's default
};

constexpr int kWorldMaxTimeoutS = 60;

// Parses the words after the command word. A province ID is a decimal number
// without sign or leading zeros; a tag three characters [A-Za-z0-9]. A word as
// the second token of `province` is a subcommand: `open` or `goto`.
WorldFailure ParseWorldRequest(std::string_view command, std::string_view argument, WorldRequest& out);

// `,"flags":[...],"variables":[...]` (and `pending_events` and `diplomacy` for a country), with
// `null` for a list that was not read and then `,"missing":{"flags":"why",...}` naming
// each such list.
std::string ExtrasMembers(const ScopeExtras& e);

// The answer members of each read, each starting with a comma.
std::string ProvinceMembers(const ProvinceRaw& p);
std::string CountryMembers(const CountryRaw& c);
std::string PlayerMembers(const PlayerRaw& p);
// The root in `tree` that shows the selection's province: the visible one of
// the view's window and the colonisation panel, else the view's window when
// the tree has it; -1 when the tree has neither.
int ViewRoot(const SelectionRaw& s, const UiTree& tree);

// `visible` is the effective visibility of ViewRoot, `path` its widget path while it is visible.
std::string SelectionMembers(const SelectionRaw& s, const UiTree& tree);

// A stored value x 1000 as a JSON number: 4000 -> 4, 4500 -> 4.5.
std::string Thousandths(std::int64_t raw);

}  // namespace tongs
