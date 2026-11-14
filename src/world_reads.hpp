// The world reads of `province`, `country` and `player` (docs/design.md,
// "Through anvil-game.world"): taken from anvil-game.world, without addresses
// of Tongs. Only the service table is touched, so the tests hand in a table of
// their own.
#pragma once

#include "world.hpp"

#include <anvil/anvil_game.h>

#include <cstdint>
#include <string_view>

namespace tongs {

// Main thread, after an outer frame, in a session, as the service requires:
// from a dispatcher job or another anvil-game handler. The service's answers
// map to Tongs' error names: ANVIL_ERROR_NOT_FOUND to unknown_province (with
// the valid range from province_limit) or unknown_country,
// ANVIL_ERROR_WRONG_PHASE to not_in_game, a missing service or call to
// unavailable, anything else to read_failed.

// The province's own fields only (owner, controller, base values, land, wasteland).
WorldFailure ReadProvinceFields(const AnvilGameWorldService* world, std::int32_t id, ProvinceRaw& out);
// The same, with the name and the cores: the `province` read without its extras.
WorldFailure ReadProvince(const AnvilGameWorldService* world, std::int32_t id, ProvinceRaw& out);
// `tag` as the client wrote it (the service ignores case); `out.tag` is the game's.
WorldFailure ReadCountry(const AnvilGameWorldService* world, std::string_view tag, CountryRaw& out);
WorldFailure ReadPlayer(const AnvilGameWorldService* world, PlayerRaw& out);

}  // namespace tongs
