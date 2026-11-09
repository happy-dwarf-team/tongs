#include "game_commands.hpp"

namespace tongs {
namespace {

constexpr int kNotDeclared = 2;   // ANVIL_ERROR_NOT_DECLARED: this build lacks the type's symbols
constexpr int kUnsupported = 9;   // ANVIL_ERROR_UNSUPPORTED

}  // namespace

ServiceChoice ChooseEventOption(const GameCommandsService* service, AnvilPlugin* self, std::int32_t pending,
                                std::int32_t option) {
    ServiceChoice out;
    if (service == nullptr || service->post == nullptr) return out;
    GameCommand command{};
    command.size = sizeof(command);
    command.type = kEventOptionCommand;
    command.pending_event_id = pending;
    command.option_index = option;
    GameCommandResult result{};
    result.size = sizeof(result);
    out.result = service->post(self, &command, &result);
    out.available = out.result != kNotDeclared && out.result != kUnsupported;
    out.posted = result.posted;
    out.skipped = result.skipped;
    out.refused = result.refused;
    return out;
}

}  // namespace tongs
