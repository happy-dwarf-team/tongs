#include "protocol.hpp"

#include "game_date.hpp"

#include <cstdio>
#include <utility>

namespace tongs {
namespace {

constexpr int kMaxSpeedPresses = 8;  // speed 1 -> 5 needs 4; the rest is slack for a lost press

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

// The state as JSON members without braces, shared by `state` and the input commands.
std::string StateFields(const StateSnapshot& s) {
    if (!s.in_game) {
        return std::string("\"in_game\":false,\"reason\":\"") + JsonEscape(s.reason ? s.reason : "") + "\"";
    }
    const GameDate date = DecodeGameDate(s.date_raw);
    std::string out = "\"in_game\":true,\"date\":\"" + FormatGameDate(date) + "\"";
    out += ",\"date_raw\":" + std::to_string(s.date_raw);
    out += ",\"hour\":" + std::to_string(date.hour);
    out += ",\"speed\":" + std::to_string(s.speed + 1);
    out += ",\"speed_internal\":" + std::to_string(s.speed);
    out += std::string(",\"paused\":") + Bool(s.paused);
    out += std::string(",\"paused_flag\":") + Bool(s.paused_flag);
    return out;
}

// Answer of an input command: what it did, then the state read afterwards.
struct ActionResult {
    bool ok = true;
    std::string error;
    int presses = 0;
    std::uint64_t waited_ms = 0;
    StateSnapshot state;
};

std::string ActionJson(std::string_view command, const ActionResult& r) {
    std::string out = std::string("{\"ok\":") + Bool(r.ok);
    if (!r.ok) out += ",\"error\":\"" + JsonEscape(r.error) + "\"";
    out += ",\"command\":\"" + JsonEscape(command) + "\"";
    out += ",\"presses\":" + std::to_string(r.presses);
    out += ",\"waited_ms\":" + std::to_string(r.waited_ms);
    out += "," + StateFields(r.state) + "}";
    return out;
}

class Actions {
public:
    explicit Actions(const Handlers& handlers) : h_(handlers) {}

    // Presses the key and reads the state after a short pause, without expecting a change.
    ActionResult Key(const KeyPress& key) {
        ActionResult r;
        const std::uint64_t start = h_.now_ms();
        if (!Press(key, r)) return Finish(r, start);
        h_.sleep_ms(h_.key_settle_ms);
        return Finish(r, start);
    }

    // Pause and resume act on the player pause (the pause button / Space).
    ActionResult SetPaused(bool pause) {
        ActionResult r;
        const std::uint64_t start = h_.now_ms();
        r.state = h_.read_state();
        if (!r.state.in_game) return Fail(r, start, "no game loaded");
        if (pause ? r.state.paused_flag : !r.state.paused) return Finish(r, start);
        if (!pause && !r.state.paused_flag) {
            // Space would set the player pause on top of whatever holds the game.
            return Fail(r, start, "the game is paused, but not by the player pause");
        }
        const KeyPress key = *FindKey(kPauseKeyName);
        if (!Press(key, r)) return Finish(r, start);
        const bool reached = WaitFor(r, [pause](const StateSnapshot& s) {
            return s.in_game && (pause ? s.paused_flag : !s.paused);
        });
        if (reached) return Finish(r, start);
        if (!pause && r.state.in_game && !r.state.paused_flag) {
            return Fail(r, start, "the player pause is off, but the game is still paused");
        }
        return Fail(r, start, std::string(pause ? "pause" : "resume") + ": no change within " +
                                  std::to_string(h_.wait_timeout_ms) + " ms");
    }

