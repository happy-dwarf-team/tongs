// anvil-game.commands as far as Tongs uses it: version 2's event option
// (include/anvil/anvil_game.h of Anvil 1.17; Tongs builds against an SDK that
// may predate it). Optional: without the service, run_until events=first clicks.
#pragma once

#include "protocol.hpp"

#include <cstdint>

struct AnvilPlugin;

namespace tongs {

struct GameCommand {
    std::uint32_t size;
    std::uint32_t type;
    std::uint32_t flags;
    std::uint32_t count;
    const void* units;
    std::int32_t pending_event_id;
    std::int32_t option_index;
};

struct GameCommandResult {
    std::uint32_t size;
    std::uint32_t posted;
    std::uint32_t skipped;
    std::uint32_t refused;
};

struct GameCommandsService {
    std::uint32_t size;
    int (*post)(AnvilPlugin* self, const GameCommand* command, GameCommandResult* out);
};

constexpr const char* kGameCommandsService = "anvil-game.commands";
constexpr std::uint32_t kGameCommandsVersion = 2;  // the first with the event option
constexpr std::uint32_t kEventOptionCommand = 4;  // ANVIL_GAME_COMMAND_EVENT_OPTION

// The service's answer for `option` of the pending event `pending`. Main thread,
// after an outer frame, as the service requires.
ServiceChoice ChooseEventOption(const GameCommandsService* service, AnvilPlugin* self, std::int32_t pending,
                                std::int32_t option);

}  // namespace tongs
