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

constexpr std::uint32_t kMaxPending = 4096;  // pending events of one country taken as plausible
constexpr const char* kNoPending = "anvil-game.world has no pending events: they need Anvil 1.21 or newer";
constexpr std::uint32_t kPendingHidden = 0x1;  // ANVIL_GAME_PENDING_HIDDEN

using TextCall = int (*)(std::int32_t, char*, std::uint32_t, std::uint32_t*);

// The text a call gives, or an empty one when it fails (the event left meanwhile).
std::string Text(TextCall call, std::int32_t id) {
    char text[512] = {};
    std::uint32_t length = 0;
    if (call(id, text, sizeof(text), &length) != kOk) return {};
    return std::string(text, strnlen(text, sizeof(text)));
}

void ReadPending(const GameWorldService* world, const std::string& tag, ScopeExtras& out) {
    std::vector<std::int32_t> ids(16);
    std::uint32_t count = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int r = world->pending_events(tag.c_str(), ids.data(), static_cast<std::uint32_t>(ids.size()), &count);
        if (r != kOk) {
            out.pending_missing = Why("pending_events", r, "pending event");
            return;
        }
        if (count <= ids.size() || count > kMaxPending) break;
        ids.resize(count);
    }
    ids.resize(std::min<std::size_t>(count, ids.size()));
    for (const std::int32_t id : ids) {
        GamePendingEvent e{};
        e.size = sizeof(e);
        if (world->pending_event(id, &e) != kOk) continue;  // chosen between the two calls
        PendingRaw p;
        p.id = e.pending_id;
        p.key = Text(world->pending_event_key, id);
        p.title = Text(world->pending_event_title, id);
        p.province = e.province;
        p.options = e.options;
        p.hidden = (e.flags & kPendingHidden) != 0;
        p.year = e.auto_select.year;
        p.month = e.auto_select.month;
        p.day = e.auto_select.day;
        out.pending.push_back(std::move(p));
    }
    out.pending_read = true;
}

constexpr const char* kNoDiplomacy = "anvil-game.world has no diplomacy: it needs Anvil 1.22 or newer";
constexpr std::uint32_t kRelationKinds = 8;
constexpr std::uint32_t kMaxTags = 4096;  // a list of tags taken as plausible

std::string TagText(const GameTag& t) { return std::string(t.text, strnlen(t.text, sizeof(t.text))); }

// A list of tags from a call, grown once to the count it gave.
int ReadTags(const std::function<int(GameTag*, std::uint32_t, std::uint32_t*)>& call, std::vector<std::string>& out) {
    std::vector<GameTag> tags(16);
    std::uint32_t count = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int r = call(tags.data(), static_cast<std::uint32_t>(tags.size()), &count);
        if (r != kOk) return r;
        if (count <= tags.size() || count > kMaxTags) break;
        tags.resize(count);
    }
    out.clear();
    for (std::size_t i = 0; i < std::min<std::size_t>(count, tags.size()); ++i) out.push_back(TagText(tags[i]));
    return kOk;
}

StatusRaw Status(const GameWorldService* world, const std::string& from, const std::string& to) {
    GameDiplomacyStatus s{};
    s.size = sizeof(s);
    StatusRaw out;
    if (world->diplomacy_status(from.c_str(), to.c_str(), &s) != kOk) return out;
    out.read = true;
    out.opinion = s.opinion;
    out.timed_sum = s.timed_sum;
    out.trust = s.trust;
    out.favors = s.favors;
    out.truce = (s.flags & 0x1u) != 0;
    return out;
}