    // Presses speed up / down one step at a time until the UI speed is `target` (1..5).
    ActionResult SetSpeed(int target) {
        ActionResult r;
        const std::uint64_t start = h_.now_ms();
        r.state = h_.read_state();
        if (!r.state.in_game) return Fail(r, start, "no game loaded");
        const KeyPress up = *FindKey(kSpeedUpKeyName);
        const KeyPress down = *FindKey(kSpeedDownKeyName);
        while (r.state.speed + 1 != target) {
            if (r.presses >= kMaxSpeedPresses) return Fail(r, start, "speed: too many key presses");
            const int before = r.state.speed;
            if (!Press(r.state.speed + 1 < target ? up : down, r)) return Finish(r, start);
            const bool changed = WaitFor(r, [before](const StateSnapshot& s) {
                return !s.in_game || s.speed != before;
            });
            if (!r.state.in_game) return Fail(r, start, "game left while changing speed");
            if (!changed) {
                return Fail(r, start, "speed: no change within " + std::to_string(h_.wait_timeout_ms) + " ms");
            }
        }
        return Finish(r, start);
    }

private:
    bool Press(const KeyPress& key, ActionResult& r) {
        const std::string error = h_.press_key(key);
        if (!error.empty()) {
            r.ok = false;
            r.error = error;
            r.state = h_.read_state();
            return false;
        }
        ++r.presses;
        return true;
    }

    template <typename Pred>
    bool WaitFor(ActionResult& r, Pred done) {
        const std::uint64_t deadline = h_.now_ms() + static_cast<std::uint64_t>(h_.wait_timeout_ms);
        for (;;) {
            r.state = h_.read_state();
            if (done(r.state)) return true;
            if (h_.now_ms() >= deadline) return false;
            h_.sleep_ms(h_.poll_interval_ms);
        }
    }

    ActionResult& Finish(ActionResult& r, std::uint64_t start) {
        if (r.ok && r.presses > 0) r.state = h_.read_state();
        r.waited_ms = h_.now_ms() - start;
        return r;
    }

    ActionResult& Fail(ActionResult& r, std::uint64_t start, std::string error) {
        r.ok = false;
        r.error = std::move(error);
        r.waited_ms = h_.now_ms() - start;
        return r;
    }

    const Handlers& h_;
};

bool InputReady(const Handlers& h) { return h.press_key && h.read_state && h.sleep_ms && h.now_ms; }

constexpr char kInputDisabled[] = "input commands are disabled (set input = true in anvil/data/tongs/config.toml)";

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
    const std::string_view request = Trim(line);
    const std::size_t space = request.find_first_of(" \t");
    const std::string_view command = request.substr(0, space);
    const std::string_view argument = space == std::string_view::npos ? std::string_view{} : Trim(request.substr(space));

    if (command == "ping" && argument.empty()) {
        return std::string("{\"ok\":true,\"pong\":true,\"version\":\"") + TONGS_VERSION + "\",\"pid\":" +
               std::to_string(handlers.process_id) + "}";
    }
    if (command == "state" && argument.empty()) {
        if (!handlers.read_state) return Error("state reader unavailable");
        return "{\"ok\":true," + StateFields(handlers.read_state()) + "}";
    }
    if (command == "help" && argument.empty()) {
        return std::string("{\"ok\":true,\"commands\":[\"ping\",\"state\",\"help\",\"key\",\"pause\",\"resume\",") +
               "\"speed\"],\"input\":" + Bool(InputReady(handlers)) + "}";
    }
    if (command == "key" || command == "pause" || command == "resume" || command == "speed") {
        if (!InputReady(handlers)) return Error(kInputDisabled);
        Actions actions(handlers);
        if (command == "key") {
            const auto key = FindKey(argument);
            if (!key) return Error("unknown key: " + std::string(argument.substr(0, 32)));
            return ActionJson(command, actions.Key(*key));
        }
        if (command == "speed") {
            if (argument.size() != 1 || argument[0] < '1' || argument[0] > '5') {
                return Error("speed takes one number from 1 to 5");
            }
            return ActionJson(command, actions.SetSpeed(argument[0] - '0'));
        }
        if (!argument.empty()) return Error(std::string(command) + " takes no argument");
        return ActionJson(command, actions.SetPaused(command == "pause"));
    }
    if (request.empty()) return Error("empty command");
    return Error("unknown command: " + std::string(request.substr(0, 64)));
}

}  // namespace tongs
