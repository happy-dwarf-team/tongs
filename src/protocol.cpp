#include "protocol.hpp"

#include "game_date.hpp"

#include <cstdio>

namespace lc {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

const char* Bool(bool value) { return value ? "true" : "false"; }

std::string Error(std::string_view message) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(message) + "\"}";
}

std::string StateJson(const StateSnapshot& s) {
    if (!s.in_game) {
        return std::string("{\"ok\":true,\"in_game\":false,\"reason\":\"") + JsonEscape(s.reason ? s.reason : "") +
               "\"}";
    }
    const GameDate date = DecodeGameDate(s.date_raw);
    std::string out = "{\"ok\":true,\"in_game\":true,\"date\":\"" + FormatGameDate(date) + "\"";
    out += ",\"date_raw\":" + std::to_string(s.date_raw);
    out += ",\"hour\":" + std::to_string(date.hour);
    out += ",\"speed\":" + std::to_string(s.speed + 1);
    out += ",\"speed_internal\":" + std::to_string(s.speed);
    out += std::string(",\"paused\":") + Bool(s.paused);
    out += std::string(",\"paused_flag\":") + Bool(s.paused_flag);
    out += "}";
    return out;
}

}  // namespace

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

std::string HandleRequest(std::string_view line, const Handlers& handlers) {
    const std::string_view command = Trim(line);
    if (command == "ping") {
        return std::string("{\"ok\":true,\"pong\":true,\"version\":\"") + LC_VERSION + "\",\"pid\":" +
               std::to_string(handlers.process_id) + "}";
    }
    if (command == "state") {
        if (!handlers.read_state) return Error("state reader unavailable");
        return StateJson(handlers.read_state());
    }
    if (command == "help") {
        return "{\"ok\":true,\"commands\":[\"ping\",\"state\",\"help\"]}";
    }
    if (command.empty()) return Error("empty command");
    return Error("unknown command: " + std::string(command.substr(0, 64)));
}

}  // namespace lc
