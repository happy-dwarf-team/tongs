// anvil-game.commands as far as Tongs uses it: version 2's event option and
// version 1's unit commands, and anvil-game.world's units, declared by the
// SDK's <anvil/anvil_game.h>. Optional: without the commands service,
// run_until events=first clicks and the unit commands answer unavailable.
#pragma once

#include "protocol.hpp"
#include "units.hpp"

#include <anvil/anvil_game.h>

#include <cstdint>

namespace tongs {

// The versions Tongs asks anvil-game.commands for.
constexpr std::uint32_t kGameCommandsVersion = 2;       // the first with the event option
constexpr std::uint32_t kGameCommandsVersionUnits = 1;  // the unit commands

// True when the table has every member Tongs calls (anvil-game 1.15 and newer).
bool HasUnits(const AnvilGameWorldService* world);

// The armies and navies of `tag` (empty: the player's). Main thread, after an
// outer frame, in a session.
WorldFailure ReadUnits(const AnvilGameWorldService* world, std::string_view tag, UnitsRaw& out);

// Posts `request` through the commands service: the units are the player's, by id, or
// the player's selection. Main thread, after an outer frame, as the service requires.
UnitPost PostUnits(const AnvilGameCommandsService* commands, const AnvilGameWorldService* world, AnvilPlugin* self,
                   const UnitRequest& request);

// The service's answer for `option` of the pending event `pending`. Main thread,
// after an outer frame, as the service requires.
ServiceChoice ChooseEventOption(const AnvilGameCommandsService* service, AnvilPlugin* self, std::int32_t pending,
                                std::int32_t option);

}  // namespace tongs
