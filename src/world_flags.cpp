#include "world_flags.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace tongs {
namespace {

constexpr int kOk = 0;
constexpr int kNotDeclared = 2;  // ANVIL_ERROR_NOT_DECLARED: the port declared the symbols absent
constexpr int kNotFound = 11;    // ANVIL_ERROR_NOT_FOUND
constexpr std::uint32_t kMaxFlags = 32768;          // the service's own bound: a flag index is an int16
constexpr std::uint32_t kMaxNames = 4u * 1024 * 1024;  // bytes of variable names taken as plausible

constexpr const char* kNoService = "anvil-game.world is not loaded (Anvil's anvil-game plugin)";
constexpr const char* kNoFlags = "anvil-game.world has no flags: they need Anvil 1.20 or newer";
constexpr const char* kNoVariables = "anvil-game.world has no variables: they need Anvil 1.20 or newer";

template <typename Member>
bool Has(const GameWorldService* world, Member GameWorldService::*member, std::size_t offset) {
    return world->size >= offset + sizeof(world->*member) && world->*member != nullptr;
}

std::string Why(const char* call, int result, const char* family) {
    if (result == kNotDeclared) return std::string("this game build has no ") + family + " symbols (" + call + ")";
    return std::string(call) + " answered " + std::to_string(result);
}

using FlagCall = std::function<int(GameFlag*, std::uint32_t, std::uint32_t*)>;
using NamesCall = std::function<int(char*, std::uint32_t, std::uint32_t*)>;
using ValueCall = std::function<int(const char*, std::int32_t*)>;

void ReadFlags(const GameWorldService* world, const FlagCall& call, const char* name, ScopeExtras& out) {
    std::vector<GameFlag> flags(64);
    std::uint32_t count = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        for (GameFlag& f : flags) {
            f = {};
            f.date.size = sizeof(WorldDate);
        }
        const int r = call(flags.data(), static_cast<std::uint32_t>(flags.size()), &count);
        if (r != kOk) {
            out.flags_missing = Why(name, r, "flag");
            return;
        }
        if (count <= flags.size() || count > kMaxFlags) break;
        flags.resize(count);
    }
    flags.resize(std::min<std::size_t>(count, flags.size()));
    for (const GameFlag& f : flags) {
        FlagRaw raw;
        char text[256] = {};
        std::uint32_t length = 0;
        const int r = world->flag_name(f.id, text, sizeof(text), &length);
        if (r != kOk) {
            out.flags.clear();
            out.flags_missing = Why("flag_name", r, "flag");
            return;
        }
        raw.name.assign(text, strnlen(text, sizeof(text)));
        raw.year = f.date.year;
        raw.month = f.date.month;
        raw.day = f.date.day;
        out.flags.push_back(std::move(raw));
    }
    out.flags_read = true;
}

void ReadVariables(const NamesCall& names_call, const ValueCall& value_call, const char* name, ScopeExtras& out) {
    std::vector<char> names(4096);
    std::uint32_t length = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int r = names_call(names.data(), static_cast<std::uint32_t>(names.size()), &length);
        if (r != kOk) {
            out.variables_missing = Why(name, r, "variable");
            return;
        }
        if (length <= names.size() || length > kMaxNames) break;
        names.resize(length);
    }
    const std::size_t filled = std::min<std::size_t>(length, names.size());
    for (std::size_t at = 0; at < filled;) {
        const std::size_t end = std::find(names.begin() + static_cast<std::ptrdiff_t>(at),
                                          names.begin() + static_cast<std::ptrdiff_t>(filled), '\0') -
                                names.begin();
        if (end == filled) break;  // a name cut at the end of the buffer
        VariableRaw v;
        v.name.assign(names.data() + at, end - at);
        at = end + 1;
        if (v.name.empty()) continue;
        const int r = value_call(v.name.c_str(), &v.value);
        if (r == kNotFound) continue;  // gone between the two calls
        if (r != kOk) {
            out.variables.clear();
            out.variables_missing = Why(name, r, "variable");
            return;
        }
        out.variables.push_back(std::move(v));
    }
    out.variables_read = true;
}

}  // namespace

bool HasFlags(const GameWorldService* world) {
    return world != nullptr && Has(world, &GameWorldService::flag_name, offsetof(GameWorldService, flag_name)) &&
           Has(world, &GameWorldService::country_flags, offsetof(GameWorldService, country_flags)) &&
           Has(world, &GameWorldService::province_flags, offsetof(GameWorldService, province_flags));
}

bool HasVariables(const GameWorldService* world) {
    return world != nullptr &&
           Has(world, &GameWorldService::country_variable, offsetof(GameWorldService, country_variable)) &&
           Has(world, &GameWorldService::province_variable, offsetof(GameWorldService, province_variable)) &&
           Has(world, &GameWorldService::country_variable_names, offsetof(GameWorldService, country_variable_names)) &&
           Has(world, &GameWorldService::province_variable_names, offsetof(GameWorldService, province_variable_names));
}

void ReadCountryExtras(const GameWorldService* world, std::string_view tag, ScopeExtras& out) {
    out = {};
    if (world == nullptr) {
        out.flags_missing = out.variables_missing = kNoService;
        return;
    }
    const std::string t(tag);
    if (HasFlags(world)) {
        ReadFlags(world, [&](GameFlag* f, std::uint32_t cap, std::uint32_t* n) { return world->country_flags(t.c_str(), f, cap, n); },
                  "country_flags", out);
    } else {
        out.flags_missing = kNoFlags;
    }
    if (HasVariables(world)) {
        ReadVariables(
            [&](char* names, std::uint32_t cap, std::uint32_t* n) { return world->country_variable_names(t.c_str(), names, cap, n); },
            [&](const char* name, std::int32_t* value) { return world->country_variable(t.c_str(), name, value); },
            "country_variable_names", out);
    } else {
        out.variables_missing = kNoVariables;
    }
}

void ReadProvinceExtras(const GameWorldService* world, std::int32_t province, ScopeExtras& out) {
    out = {};
    if (world == nullptr) {
        out.flags_missing = out.variables_missing = kNoService;
        return;
    }
    if (HasFlags(world)) {
        ReadFlags(world, [&](GameFlag* f, std::uint32_t cap, std::uint32_t* n) { return world->province_flags(province, f, cap, n); },
                  "province_flags", out);
    } else {
        out.flags_missing = kNoFlags;
    }
    if (HasVariables(world)) {
        ReadVariables(
            [&](char* names, std::uint32_t cap, std::uint32_t* n) { return world->province_variable_names(province, names, cap, n); },
            [&](const char* name, std::int32_t* value) { return world->province_variable(province, name, value); },
            "province_variable_names", out);
    } else {
        out.variables_missing = kNoVariables;
    }
}

}  // namespace tongs
