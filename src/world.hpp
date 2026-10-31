#pragma once

#include "ui_tree.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// World reads (docs/design.md, "World reads (0.20.0)"): one province, one
// country, the player and the province selection, read on the main thread
// after an outer frame (game_world.cpp). Nothing here touches game memory, so
// the tests fill the raw structs by hand. Tags are decoded by the reader:
// three characters as the game stores them, or empty for no country (`---`).

// Why a world read gave no answer: `error` is the code ("unknown_province",
// "unknown_country", "not_in_game", "read_failed", "unavailable",
// "bad_command"), `detail` a sentence for people.
struct WorldFailure {
    std::string error;
    std::string detail;
    bool failed() const { return !error.empty(); }
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
};

struct CountryRaw {
    std::string tag;
    bool observer = false;            // the kind byte of its tag is the observer's
    std::int32_t province_count = 0;  // > 0: the country exists on the map
    std::vector<std::int32_t> provinces;  // the IDs it owns, in the game's order
    std::int32_t capital = 0;        // as stored
    bool capital_valid = false;      // 1 <= capital < the province count
};

struct PlayerRaw {
    std::string tag;           // the country the game treats as the player's (observe mode included)
    std::string player_tag;    // root+0x1E60 as stored
    std::string observed_tag;  // root+0x1E68; empty: no country
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
constexpr int kTagBuckets = 64;  // the game's tag buckets: tag_buckets.bucket_mask + 1

// Parses the words after the command word. A province ID is a decimal number
// without sign or leading zeros; a tag three characters [A-Za-z0-9]. A word as
// the second token of `province` is a subcommand: `open` or `goto`.
WorldFailure ParseWorldRequest(std::string_view command, std::string_view argument, WorldRequest& out);

// The bucket the game files a tag in: its three characters, upper-cased, summed, & mask.
int TagBucket(std::string_view tag, int mask);

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
