#include "map_modes.hpp"

#include <array>

namespace tongs {
namespace {

struct Named {
    std::int32_t number;
    const char* name;
};

// The modes of the four tabs (anvil-map's tab_1..tab_4 arrays), 14 + 13 + 16 + 11.
constexpr std::array<Named, 54> kModes = {{
    {1, "political"}, {3, "empire"}, {74, "religious_leagues"}, {11, "culture"}, {19, "acceptedcultures"},
    {2, "religion"}, {64, "patriarchs"}, {12, "unrest"}, {31, "government"}, {32, "rebeltypes"},
    {36, "states_edicts"}, {33, "dynasty"}, {69, "revolution"}, {79, "parliament_seats"},
    {0, "terrain"}, {26, "simple_terrain"}, {27, "areas"}, {10, "region"}, {67, "super_region"},
    {35, "states_territories"}, {30, "overseas"}, {22, "colonialandtradecompanyregions"}, {16, "supply_limit"},
    {25, "fort_level"}, {70, "great_projects"}, {20, "weather"}, {21, "climate"},
    {4, "trade"}, {54, "development"}, {17, "buildings"}, {55, "manpower"}, {24, "local_autonomy"},
    {14, "colonial"}, {23, "trade_goods"}, {15, "tech_cost"}, {29, "institutions"}, {56, "trade_value"},
    {34, "loot"}, {63, "devastation"}, {75, "piracy"}, {76, "razing"}, {77, "scorched_earth"},
    {78, "improved_infrastructure"},
    {5, "diplomacy"}, {6, "military_access"}, {7, "opinion"}, {8, "coalition"}, {9, "truce"},
    {52, "federations"}, {61, "trade_leagues"}, {53, "rivals"}, {18, "players"}, {62, "great_powers"},
    {28, "victorycards"},
}};

}  // namespace

std::int32_t GameMapModeByName(std::string_view name) {
    for (const Named& m : kModes) {
        if (name == m.name) return m.number;
    }
    return -1;
}

const char* GameMapModeName(std::int32_t number) {
    for (const Named& m : kModes) {
        if (m.number == number) return m.name;
    }
    return nullptr;
}

}  // namespace tongs
