// `cursor`: what the game has under its pointer (docs/commands.md, "Cursor").
#pragma once

#include "units.hpp"
#include "world.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tongs {

// What the game's map picks at a client point (game_map.cpp fills it).
struct MapPickRaw {
    std::int32_t province = -1;   // the province under the point, -1 none (off the map)
    std::int32_t pick_kind = -1;  // the picked object's kind (0 army, 1 navy, 2 province), -1 none
    std::uint64_t unit_id = 0;    // army or navy: its id, as anvil-game.world gives it
    char unit_tag[4] = {};        // army or navy: its country's tag
};

// Everything `cursor` answers, read on one frame.
struct CursorRaw {
    int x = 0;  // the point the interface sees, in client pixels
    int y = 0;
    bool tongs_pointer = false;  // Tongs' virtual pointer (a mouse command), else the real mouse
    bool inside = false;         // the point lies in the client area
    std::uintptr_t hovered = 0;  // the widget the game hovers, 0 none
    std::string widget;          // its path; empty when none or when the tree does not have it
    bool map_read = false;       // the map was asked (in a game, with a map)
    MapPickRaw pick;
    bool province_read = false;  // `province` holds the picked province's facts
    ProvinceRaw province;
    bool unit_found = false;  // the picked army or navy is in anvil-game.world's units of its country
    UnitInfo unit;
    std::string unit_tag;
};

// `cursor [timeout=S]`; a bad option fails like the world reads.
WorldFailure ParseCursorRequest(std::string_view argument, int& timeout_ms);

// The members after "command": point, pointer, inside, widget, over_map, province, unit.
std::string CursorMembers(const CursorRaw& c);

}  // namespace tongs
