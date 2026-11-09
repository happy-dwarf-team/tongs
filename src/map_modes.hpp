// The game's map modes by name (map_mode command): the 54 the minimap's tabs
// offer, named after EU4's own enum (EPdxMapColorUpdateMode in GOG's eu4.pdb,
// without MAPMODE_, lower case).
#pragma once

#include <cstdint>
#include <string_view>

namespace tongs {

// The number of a map mode on the game's tabs by its name; -1 when none has it.
std::int32_t GameMapModeByName(std::string_view name);
// The name of a map mode on the game's tabs; nullptr for other numbers.
const char* GameMapModeName(std::int32_t number);

}  // namespace tongs
