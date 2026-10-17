#include "protocol.hpp"

#include "game_date.hpp"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace tongs {
namespace {

constexpr int kMaxSpeedPresses = 8;  // speed 1 -> 5 needs 4; the rest is slack for a lost press
constexpr int kDefaultRunTimeoutS = 3600;
constexpr int kMaxRunTimeoutS = 4 * 3600;

// Shared by all pipe clients: input commands take turns, and `cancel` ends
// every wait_until / run_until that started before it.
std::mutex g_input_mutex;
std::atomic<std::uint64_t> g_cancel_generation{0};

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

std::vector<std::string_view> SplitWords(std::string_view text) {
    std::vector<std::string_view> words;
    while (!text.empty()) {
        const std::size_t start = text.find_first_not_of(" 	");
        if (start == std::string_view::npos) break;
        text.remove_prefix(start);
        const std::size_t end = text.find_first_of(" 	");
        words.push_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
    }
    return words;
}

bool ParseInt(std::string_view text, int& out) {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::string DateText(std::int32_t raw) { return FormatGameDate(DecodeGameDate(raw)); }

// Game hours as a decimal number of days, e.g. 36 -> "1.5".
std::string Days(std::int64_t hours) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(hours) / 24.0);
    return buf;
}

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

// wait_until / run_until arguments: DATE [speed=N] [timeout=SECONDS].
struct RunRequest {
    std::int32_t target = 0;
    int speed = 0;  // 0 = keep the current speed
    int timeout_s = kDefaultRunTimeoutS;
};

std::string ParseRunRequest(std::string_view argument, bool allow_speed, RunRequest& out) {
    const std::vector<std::string_view> words = SplitWords(argument);
    if (words.empty()) return "expected a date such as 1720.1.1";
    const auto target = ParseGameDate(words[0]);
    if (!target) return "bad date: " + std::string(words[0].substr(0, 32)) + " (expected year.month.day[.hour])";
    out.target = *target;
    for (std::size_t i = 1; i < words.size(); ++i) {
        const std::string_view word = words[i];
        const std::size_t eq = word.find('=');
        const std::string_view key = word.substr(0, eq);
        const std::string_view value = eq == std::string_view::npos ? std::string_view{} : word.substr(eq + 1);
        int number = 0;
        if (eq == std::string_view::npos || !ParseInt(value, number)) {
            return "bad option: " + std::string(word.substr(0, 32)) + " (expected name=number)";
        }
        if (key == "timeout" && number >= 1 && number <= kMaxRunTimeoutS) {
            out.timeout_s = number;
        } else if (key == "speed" && allow_speed && number >= 1 && number <= 5) {
            out.speed = number;
        } else {
            return "bad option: " + std::string(word.substr(0, 32)) +
                   (allow_speed ? " (speed=1..5, timeout=1.." : " (timeout=1..") + std::to_string(kMaxRunTimeoutS) +
                   ")";
        }
    }
    return {};
}

// What a wait for a date observed. Times are wall-clock milliseconds.
struct WaitResult {
    bool reached = false;
    bool cancelled = false;
    bool timed_out = false;
    bool left_game = false;
    std::int32_t start_raw = 0;    // date when the wait started
    std::int32_t reached_raw = 0;  // first date read at or after the target
    std::uint64_t wall_ms = 0;     // start of the wait -> target first seen
    std::uint64_t paused_ms = 0;   // time seen paused while waiting
    std::uint64_t first_change_ms = 0;  // start of the wait -> first new date (0 = none)
    std::int32_t first_change_raw = 0;
    StateSnapshot state;
};

// Polls the date until it reaches `target`, a cancel, a timeout or the game is left.
WaitResult WaitForDate(const Handlers& h, std::int32_t target, int timeout_s) {
    WaitResult w;
    const std::uint64_t generation = g_cancel_generation.load();
    const std::uint64_t start = h.now_ms();
    const std::uint64_t deadline = start + static_cast<std::uint64_t>(timeout_s) * 1000;
    w.state = h.read_state();
    w.start_raw = w.state.date_raw;
    std::uint64_t last = start;
    for (;;) {
        const std::uint64_t now = h.now_ms();
        if (!w.state.in_game) {
            w.left_game = true;
            break;
        }
        if (w.state.paused) w.paused_ms += now - last;
        last = now;
        if (w.first_change_ms == 0 && w.state.date_raw != w.start_raw) {
            w.first_change_ms = now - start;
            w.first_change_raw = w.state.date_raw;
        }
        if (w.state.date_raw >= target) {
            w.reached = true;
            w.reached_raw = w.state.date_raw;
            w.wall_ms = now - start;
            break;
        }
        if (g_cancel_generation.load() != generation) {
            w.cancelled = true;
            break;
        }
        if (now >= deadline) {
            w.timed_out = true;
            break;
        }
        h.sleep_ms(h.poll_interval_ms);
        w.state = h.read_state();
    }
    if (!w.reached) w.wall_ms = h.now_ms() - start;
    return w;
}

