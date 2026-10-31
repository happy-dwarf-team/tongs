#include "world.hpp"

#include "protocol.hpp"

#include <charconv>
#include <cstdio>

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

// timeout=S, 1..kWorldMaxTimeoutS seconds.
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

std::string Quote(std::string_view text) { return "\"" + JsonEscape(text) + "\""; }

std::string TagOrNull(const std::string& tag) { return tag.empty() ? "null" : Quote(tag); }

const char* Bool(bool value) { return value ? "true" : "false"; }

}  // namespace

WorldFailure ParseWorldRequest(std::string_view command, std::string_view argument, WorldRequest& out) {
    out = {};
    const std::vector<std::string_view> words = Words(argument);
    std::size_t options = 0;  // the index of the first option word
    if (command == "province") {
        out.kind = WorldRequest::Kind::Province;
        constexpr const char* kUsage = " (usage: province ID|open ID|goto ID [timeout=S])";
        if (words.empty()) return Bad(std::string("usage") + kUsage);
        if (IsLetter(words[0][0])) {
            if (words[0] == "open") {
                out.kind = WorldRequest::Kind::Open;
            } else if (words[0] == "goto") {
                out.kind = WorldRequest::Kind::Goto;
            } else {
                return Bad("province has no subcommand " + Shown(words[0]) + kUsage);
            }
            if (words.size() < 2) return Bad("province " + std::string(words[0]) + " takes an ID" + kUsage);
            ++options;
        }
        const std::string_view id = words[options];
        // Decimal only: no sign, no leading zeros, no hex; within int32.
        bool decimal = id.size() <= 9 && IsDigit(id[0]) && !(id[0] == '0' && id.size() > 1);
        for (const char c : id) decimal = decimal && IsDigit(c);
        if (!decimal) return {"unknown_province", "not a province ID: " + Shown(id) + " (a decimal number from 1)"};
        std::from_chars(id.data(), id.data() + id.size(), out.id);
        ++options;
    } else if (command == "country") {
        out.kind = WorldRequest::Kind::Country;
        if (words.empty()) return Bad("usage: country TAG [timeout=S]");
        const std::string_view tag = words[0];
        bool valid = tag.size() == 3;
        for (const char c : tag) valid = valid && (IsDigit(c) || IsLetter(c));
        if (!valid) return {"unknown_country", "not a tag: " + Shown(tag) + " (three letters or digits)"};
        for (const char c : tag) out.tag += Upper(c);
        options = 1;
    } else if (command == "player") {
        out.kind = WorldRequest::Kind::Player;
    } else {
        out.kind = WorldRequest::Kind::Selection;
    }
    for (std::size_t i = options; i < words.size(); ++i) {
        if (!ParseTimeout(words[i], out.timeout_ms)) {
            return Bad("bad option: " + Shown(words[i]) + " (timeout=1.." + std::to_string(kWorldMaxTimeoutS) + ")");
        }
    }
    return {};
}

int TagBucket(std::string_view tag, int mask) {
    int sum = 0;
    for (std::size_t i = 0; i < 3 && i < tag.size(); ++i) sum += static_cast<unsigned char>(Upper(tag[i]));
    return sum & mask;
}

std::string Thousandths(std::int64_t raw) {
    const bool negative = raw < 0;
    const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-raw) : static_cast<std::uint64_t>(raw);
    std::string out = (negative ? "-" : "") + std::to_string(magnitude / 1000);
    unsigned fraction = static_cast<unsigned>(magnitude % 1000);
    if (fraction != 0) {
        char digits[8];
        std::snprintf(digits, sizeof digits, ".%03u", fraction);
        std::string text = digits;
        while (text.back() == '0') text.pop_back();
        out += text;
    }
    return out;
}

std::string ProvinceMembers(const ProvinceRaw& p) {
    std::string out = ",\"id\":" + std::to_string(p.id) + ",\"name\":" + Quote(p.name);
    out += ",\"owner\":" + TagOrNull(p.owner) + ",\"controller\":" + TagOrNull(p.controller) + ",\"cores\":[";
    for (std::size_t i = 0; i < p.cores.size(); ++i) out += (i ? "," : "") + Quote(p.cores[i]);
    out += "],\"base_tax\":" + Thousandths(p.base_tax) + ",\"base_production\":" + Thousandths(p.base_production) +
           ",\"base_manpower\":" + Thousandths(p.base_manpower);
    out += ",\"development\":" +
           Thousandths(static_cast<std::int64_t>(p.base_tax) + p.base_production + p.base_manpower);
    return out + ",\"land\":" + Bool(p.land) + ",\"wasteland\":" + Bool(p.wasteland);
}

std::string CountryMembers(const CountryRaw& c) {
    std::string out = ",\"tag\":" + Quote(c.tag) + ",\"exists\":" + Bool(c.province_count > 0);
    out += ",\"province_count\":" + std::to_string(c.province_count) + ",\"provinces\":[";
    for (std::size_t i = 0; i < c.provinces.size(); ++i) out += (i ? "," : "") + std::to_string(c.provinces[i]);
    out += "],\"capital\":" + (c.capital_valid ? std::to_string(c.capital) : std::string("null"));
    return out + ",\"observer\":" + Bool(c.observer);
}

std::string PlayerMembers(const PlayerRaw& p) {
    return ",\"tag\":" + TagOrNull(p.tag) + ",\"player_tag\":" + TagOrNull(p.player_tag) +
           ",\"observed_tag\":" + TagOrNull(p.observed_tag) + ",\"observer\":" + Bool(p.observer);
}

std::string SelectionMembers(const SelectionRaw& s, const UiTree& tree) {
    const auto id = [](std::int32_t province) { return province > 0 ? std::to_string(province) : std::string("null"); };
    bool visible = false;
    std::string path = "null";
    if (s.view_widget != 0) {
        const UiView all(tree, true, true);
        for (std::size_t i = 0; i < tree.widgets.size(); ++i) {
            if (tree.widgets[i].address != s.view_widget) continue;
            visible = all.Visible(static_cast<int>(i));
            // The path a `ui find` without `all` resolves.
            if (visible) path = Quote(UiView(tree, false, true).PathOf(static_cast<int>(i)));
            break;
        }
    }
    return ",\"last_selected\":" + id(s.last_selected) + ",\"view\":{\"province\":" + id(s.view_province) +
           ",\"visible\":" + Bool(visible) + ",\"path\":" + path + "}";
}

}  // namespace tongs
