#include "game_commands.hpp"

#include <cstddef>
#include <algorithm>
#include <cstring>
#include <vector>

namespace tongs {
namespace {

constexpr int kOk = 0;
constexpr int kNotDeclared = 2;   // ANVIL_ERROR_NOT_DECLARED: this build lacks the type's symbols
constexpr int kUnsupported = 9;   // ANVIL_ERROR_UNSUPPORTED
constexpr int kNotFound = 11;     // ANVIL_ERROR_NOT_FOUND
constexpr std::uint32_t kListLimit = 4096;  // more units than any country has

std::string TagText(const GameTag& tag) {
    std::string out;
    for (int i = 0; i < 3 && tag.text[i] != 0; ++i) out += tag.text[i];
    return out;
}

WorldFailure Failed(const char* call, int result) {
    if (result == kNotDeclared) return {"unavailable", std::string(call) + ": this game build has no unit symbols"};
    return {"read_failed", std::string(call) + " answered " + std::to_string(result)};
}

WorldFailure PlayerTag(const GameWorldService* world, std::string& tag) {
    GameTag player{};
    const int r = world->player(&player);
    if (r != kOk) return Failed("player", r);
    tag = TagText(player);
    if (tag.empty()) return {"not_in_game", "no player country"};
    return {};
}

}  // namespace

bool HasUnits(const GameWorldService* world) {
    return world != nullptr && world->size >= offsetof(GameWorldService, selected_units) + sizeof(world->selected_units) &&
           world->player != nullptr && world->country_units != nullptr && world->unit != nullptr &&
           world->unit_name != nullptr && world->selected_units != nullptr;
}

WorldFailure ReadUnits(const GameWorldService* world, std::string_view tag, UnitsRaw& out) {
    out = {};
    if (!HasUnits(world)) return {"unavailable", "anvil-game.world has no units: it needs Anvil 1.15 or newer"};
    if (tag.empty()) {
        if (WorldFailure f = PlayerTag(world, out.tag); f.failed()) return f;
    } else {
        out.tag = std::string(tag);
    }
    std::vector<std::uint64_t> ids(256);
    std::uint32_t count = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int r = world->country_units(out.tag.c_str(), ids.data(), static_cast<std::uint32_t>(ids.size()), &count);
        if (r == kNotFound) return {"unknown_country", "the game has no country " + out.tag};
        if (r != kOk) return Failed("country_units", r);
        if (count <= ids.size() || count > kListLimit) break;
        ids.resize(count);
    }
    ids.resize(std::min<std::size_t>(count, ids.size()));
    for (const std::uint64_t id : ids) {
        GameUnit u{};
        u.size = sizeof(u);
        if (world->unit(out.tag.c_str(), id, &u) != kOk) continue;  // gone between the two calls
        UnitInfo info;
        info.id = id;
        info.kind = u.kind;
        info.location = u.location;
        info.mission = u.mission;
        info.subunits = u.subunits;
        info.strength = u.strength;
        info.leader = u.leader;
        char name[256] = {};
        std::uint32_t length = 0;
        if (world->unit_name(out.tag.c_str(), id, name, sizeof(name), &length) == kOk) {
            info.name.assign(name, strnlen(name, sizeof(name)));
        }
        out.units.push_back(std::move(info));
    }
    return {};
}

UnitPost PostUnits(const GameCommandsService* commands, const GameWorldService* world, AnvilPlugin* self,
                   const UnitRequest& request) {
    UnitPost out;
    if (commands == nullptr || commands->post == nullptr) {
        out.failure = {"unavailable", "anvil-game.commands is missing: the unit commands need Anvil 1.17 or newer"};
        return out;
    }
    if (!HasUnits(world)) {
        out.failure = {"unavailable", "anvil-game.world has no units: it needs Anvil 1.15 or newer"};
        return out;
    }
    std::vector<GameUnitRef> refs;
    if (request.selected) {
        refs.resize(kMaxUnitsPerCommand + 1);
        std::uint32_t count = 0;
        const int r = world->selected_units(refs.data(), static_cast<std::uint32_t>(refs.size()), &count);
        if (r != kOk) {
            out.failure = Failed("selected_units", r);
            return out;
        }
        if (count == 0) {
            out.failure = {"no_selection", "the player has selected no army or navy"};
            return out;
        }
        if (count > kMaxUnitsPerCommand) {
            out.failure = {"bad_command", "more than " + std::to_string(kMaxUnitsPerCommand) + " units selected"};
            return out;
        }
        refs.resize(count);
    } else {
        std::string tag;
        if (WorldFailure f = PlayerTag(world, tag); f.failed()) {
            out.failure = f;
            return out;
        }
        for (const std::uint64_t id : request.ids) {
            GameUnit u{};
            u.size = sizeof(u);
            const int r = world->unit(tag.c_str(), id, &u);
            if (r == kNotFound) {
                out.failure = {"unknown_unit", "the player's country " + tag + " has no unit " + std::to_string(id)};
                return out;
            }
            if (r != kOk) {
                out.failure = Failed("unit", r);
                return out;
            }
            GameUnitRef ref{};
            ref.id = id;
            std::memcpy(ref.country, tag.c_str(), std::min<std::size_t>(tag.size(), 3));
            ref.kind = u.kind;
            refs.push_back(ref);
        }
    }
    GameCommand command{};
    command.size = sizeof(command);
    command.type = request.verb == UnitRequest::Verb::Drill   ? kDrillCommand
                   : request.verb == UnitRequest::Verb::Merge ? kMergeCommand
                                                              : kSplitCommand;
    command.flags = request.verb == UnitRequest::Verb::Drill && request.start ? kCommandStart : 0;
    command.count = static_cast<std::uint32_t>(refs.size());
    command.units = refs.data();
    GameCommandResult result{};
    result.size = sizeof(result);
    out.units = command.count;
    out.result = commands->post(self, &command, &result);
    out.posted = result.posted;
    out.skipped = result.skipped;
    out.refused = result.refused;
    return out;
}

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
