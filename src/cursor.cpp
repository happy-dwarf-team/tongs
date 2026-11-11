#include "cursor.hpp"

#include "protocol.hpp"

#include <charconv>

namespace tongs {
namespace {

std::string Quote(std::string_view text) { return "\"" + JsonEscape(text) + "\""; }
const char* Bool(bool value) { return value ? "true" : "false"; }
std::string TagOrNull(const std::string& tag) { return tag.empty() ? "null" : Quote(tag); }

}  // namespace

WorldFailure ParseCursorRequest(std::string_view argument, int& timeout_ms) {
    timeout_ms = 0;
    while (!argument.empty()) {
        const std::size_t start = argument.find_first_not_of(" \t");
        if (start == std::string_view::npos) break;
        argument.remove_prefix(start);
        const std::size_t end = argument.find_first_of(" \t");
        const std::string_view word = argument.substr(0, end);
        argument = end == std::string_view::npos ? std::string_view{} : argument.substr(end);
        int seconds = 0;
        const std::string_view value = word.starts_with("timeout=") ? word.substr(8) : std::string_view{};
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size() || seconds < 1 ||
            seconds > kWorldMaxTimeoutS) {
            return {"bad_command", "bad option: " + std::string(word.substr(0, 32)) + " (usage: cursor [timeout=1.." +
                                       std::to_string(kWorldMaxTimeoutS) + "])"};
        }
        timeout_ms = seconds * 1000;
    }
    return {};
}

std::string CursorMembers(const CursorRaw& c) {
    std::string out = ",\"point\":{\"x\":" + std::to_string(c.x) + ",\"y\":" + std::to_string(c.y) + "}";
    out += ",\"pointer\":" + std::string(c.tongs_pointer ? "\"tongs\"" : "\"mouse\"");
    out += ",\"inside\":" + std::string(Bool(c.inside));
    out += ",\"widget\":" + (c.widget.empty() ? std::string("null") : Quote(c.widget));
    // The map takes the pointer when the interface hovers nothing there.
    out += ",\"over_map\":" + std::string(Bool(c.inside && c.hovered == 0 && c.map_read));
    out += ",\"province\":";
    if (c.province_read) {
        out += "{\"id\":" + std::to_string(c.province.id) + ",\"name\":" + Quote(c.province.name) +
               ",\"owner\":" + TagOrNull(c.province.owner) + ",\"land\":" + Bool(c.province.land) + "}";
    } else if (c.map_read && c.pick.province > 0) {
        out += "{\"id\":" + std::to_string(c.pick.province) + "}";
    } else {
        out += "null";
    }
    out += ",\"unit\":";
    if (c.unit_found) {
        out += "{\"id\":" + std::to_string(c.unit.id) + ",\"kind\":" +
               (c.unit.kind == kUnitNavy ? "\"navy\"" : "\"army\"") + ",\"tag\":" + Quote(c.unit_tag) +
               ",\"name\":" + Quote(c.unit.name) + ",\"location\":" +
               (c.unit.location > 0 ? std::to_string(c.unit.location) : std::string("null")) + "}";
    } else {
        out += "null";
    }
    return out;
}

}  // namespace tongs