void ReadDiplomacy(const GameWorldService* world, const std::string& tag, ScopeExtras& out) {
    DiplomacyRaw& d = out.diplomacy;
    for (std::uint32_t kind = 0; kind < kRelationKinds; ++kind) {
        std::vector<GameRelation> list(16);
        std::uint32_t count = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const int r = world->country_relations(tag.c_str(), kind, list.data(), static_cast<std::uint32_t>(list.size()), &count);
            if (r != kOk) {
                out.diplomacy = {};
                out.diplomacy_missing = Why("country_relations", r, "diplomacy");
                return;
            }
            if (count <= list.size() || count > kMaxTags) break;
            list.resize(count);
        }
        for (std::size_t i = 0; i < std::min<std::size_t>(count, list.size()); ++i) {
            const GameRelation& g = list[i];
            RelationRaw rel;
            rel.first = TagText(g.first);
            rel.second = TagText(g.second);
            rel.has_start = g.start.size != 0;
            rel.start_year = g.start.year, rel.start_month = g.start.month, rel.start_day = g.start.day;
            rel.has_end = g.end.size != 0;
            rel.end_year = g.end.year, rel.end_month = g.end.month, rel.end_day = g.end.day;
            if (kind == 6 && rel.first == tag) {  // ANVIL_GAME_RELATION_DEPENDENCY: the overlord's subject
                char text[128] = {};
                std::uint32_t length = 0;
                if (world->subject_type(tag.c_str(), rel.second.c_str(), text, sizeof(text), &length) == kOk) {
                    rel.subject_type.assign(text, strnlen(text, sizeof(text)));
                }
            }
            d.relations[kind].push_back(std::move(rel));
        }
    }
    int r = ReadTags([&](GameTag* o, std::uint32_t c, std::uint32_t* n) { return world->country_rivals(tag.c_str(), o, c, n); },
                     d.rivals);
    if (r == kOk) {
        r = ReadTags([&](GameTag* o, std::uint32_t c, std::uint32_t* n) { return world->country_subjects(tag.c_str(), o, c, n); },
                     d.subjects);
    }
    if (r != kOk) {
        out.diplomacy = {};
        out.diplomacy_missing = Why("country_rivals / country_subjects", r, "diplomacy");
        return;
    }
    GameTag overlord{};
    if (world->country_overlord(tag.c_str(), &overlord) == kOk) d.overlord = TagText(overlord);  // NOT_FOUND: none
    GameTag player{};
    if (world->player != nullptr && world->player(&player) == kOk && TagText(player) != tag) {
        d.player = TagText(player);
        if (!d.player.empty()) {
            d.toward_player = Status(world, tag, d.player);
            d.player_toward = Status(world, d.player, tag);
        }
    }
    out.diplomacy_read = true;
}

}  // namespace

bool HasDiplomacy(const GameWorldService* world) {
    return world != nullptr && Has(world, &GameWorldService::country_relations, offsetof(GameWorldService, country_relations)) &&
           Has(world, &GameWorldService::subject_type, offsetof(GameWorldService, subject_type)) &&
           Has(world, &GameWorldService::diplomacy_status, offsetof(GameWorldService, diplomacy_status)) &&
           Has(world, &GameWorldService::country_rivals, offsetof(GameWorldService, country_rivals)) &&
           Has(world, &GameWorldService::country_overlord, offsetof(GameWorldService, country_overlord)) &&
           Has(world, &GameWorldService::country_subjects, offsetof(GameWorldService, country_subjects));
}

bool HasPending(const GameWorldService* world) {
    return world != nullptr && Has(world, &GameWorldService::pending_events, offsetof(GameWorldService, pending_events)) &&
           Has(world, &GameWorldService::pending_event, offsetof(GameWorldService, pending_event)) &&
           Has(world, &GameWorldService::pending_event_key, offsetof(GameWorldService, pending_event_key)) &&
           Has(world, &GameWorldService::pending_event_title, offsetof(GameWorldService, pending_event_title));
}

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
    out.country = true;
    if (world == nullptr) {
        out.flags_missing = out.variables_missing = out.pending_missing = out.diplomacy_missing = kNoService;
        return;
    }
    const std::string t(tag);
    if (HasPending(world)) {
        ReadPending(world, t, out);
    } else {
        out.pending_missing = kNoPending;
    }
    if (HasDiplomacy(world)) {
        ReadDiplomacy(world, t, out);
    } else {
        out.diplomacy_missing = kNoDiplomacy;
    }
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
