// Flags and variables of `country` and `province` (docs/design.md, "Flags and
// variables (0.33.0)"): read through anvil-game.world's 1.20 calls, without
// addresses of Tongs. Only the service table is touched, so the tests hand in a
// table of their own.
#pragma once

#include "game_commands.hpp"
#include "world.hpp"

#include <cstdint>
#include <string_view>

namespace tongs {

// True when the table has the calls each list needs (anvil-game 1.20 and newer).
bool HasFlags(const GameWorldService* world);
bool HasVariables(const GameWorldService* world);
bool HasPending(const GameWorldService* world);  // anvil-game 1.21

// Fill `out` for a country (by tag, as the game stores it) or a province. A list
// the service cannot give keeps `*_read` false with the reason in `*_missing`;
// the read of the country or province itself is not affected. Main thread, after
// an outer frame, in a session, as the service requires.
void ReadCountryExtras(const GameWorldService* world, std::string_view tag, ScopeExtras& out);
void ReadProvinceExtras(const GameWorldService* world, std::int32_t province, ScopeExtras& out);

}  // namespace tongs
