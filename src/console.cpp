#include "console.hpp"

#include <cstdint>
#include <string>

namespace tongs {
namespace {

constexpr std::size_t kMaxLine = 1024;
constexpr std::size_t kMaxTokens = 32;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) return false;
    }
    return true;
}

bool StartsNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && EqualNoCase(text.substr(0, prefix.size()), prefix);
}

// ADR 0001: never run, whatever the config says. enable_all_commands opens the
// game's developer and multiplayer/ironman gates; the others act on the
// user's account outside the game. social_* is matched as a prefix.
constexpr std::string_view kAlwaysRefused[] = {
    "enable_all_commands", "debug_achievements_clear", "test_achievement", "debug_achievements",
    "CrashReporter.UploadCrash",
};

// The `danger` class of the research table (crashes, leaks, desync, rewriting
// the map or the world, install and save rules), the engine's simulated crash,
// and the two commands that run a file of console commands, which would pass
// lines past these refusals.
constexpr std::string_view kDanger[] = {
    "oos", "startup_screen", "reinitchat", "disable_dlc", "spawn_all_units", "bearhaslanded", "leak", "leak_gpu",
    "low_memory", "map_random", "debug_crash", "set_achievements_ok", "purge_mem", "deiron", "syntheticdawn",
    "CrashReporter.SimulateCrash", "run_commands", "runyear",
};

}  // namespace

ConsoleClass ClassifyConsoleCommand(std::string_view name) {
    if (StartsNoCase(name, "social_")) return ConsoleClass::AlwaysRefused;
    for (const std::string_view refused : kAlwaysRefused) {
        if (EqualNoCase(name, refused)) return ConsoleClass::AlwaysRefused;
    }
    for (const std::string_view danger : kDanger) {
        if (EqualNoCase(name, danger)) return ConsoleClass::Danger;
    }
    return ConsoleClass::Normal;
}

std::string SplitConsoleLine(std::string_view line, std::vector<std::string>& tokens) {
    tokens.clear();
    if (line.size() > kMaxLine) return "the line is longer than " + std::to_string(kMaxLine) + " characters";
    for (const char c : line) {
        const auto u = static_cast<unsigned char>(c);
        if (u != '\t' && (u < 0x20 || u > 0x7E)) return "only printable ASCII is accepted";
    }
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) tokens.emplace_back(line.substr(start, i - start));
    }
    if (tokens.empty()) return "usage: console COMMAND [ARGUMENTS]";
    if (tokens.size() > kMaxTokens) return "more than " + std::to_string(kMaxTokens) + " words";
    return {};
}

const ConsoleEntry* FindConsoleEntry(const std::vector<ConsoleEntry>& entries, std::string_view token) {
    for (const ConsoleEntry& entry : entries) {
        if (EqualNoCase(entry.name, token)) return &entry;
        for (const std::string& alias : entry.aliases) {
            if (EqualNoCase(alias, token)) return &entry;
        }
    }
    return nullptr;
}

ConsoleRefusal CheckConsoleName(const GameCallPolicy& policy, std::string_view name) {
    switch (ClassifyConsoleCommand(name)) {
        case ConsoleClass::AlwaysRefused:
            return {"refused", std::string(name) + " is always refused (ADR 0001)"};
        case ConsoleClass::Danger:
            for (const std::string& allowed : policy.allow) {
                if (EqualNoCase(allowed, name)) return {};
            }
            return {"refused", std::string(name) + " is a danger command; console_allow = [\"" + std::string(name) +
                                   "\"] in the config allows it"};
        case ConsoleClass::Normal:
            break;
    }
    return {};
}

namespace {
constexpr std::uint32_t kStarted = 1, kIronman = 2, kMultiplayer = 4, kSuspended = 8;
}  // namespace

void GameCallSessionWatch::Suspended(bool suspended) {
    if (suspended) {
        bits_.fetch_or(kSuspended, std::memory_order_acq_rel);
    } else {
        bits_.fetch_and(~kSuspended, std::memory_order_acq_rel);
    }
}