std::string WaitFields(const WaitResult& w, std::int32_t target) {
    std::string out = ",\"target\":\"" + DateText(target) + "\"";
    out += ",\"start_date\":\"" + DateText(w.start_raw) + "\"";
    out += std::string(",\"reached\":") + Bool(w.reached);
    if (w.reached) {
        out += ",\"reached_date\":\"" + DateText(w.reached_raw) + "\"";
        out += ",\"reached_date_raw\":" + std::to_string(w.reached_raw);
        const std::int64_t hours = static_cast<std::int64_t>(w.reached_raw) - w.start_raw;
        out += ",\"game_days\":" + Days(hours);
        if (w.wall_ms > 0) {
            char rate[32];
            std::snprintf(rate, sizeof rate, "%.3f", static_cast<double>(hours) / 24.0 * 1000.0 / w.wall_ms);
            out += std::string(",\"days_per_second\":") + rate;
        }
    }
    out += ",\"wall_ms\":" + std::to_string(w.wall_ms);
    out += ",\"paused_ms\":" + std::to_string(w.paused_ms);
    if (w.first_change_ms > 0) {
        out += ",\"first_change_ms\":" + std::to_string(w.first_change_ms);
        out += ",\"first_change_date\":\"" + DateText(w.first_change_raw) + "\"";
    }
    return out;
}

const char* WaitError(const WaitResult& w) {
    if (w.left_game) return "the game was left while waiting";
    if (w.cancelled) return "cancelled";
    if (w.timed_out) return "timeout before the target date";
    return nullptr;
}

std::string WaitUntil(const Handlers& h, std::string_view argument) {
    RunRequest request;
    if (std::string error = ParseRunRequest(argument, false, request); !error.empty()) return Error(error);
    const StateSnapshot now = h.read_state();
    if (!now.in_game) return Error("no game loaded");
    const WaitResult w = WaitForDate(h, request.target, request.timeout_s);
    const char* error = WaitError(w);
    std::string out = std::string("{\"ok\":") + Bool(error == nullptr);
    if (error) out += ",\"error\":\"" + std::string(error) + "\"";
    out += ",\"command\":\"wait_until\"" + WaitFields(w, request.target) + "," + StateFields(w.state) + "}";
    return out;
}

// Sets the speed, resumes, waits for the date and pauses again. The wait runs
// without the input lock, so other clients can read the state or cancel.
std::string RunUntil(const Handlers& h, std::string_view argument) {
    RunRequest request;
    if (std::string error = ParseRunRequest(argument, true, request); !error.empty()) return Error(error);
    Actions actions(h);
    std::string fail;
    StateSnapshot before;
    {
        std::lock_guard lock(g_input_mutex);
        before = h.read_state();
        if (!before.in_game) return Error("no game loaded");
        if (before.date_raw >= request.target) return Error("the game is already at or past " + DateText(request.target));
        if (request.speed != 0) {
            const ActionResult speed = actions.SetSpeed(request.speed);
            if (!speed.ok) return ActionJson("run_until", speed);
        }
        const ActionResult resume = actions.SetPaused(false);
        if (!resume.ok) return ActionJson("run_until", resume);
    }
    const WaitResult w = WaitForDate(h, request.target, request.timeout_s);
    ActionResult pause;
    {
        std::lock_guard lock(g_input_mutex);
        pause = actions.SetPaused(true);
    }
    const char* error = WaitError(w);
    std::string message = error ? error : "";
    if (!pause.ok) message += std::string(message.empty() ? "" : "; ") + "pause after the run failed: " + pause.error;
    std::string out = std::string("{\"ok\":") + Bool(message.empty());
    if (!message.empty()) out += ",\"error\":\"" + JsonEscape(message) + "\"";
    out += ",\"command\":\"run_until\"" + WaitFields(w, request.target);
    out += ",\"start_speed\":" + std::to_string(before.speed + 1);
    out += ",\"overshoot_hours\":" +
           std::to_string(pause.state.in_game ? static_cast<std::int64_t>(pause.state.date_raw) - request.target : 0);
    out += "," + StateFields(pause.state) + "}";
    return out;
}

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
        return std::string("{\"ok\":true,\"commands\":[\"ping\",\"state\",\"help\",\"wait_until\",\"cancel\",") +
               "\"key\",\"pause\",\"resume\",\"speed\",\"run_until\"],\"input\":" + Bool(InputReady(handlers)) + "}";
    }
    if (command == "cancel" && argument.empty()) {
        g_cancel_generation.fetch_add(1);
        return "{\"ok\":true,\"command\":\"cancel\"}";
    }
    if (command == "wait_until") {
        if (!handlers.read_state || !handlers.sleep_ms || !handlers.now_ms) return Error("state reader unavailable");
        return WaitUntil(handlers, argument);
    }
    if (command == "run_until") {
        if (!InputReady(handlers)) return Error(kInputDisabled);
        return RunUntil(handlers, argument);
    }
    if (command == "key" || command == "pause" || command == "resume" || command == "speed") {
        if (!InputReady(handlers)) return Error(kInputDisabled);
        std::lock_guard lock(g_input_mutex);
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
