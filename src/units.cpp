#include "units.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <charconv>

namespace tongs {
namespace {

std::vector<std::string_view> Words(std::string_view text) {
    std::vector<std::string_view> words;
    while (!text.empty()) {
        const std::size_t start = text.find_first_not_of(" \t");
        if (start == std::string_view::npos) break;
        text.remove_prefix(start);
        const std::size_t end = text.find_first_of(" \t");
        words.push_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
    }
    return words;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
char Upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
std::string Shown(std::string_view word) { return std::string(word.substr(0, 32)); }
WorldFailure Bad(std::string detail) { return {"bad_command", std::move(detail)}; }

bool ParseTimeout(std::string_view word, int& timeout_ms) {
    if (!word.starts_with("timeout=")) return false;
    const std::string_view value = word.substr(8);
    int seconds = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (ec != std::errc{} || ptr != value.data() + value.size() || seconds < 1 || seconds > kWorldMaxTimeoutS) {
        return false;
    }
    timeout_ms = seconds * 1000;
    return true;
}

// A unit id: decimal, no sign, no leading zeros, from 1, within uint64.
bool ParseId(std::string_view word, std::uint64_t& id) {
    if (word.empty() || word.size() > 20 || word[0] == '0') return false;
    for (const char c : word) {
        if (!IsDigit(c)) return false;
    }
    const auto [ptr, ec] = std::from_chars(word.data(), word.data() + word.size(), id);
    return ec == std::errc{} && ptr == word.data() + word.size() && id != 0;
}

std::string Quote(std::string_view text) { return "\"" + JsonEscape(text) + "\""; }

}  // namespace

WorldFailure ParseUnitsRequest(std::string_view argument, UnitsRequest& out) {
    out = {};
    constexpr const char* kUsage = " (usage: units [TAG] [timeout=S])";
    const std::vector<std::string_view> words = Words(argument);
    std::size_t i = 0;
    if (i < words.size() && !words[i].starts_with("timeout=")) {
        const std::string_view tag = words[i];
        bool valid = tag.size() == 3;
        for (const char c : tag) valid = valid && (IsDigit(c) || IsLetter(c));
        if (!valid) return {"unknown_country", "not a tag: " + Shown(tag) + " (three letters or digits)"};
        for (const char c : tag) out.tag += Upper(c);
        ++i;
    }
    for (; i < words.size(); ++i) {
        if (!ParseTimeout(words[i], out.timeout_ms)) return Bad("bad option: " + Shown(words[i]) + kUsage);
    }
    return {};
}

WorldFailure ParseUnitRequest(std::string_view argument, UnitRequest& out, std::string& command) {
    out = {};
    command = "unit";
    constexpr const char* kUsage =
        " (usage: unit drill start|stop UNITS, unit split UNIT, unit merge UNITS; UNITS are unit ids or "
        "`selected`, then [timeout=S])";
    const std::vector<std::string_view> words = Words(argument);
    if (words.empty()) return Bad(std::string("usage") + kUsage);
    std::size_t i = 1;
    if (words[0] == "drill") {
        out.verb = UnitRequest::Verb::Drill;
        command = "unit drill";
        if (words.size() < 2 || (words[1] != "start" && words[1] != "stop")) {
            return Bad("unit drill takes start or stop" + std::string(kUsage));
        }
        out.start = words[1] == "start";
        i = 2;
    } else if (words[0] == "split") {
        out.verb = UnitRequest::Verb::Split;
        command = "unit split";
    } else if (words[0] == "merge") {
        out.verb = UnitRequest::Verb::Merge;
        command = "unit merge";
    } else {
        return Bad("unit has no subcommand " + Shown(words[0]) + kUsage);
    }
    for (; i < words.size(); ++i) {
        const std::string_view word = words[i];
        if (word.starts_with("timeout=")) {
            if (!ParseTimeout(word, out.timeout_ms)) return Bad("bad option: " + Shown(word) + kUsage);
            continue;
        }
        if (word == "selected") {
            if (out.selected || !out.ids.empty()) return Bad("`selected` stands alone" + std::string(kUsage));
            out.selected = true;
            continue;
        }
        std::uint64_t id = 0;
        if (!ParseId(word, id)) return {"unknown_unit", "not a unit id: " + Shown(word) + " (a decimal number from 1)"};
        if (out.selected) return Bad("`selected` stands alone" + std::string(kUsage));
        if (std::find(out.ids.begin(), out.ids.end(), id) != out.ids.end()) {
            return Bad("unit " + std::string(word) + " is named twice");
        }
        out.ids.push_back(id);
    }
    if (!out.selected && out.ids.empty()) return Bad(command + " needs units" + kUsage);
    if (out.ids.size() > kMaxUnitsPerCommand) {
        return Bad("at most " + std::to_string(kMaxUnitsPerCommand) + " units in one command");
    }
    if (out.verb == UnitRequest::Verb::Split && out.ids.size() > 1) return Bad("unit split takes one unit");
    if (out.verb == UnitRequest::Verb::Merge && !out.selected && out.ids.size() < 2) {
        return Bad("unit merge takes two or more units");
    }
    return {};
}

const char* UnitResultName(int result) {
    switch (result) {
        case 0: return "ok";
        case 1: return "invalid_argument";
        case 2: return "not_declared";
        case 7: return "out_of_memory";
        case 8: return "wrong_phase";
        case 9: return "unsupported";
        case 11: return "not_found";
        case 13: return "suspended";
        case 14: return "limit";
        default: return "failed";
    }
}

const char* MissionName(std::int32_t mission) {
    static const char* const kNames[] = {"none",       "protect_trade",  "privateer",      "hunt_pirates",
                                         "patrol",     "explore",        "evade",          "hunt_fleets",
                                         "blockade",   "intercept",      "repair",         "circumnavigate",
                                         "explore_land", "hunt_rebels",  "siege",          "drill"};
    if (mission >= 0 && mission < static_cast<std::int32_t>(std::size(kNames))) return kNames[mission];
    return "unknown";
}

std::string UnitsJson(const UnitsRaw& raw) {
    std::string out = ",\"tag\":" + Quote(raw.tag) + ",\"units\":[";
    for (std::size_t i = 0; i < raw.units.size(); ++i) {
        const UnitInfo& u = raw.units[i];
        if (i) out += ',';
        out += "{\"id\":" + std::to_string(u.id) + ",\"kind\":\"" + (u.kind == kUnitNavy ? "navy" : "army") +
               "\",\"name\":" + Quote(u.name) + ",\"location\":" + (u.location ? std::to_string(u.location) : "null") +
               ",\"mission\":\"" + MissionName(u.mission) + "\",\"subunits\":" + std::to_string(u.subunits) +
               ",\"strength\":" + std::to_string(u.strength) +
               ",\"leader\":" + (u.leader ? std::to_string(u.leader) : "null") + "}";
    }
    return out + "]";
}

}  // namespace tongs