void GameCallSessionWatch::Session(bool started, bool ironman, bool multiplayer) {
    std::uint32_t old = bits_.load(std::memory_order_acquire);
    std::uint32_t next;
    do {
        next = old & kSuspended;
        if (started) next |= kStarted | (ironman ? kIronman : 0) | (multiplayer ? kMultiplayer : 0);
    } while (!bits_.compare_exchange_weak(old, next, std::memory_order_acq_rel));
}

GameCallSession GameCallSessionWatch::Read() const {
    const std::uint32_t b = bits_.load(std::memory_order_acquire);
    return {(b & kStarted) != 0, (b & kIronman) != 0, (b & kMultiplayer) != 0, (b & kSuspended) != 0};
}

bool GameCallPolicy::ActsInIronman() const {
    const GameCallSession s = Session();
    return s.Acts() && s.ironman;
}

bool GameCallPolicy::ActsInMultiplayer() const {
    const GameCallSession s = Session();
    return s.Acts() && s.multiplayer;
}

namespace {
ConsoleRefusal IronmanRefusal() {
    return {"ironman", "game calls are refused in ironman unless the player allows Tongs and its Game calls "
                       "module there (allow_ironman = [\"tongs\", \"tongs.game_calls\"] in anvil/anvil.toml)"};
}

ConsoleRefusal MultiplayerRefusal() {
    return {"multiplayer", "game calls are refused in multiplayer unless the player allows Tongs and its Game calls "
                           "module there (allow_multiplayer = [\"tongs\", \"tongs.game_calls\"] in anvil/anvil.toml)"};
}
}  // namespace

ConsoleRefusal CheckModuleSuspended(const GameCallPolicy& policy) {
    const GameCallSession s = policy.Session();
    if (!s.suspended) return {};
    if (s.started && s.ironman) return IronmanRefusal();
    if (s.started && s.multiplayer) return MultiplayerRefusal();
    return {"game_calls_suspended", "Anvil suspended Tongs' Game calls module in the session in progress"};
}

ConsoleRefusal CheckConsoleMode(const GameCallPolicy& policy, const ConsoleRaw& raw) {
    if (ConsoleRefusal r = CheckModuleSuspended(policy); r.refused()) return r;
    if (raw.ironman && !policy.ActsInIronman()) return IronmanRefusal();
    if (raw.multiplayer && !policy.ActsInMultiplayer()) return MultiplayerRefusal();
    return {};
}

std::vector<std::string> CheckConsoleAllow(const std::vector<std::string>& allow) {
    std::vector<std::string> notes;
    for (const std::string& name : allow) {
        const ConsoleClass c = ClassifyConsoleCommand(name);
        if (c == ConsoleClass::AlwaysRefused) {
            notes.push_back("console_allow: " + name + " is always refused; ignored");
        } else if (c == ConsoleClass::Normal) {
            notes.push_back("console_allow: " + name + " is not a danger command; it needs no entry");
        }
    }
    return notes;
}


