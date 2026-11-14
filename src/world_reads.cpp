#include "world_reads.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace tongs {
namespace {

constexpr std::uint32_t kMaxList = 65536;          // more entries than any core or province list has
constexpr std::uint32_t kMaxName = 64u * 1024;     // bytes of a province name taken as plausible

constexpr const char* kNoService = "anvil-game.world is not loaded (Anvil's anvil-game plugin)";

std::string TagText(const AnvilGameTag& tag) { return std::string(tag.text, strnlen(tag.text, sizeof(tag.text))); }

// A failure of `call` that has no meaning of its own for the read.
WorldFailure Failed(const char* call, AnvilResult r) {
    if (r == ANVIL_ERROR_WRONG_PHASE) {
        return {"not_in_game", std::string("anvil-game.world does not answer outside a session (") + call + ")"};
    }
    return {"read_failed", std::string(call) + " answered " + std::to_string(static_cast<int>(r))};
}

WorldFailure UnknownProvince(const AnvilGameWorldService* world, std::int32_t id) {
    const std::int32_t limit = world->province_limit != nullptr ? world->province_limit() : 0;
    std::string detail = "no province " + std::to_string(id);
    if (limit > 1) detail += " (valid IDs 1.." + std::to_string(limit - 1) + ")";
    return {"unknown_province", detail};
}

// A list call's answer: asked with room for 64, then again with room for the whole count.
template <typename T, typename Call>
AnvilResult ReadList(Call call, std::vector<T>& out) {
    out.assign(64, T{});
    std::uint32_t count = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const AnvilResult r = call(out.data(), static_cast<std::uint32_t>(out.size()), &count);
        if (r != ANVIL_OK) return r;
        if (count <= out.size()) break;
        if (count > kMaxList) return ANVIL_ERROR_INTERNAL;
        out.assign(count, T{});
    }
    if (count > out.size()) return ANVIL_ERROR_INTERNAL;  // still growing after four tries
    out.resize(count);
    return ANVIL_OK;
}

}  // namespace

WorldFailure ReadProvinceFields(const AnvilGameWorldService* world, std::int32_t id, ProvinceRaw& out) {
    out = {};
    if (world == nullptr || world->province == nullptr) return {"unavailable", kNoService};
    AnvilGameProvince p{};
    p.size = sizeof(p);
    const AnvilResult r = world->province(id, &p);
    if (r == ANVIL_ERROR_NOT_FOUND) return UnknownProvince(world, id);
    if (r != ANVIL_OK) return Failed("province", r);
    out.id = p.id;
    out.owner = TagText(p.owner);
    out.controller = TagText(p.controller);
    out.base_tax = p.base_tax;
    out.base_production = p.base_production;
    out.base_manpower = p.base_manpower;
    out.land = (p.flags & ANVIL_GAME_PROVINCE_LAND) != 0;
    out.wasteland = (p.flags & ANVIL_GAME_PROVINCE_WASTELAND) != 0;
    return {};
}

WorldFailure ReadProvince(const AnvilGameWorldService* world, std::int32_t id, ProvinceRaw& out) {
    if (WorldFailure f = ReadProvinceFields(world, id, out); f.failed()) return f;
    if (world->province_name == nullptr || world->province_cores == nullptr) return {"unavailable", kNoService};
    std::vector<char> name(256);
    std::uint32_t length = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const AnvilResult r = world->province_name(id, name.data(), static_cast<std::uint32_t>(name.size()), &length);
        if (r != ANVIL_OK) return Failed("province_name", r);
        if (length < name.size()) break;
        if (length >= kMaxName) return {"read_failed", "the name of province " + std::to_string(id) + " is not plausible"};
        name.assign(length + 1, '\0');
    }
    if (length >= name.size()) return {"read_failed", "the name of province " + std::to_string(id) + " kept growing"};
    out.name.assign(name.data(), length);
    std::vector<AnvilGameTag> cores;
    const AnvilResult r = ReadList<AnvilGameTag>(
        [&](AnvilGameTag* list, std::uint32_t capacity, std::uint32_t* count) {
            return world->province_cores(id, list, capacity, count);
        },
        cores);
    if (r != ANVIL_OK) return Failed("province_cores", r);
    for (const AnvilGameTag& core : cores) out.cores.push_back(TagText(core));
    return {};
}

WorldFailure ReadCountry(const AnvilGameWorldService* world, std::string_view tag, CountryRaw& out) {
    out = {};
    if (world == nullptr || world->country == nullptr || world->country_provinces == nullptr) {
        return {"unavailable", kNoService};
    }
    const std::string text(tag);
    AnvilGameCountry c{};
    c.size = sizeof(c);
    AnvilResult r = world->country(text.c_str(), &c);
    if (r == ANVIL_ERROR_NOT_FOUND) return {"unknown_country", "the game knows no country " + text};
    if (r != ANVIL_OK) return Failed("country", r);
    out.tag = TagText(c.tag);
    out.observer = (c.flags & ANVIL_GAME_COUNTRY_OBSERVER) != 0;
    out.province_count = c.province_count;
    out.capital = c.capital;
    out.capital_valid = c.capital != 0;  // the service answers 0 for a capital outside 1 .. province_limit - 1
    r = ReadList<std::int32_t>(
        [&](std::int32_t* ids, std::uint32_t capacity, std::uint32_t* count) {
            return world->country_provinces(text.c_str(), ids, capacity, count);
        },
        out.provinces);
    if (r != ANVIL_OK) return Failed("country_provinces", r);
    return {};
}

WorldFailure ReadPlayer(const AnvilGameWorldService* world, PlayerRaw& out) {
    out = {};
    if (world == nullptr) return {"unavailable", kNoService};
    if (!ANVIL_GAME_WORLD_HAS(world, player_tags) || world->player_tags == nullptr) {
        return {"unavailable", "anvil-game.world has no player_tags: it needs Anvil 1.23 or newer"};
    }
    AnvilGamePlayer p{};
    p.size = sizeof(p);
    const AnvilResult r = world->player_tags(&p);
    if (r != ANVIL_OK) return Failed("player_tags", r);
    out.tag = TagText(p.tag);
    out.player_tag = TagText(p.own);
    out.observed_tag = TagText(p.observed);
    out.observer = (p.flags & ANVIL_GAME_PLAYER_OBSERVER) != 0;
    return {};
}

}  // namespace tongs