namespace {

struct ArgumentKinds {
    std::string_view name;
    std::string_view kinds;
};

// Generated from the game's static command table (0x1FC23E0, 378 records; docs/design.md, "Game calls
// (0.7.0)"): each record's argument descriptions, classified as a province ID ('P'), a country tag ('T'),
// either ('E') or neither ('-'). Sorted by name; commands with neither are left out.
constexpr ArgumentKinds kArgumentKinds[] = {
    {"absolutism", "-T"},
    {"add_backer", "P"},
    {"add_baseunrest", "P"},
    {"add_cb", "-TT"},
    {"add_claim", "PT"},
    {"add_colonist", "T"},
    {"add_consort", "TT"},
    {"add_core", "PT"},
    {"add_devastation", "P"},
    {"add_diplo", "T"},
    {"add_heir", "T"},
    {"add_idea_group", "-T"},
    {"add_interest", "T"},
    {"add_liberty_desire", "-T"},
    {"add_local_autonomy", "P"},
    {"add_missionary", "T"},
    {"add_monarch", "T"},
    {"add_natives", "P"},
    {"add_opinion", "TT"},
    {"add_pa", "T"},
    {"add_permanent_claim", "PT"},
    {"add_pi", "T"},
    {"add_prosperity", "P"},
    {"add_republican_tradition", "-T"},
    {"adm", "-T"},
    {"admiral", "----T"},
    {"age_consort", "-T"},
    {"age_heir", "-T"},
    {"age_ruler", "-T"},
    {"ai", "T"},
    {"ai_army_tick", "T"},
    {"ai_budget", "T"},
    {"ai_plan_regions", "T"},
    {"annex", "TT"},
    {"army_drill", "-T"},
    {"army_professionalism", "-T"},
    {"army_tradition", "-T"},
    {"assimilate", "PT"},
    {"authority", "-T"},
    {"bird_mana", "-T"},
    {"build_all", "P"},
    {"build_forts", "-P"},
    {"cash", "-T"},
    {"center_of_trade", "-P"},
    {"change_culture_court", "--T"},
    {"change_religion", "P"},
    {"change_religion_court", "--T"},
    {"church_power", "-T"},
    {"clearae", "TT"},
    {"clearreforms", "T"},
    {"clr_consort_flag", "T"},
    {"clr_prov_flag", "-P"},
    {"colonize", "P"},
    {"condottieri", "T"},
    {"control", "PT"},
    {"controll", "PT"},
    {"corrupt", "-T"},
    {"country_modifier", "T"},
    {"create_march", "TT"},
    {"crownland", "-T"},
    {"culture", "P"},
    {"debug_ai_budget", "T"},
    {"debug_budgetall", "T"},
    {"debug_budgettag", "T"},
    {"debug_crownland", "T"},
    {"debug_name", "P"},
    {"declare_war", "TT"},
    {"delete_wars", "T"},
    {"devotion", "-T"},
    {"die", "T"},
    {"dip", "-T"},
    {"discover", "TT"},
    {"dist", "PP"},
    {"doom", "-T"},
    {"embrace", "P"},
    {"epicfail", "T"},
    {"event", "-T"},
    {"exhaust", "-T"},
    {"favors", "T"},
    {"fervor", "-T"},
    {"form_union", "TT"},
    {"fow", "P"},
    {"golden_age", "T"},
    {"harmonization", "-T"},
    {"harmony", "-T"},
    {"horde_unity", "-T"},
    {"imperial_authority", "-T"},
    {"incident", "-T"},
    {"inflation", "-T"},
    {"institution", "P"},
    {"integrate", "TT"},
    {"invasion_target", "P"},
    {"invest", "-P"},
    {"karma", "-T"},
    {"kill_advisors", "T"},
    {"kill_cardinal", "T"},
    {"kill_consort", "T"},
    {"kill_heir", "T"},
    {"leader", "----T"},
    {"legitimacy", "-T"},
    {"lucky", "T"},
    {"make_subject", "TT"},
    {"manpower", "-T"},
    {"meritocracy", "-T"},
    {"mil", "-T"},
    {"mission_tree", "-T"},
    {"moveunit", "-P"},
    {"native_uprising", "P"},
    {"navy_tradition", "-T"},
    {"on_action", "-T"},
    {"own", "PT"},
    {"own_core", "PT"},
    {"paper_mana", "-T"},
    {"piety", "-T"},
    {"pirate", "P"},
    {"population", "P"},
    {"power", "-T"},
    {"powerpoints", "-T"},
    {"prestige", "-T"},
    {"print_prov_flags", "P"},
    {"province_modifier", "P"},
    {"reform", "P"},
    {"reformprogress", "-T"},
    {"religious_school", "-T"},
    {"remove_cb", "-TT"},
    {"remove_claim", "PT"},
    {"remove_core", "PT"},
    {"remove_country", "T"},
    {"remove_interest", "T"},
    {"remove_march", "TT"},
    {"remove_rival", "T"},
    {"repair", "T"},
    {"revolt", "P"},
    {"revolution_target", "T"},
    {"sailors", "-T"},
    {"set_base_manpower", "P"},
    {"set_base_production", "P"},
    {"set_base_tax", "P"},
    {"set_consort_flag", "T"},
    {"set_prov_flag", "-P"},
    {"setmissionaryprogress", "P"},
    {"show_ideas", "T"},
    {"siege", "P"},
    {"spawn", "P"},
    {"spawnactor", "-P"},
    {"splendor", "-T"},
    {"spread_revolution", "E"},
    {"sprite_pack_debug", "T"},
    {"spynetwork", "T"},
    {"stability", "-T"},
    {"state", "P"},
    {"sword_mana", "-T"},
    {"syntheticdawn", "T"},
    {"tag", "T"},
    {"test_neighbour", "T"},
    {"test_trade_steering", "T"},
    {"testprovs", "PP"},
    {"tribal_development", "P"},
    {"tributary", "TT"},
    {"trust", "T"},
    {"vassalize", "TT"},
    {"zeal", "-T"},
};

// The leading integer of `token`, as atoi reads it: a sign, then digits. False
// when the token does not start with one; `value` saturates far past any limit.
bool LeadingInteger(std::string_view token, std::int64_t& value) {
    std::size_t i = 0;
    bool negative = false;
    if (i < token.size() && (token[i] == '+' || token[i] == '-')) negative = token[i++] == '-';
    if (i >= token.size() || token[i] < '0' || token[i] > '9') return false;
    value = 0;
    for (; i < token.size() && token[i] >= '0' && token[i] <= '9'; ++i) {
        if (value < (std::int64_t{1} << 40)) value = value * 10 + (token[i] - '0');
    }
    if (negative) value = -value;
    return true;
}

bool LooksLikeTag(std::string_view token) {
    if (token.size() != 3) return false;
    for (const char c : token) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

ConsoleRefusal CheckProvince(std::string_view name, std::size_t position, std::int64_t id, std::int32_t limit) {
    if (id >= 1 && id < limit) return {};
    return {"bad_command", "argument " + std::to_string(position) + " of " + std::string(name) + " is province " +
                               std::to_string(id) + ", which the map does not have (valid IDs 1.." +
                               std::to_string(limit - 1) + ")"};
}

ConsoleRefusal CheckTag(std::string_view name, std::size_t position, std::string_view token,
                        const std::function<bool(std::string_view)>& country_known) {
    std::string tag(token);
    for (char& c : tag) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (country_known(tag)) return {};
    return {"bad_command",
            "argument " + std::to_string(position) + " of " + std::string(name) + " is the country tag " + tag +
                ", which the game does not know"};
}

}  // namespace

std::string_view ConsoleArgumentKinds(std::string_view name) {
    for (const ArgumentKinds& entry : kArgumentKinds) {
        if (EqualNoCase(entry.name, name)) return entry.kinds;
    }
    return {};
}

ConsoleRefusal CheckConsoleArguments(std::string_view name, const std::vector<std::string>& tokens,
                                     std::int32_t province_limit,
                                     const std::function<bool(std::string_view tag)>& country_known) {
    const std::string_view kinds = ConsoleArgumentKinds(name);
    for (std::size_t i = 0; i < kinds.size() && i + 1 < tokens.size(); ++i) {
        const char kind = kinds[i];
        if (kind == '-') continue;
        const std::string_view token = tokens[i + 1];
        std::int64_t id = 0;
        if ((kind == 'P' || kind == 'E') && LeadingInteger(token, id)) {
            if (province_limit <= 1) continue;
            if (ConsoleRefusal r = CheckProvince(name, i + 1, id, province_limit); r.refused()) return r;
        } else if ((kind == 'T' || kind == 'E') && LooksLikeTag(token) && country_known) {
            if (ConsoleRefusal r = CheckTag(name, i + 1, token, country_known); r.refused()) return r;
        }
    }
    return {};
}

}  // namespace tongs
