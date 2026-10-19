#include "protocol.hpp"

#include "game_date.hpp"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
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
// Input commands sent by any client, for `inputs` in wait_until / run_until.
std::atomic<std::uint64_t> g_inputs{0};
// Game calls made for any client, for `game_calls` in wait_until / run_until.
std::atomic<std::uint64_t> g_game_calls{0};
// Set by a game call that faulted; refuses every later game call until the game exits.
std::atomic<bool> g_game_calls_tripped{false};
// The pipe instance (from 1) whose request this thread is handling, for `activity`.
thread_local int t_client = 0;

// An `activity` notification, when anyone subscribes to them.
void PublishActivity(const Handlers& h, const std::string& members) {
    if (!h.notifications || !h.notifications->Wants(NotificationKind::Activity)) return;
    std::optional<std::int32_t> date;
    if (h.read_state) {
        const StateSnapshot s = h.read_state();
        if (s.in_game) date = s.date_raw;
    }
    h.notifications->Publish(NotificationKind::Activity, date, members + ",\"client\":" + std::to_string(t_client));
}

// Game calls since the save was loaded, for `game_calls_total` in `state`. A
// loaded save is told by the load the state reads saw (LoadWatch: a moment
// without a game in between), its session object, or a date earlier than the
// last call's. Guarded by g_total_mutex.
struct TotalCalls {
    std::uint64_t load = 0;
    std::uintptr_t session = 0;
    std::int32_t date_raw = 0;
    std::uint64_t count = 0;
};
std::mutex g_total_mutex;
TotalCalls g_total;

bool SameSave(const StateSnapshot& s) {
    return s.load == g_total.load && s.session == g_total.session && s.date_raw >= g_total.date_raw;
}

std::uint64_t TotalCallsFor(const StateSnapshot& s) {
    std::lock_guard lock(g_total_mutex);
    return SameSave(s) ? g_total.count : 0;
}

std::uint64_t AddTotalCall(const StateSnapshot& s) {
    std::lock_guard lock(g_total_mutex);
    if (!SameSave(s)) g_total = {s.load, s.session, s.date_raw, 0};
    g_total.date_raw = s.date_raw;
    return ++g_total.count;
}

// The handlers with a send_input that notices whether anything was queued; the
// command counts as one input when it was, and an `activity` notification
// names it.
class CountedInput {
public:
    CountedInput(const Handlers& h, std::string_view command) : handlers_(h), command_(command) {
        handlers_.send_input = [this, send = h.send_input](const std::vector<InputEvent>& events) {
            std::string error = send(events);
            if (error.empty()) sent_ = true;
            return error;
        };
    }
    CountedInput(const CountedInput&) = delete;
    CountedInput& operator=(const CountedInput&) = delete;
    ~CountedInput() {
        if (!sent_) return;
        const std::uint64_t total = g_inputs.fetch_add(1) + 1;
        PublishActivity(handlers_, ",\"what\":\"input\",\"command\":\"" + JsonEscape(command_) +
                                       "\",\"inputs\":" + std::to_string(total));
    }
    const Handlers& handlers() const { return handlers_; }

private:
    Handlers handlers_;
    std::string command_;
    bool sent_ = false;
};

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
    std::string extra;  // command-specific JSON members, each starting with a comma
    StateSnapshot state;
};

std::string ActionJson(std::string_view command, const ActionResult& r) {
    std::string out = std::string("{\"ok\":") + Bool(r.ok);
    if (!r.ok) out += ",\"error\":\"" + JsonEscape(r.error) + "\"";
    out += ",\"command\":\"" + JsonEscape(command) + "\"";
    out += ",\"presses\":" + std::to_string(r.presses);
    out += ",\"waited_ms\":" + std::to_string(r.waited_ms);
    out += r.extra;
    out += "," + StateFields(r.state) + "}";
    return out;
}

class Actions {
public:
    explicit Actions(const Handlers& handlers) : h_(handlers) {}

    // Queues the events and reads the state after a short pause, without expecting a change.
    ActionResult Settled(const std::vector<InputEvent>& events, int presses, std::string extra = {}) {
        ActionResult r;
        r.extra = std::move(extra);
        const std::uint64_t start = h_.now_ms();
        if (!Send(events, presses, r)) return Finish(r, start);
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
    bool Press(const KeyPress& key, ActionResult& r) { return Send(KeyPressEvents(key), 1, r); }

    bool Send(const std::vector<InputEvent>& events, int presses, ActionResult& r) {
        const std::string error = h_.send_input(events);
        if (!error.empty()) {
            r.ok = false;
            r.error = error;
            r.state = h_.read_state();
            return false;
        }
        r.presses += presses;
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

    // The state afterwards, also for commands that press nothing (`move`, a pause already on).
    ActionResult& Finish(ActionResult& r, std::uint64_t start) {
        if (r.ok) r.state = h_.read_state();
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

// wait_until / run_until arguments: DATE [speed=N] [timeout=SECONDS] [max_pause=SECONDS]
// [events=first|stop] (run_until only for speed and events).
struct RunRequest {
    std::int32_t target = 0;
    int speed = 0;  // 0 = keep the current speed
    int timeout_s = kDefaultRunTimeoutS;
    int max_pause_s = 0;  // stop when the game stays paused this long; 0 = wait until the timeout
    bool events_first = false;  // choose the first enabled option of every event that opens
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
        if (key == "events" && allow_speed && (value == "first" || value == "stop")) {
            out.events_first = value == "first";
            continue;
        }
        int number = 0;
        if (eq == std::string_view::npos || !ParseInt(value, number)) {
            return "bad option: " + std::string(word.substr(0, 32)) + " (expected name=number)";
        }
        if (key == "timeout" && number >= 1 && number <= kMaxRunTimeoutS) {
            out.timeout_s = number;
        } else if (key == "max_pause" && number >= 1 && number <= kMaxRunTimeoutS) {
            out.max_pause_s = number;
        } else if (key == "speed" && allow_speed && number >= 1 && number <= 5) {
            out.speed = number;
        } else {
            return "bad option: " + std::string(word.substr(0, 32)) +
                   (allow_speed ? " (speed=1..5, events=first|stop, timeout and max_pause 1.." : " (timeout and max_pause 1..") +
                   std::to_string(kMaxRunTimeoutS) + ")";
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
    bool pause_limit = false;      // stopped by max_pause
    std::int32_t pause_raw = 0;    // date of the last pause that began while waiting
    std::int32_t start_raw = 0;    // date when the wait started
    std::int32_t reached_raw = 0;  // first date read at or after the target
    std::uint64_t wall_ms = 0;     // start of the wait -> target first seen
    std::uint64_t paused_ms = 0;   // time seen paused while waiting
    std::uint64_t first_change_ms = 0;  // start of the wait -> first new date (0 = none)
    std::int32_t first_change_raw = 0;
    std::uint64_t inputs = 0;      // input commands of any client during the wait
    std::uint64_t game_calls = 0;  // game calls of any client during the wait
    bool stopped = false;          // the poll hook ended the wait
    StateSnapshot state;
};

// Called between the polls of a wait with the game in it; returns false to end the wait.
using PollHook = std::function<bool(WaitResult&)>;

// Polls the date until it reaches `target`, a cancel, a timeout or the game is left.
WaitResult WaitForDate(const Handlers& h, std::int32_t target, int timeout_s, int max_pause_s, const PollHook& hook = {}) {
    WaitResult w;
    const std::uint64_t generation = g_cancel_generation.load();
    const std::uint64_t inputs = g_inputs.load();
    const std::uint64_t game_calls = g_game_calls.load();
    const std::uint64_t start = h.now_ms();
    const std::uint64_t deadline = start + static_cast<std::uint64_t>(timeout_s) * 1000;
    w.state = h.read_state();
    w.start_raw = w.state.date_raw;
    std::uint64_t last = start;
    std::uint64_t paused_since = 0;  // start of the current pause; 0 = running
    for (;;) {
        const std::uint64_t now = h.now_ms();
        if (!w.state.in_game) {
            w.left_game = true;
            break;
        }
        if (w.state.paused) {
            w.paused_ms += now - last;
            if (paused_since == 0) {
                paused_since = now;
                w.pause_raw = w.state.date_raw;
            }
        } else {
            paused_since = 0;
        }
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
        if (max_pause_s > 0 && paused_since != 0 && now - paused_since >= static_cast<std::uint64_t>(max_pause_s) * 1000) {
            w.pause_limit = true;
            break;
        }
        if (now >= deadline) {
            w.timed_out = true;
            break;
        }
        if (hook && !hook(w)) {
            w.stopped = true;
            break;
        }
        h.sleep_ms(h.poll_interval_ms);
        w.state = h.read_state();
    }
    if (!w.reached) w.wall_ms = h.now_ms() - start;
    w.inputs = g_inputs.load() - inputs;
    w.game_calls = g_game_calls.load() - game_calls;
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
    out += ",\"inputs\":" + std::to_string(w.inputs);
    out += ",\"game_calls\":" + std::to_string(w.game_calls);
    if (w.paused_ms > 0) out += ",\"paused_date\":\"" + DateText(w.pause_raw) + "\"";
    if (w.first_change_ms > 0) {
        out += ",\"first_change_ms\":" + std::to_string(w.first_change_ms);
        out += ",\"first_change_date\":\"" + DateText(w.first_change_raw) + "\"";
    }
    return out;
}

const char* WaitError(const WaitResult& w) {
    if (w.left_game) return "the game was left while waiting";
    if (w.cancelled) return "cancelled";
    if (w.pause_limit) return "the game stayed paused longer than max_pause";
    if (w.timed_out) return "timeout before the target date";
    return nullptr;
}

std::string WaitUntil(const Handlers& h, std::string_view argument) {
    RunRequest request;
    if (std::string error = ParseRunRequest(argument, false, request); !error.empty()) return Error(error);
    const StateSnapshot now = h.read_state();
    if (!now.in_game) return Error("no game loaded");
    const WaitResult w = WaitForDate(h, request.target, request.timeout_s, request.max_pause_s);
    const char* error = WaitError(w);
    std::string out = std::string("{\"ok\":") + Bool(error == nullptr);
    if (error) out += ",\"error\":\"" + std::string(error) + "\"";
    out += ",\"command\":\"wait_until\"" + WaitFields(w, request.target) + "," + StateFields(w.state) + "}";
    return out;
}

std::string Hex(std::uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "\"0x%llX\"", static_cast<unsigned long long>(value));
    return buf;
}

std::string ThreadsJson(const ThreadsSnapshot& t) {
    std::string out = std::string("{\"ok\":") + Bool(t.ok);
    if (!t.ok) out += ",\"error\":\"" + JsonEscape(t.reason ? t.reason : "") + "\"";
    out += ",\"threads_setting\":" + std::to_string(t.threads_setting) + ",\"pools\":[";
    for (int i = 0; i < 2; ++i) {
        if (i) out += ",";
        char rva[16];
        std::snprintf(rva, sizeof rva, "0x%X", t.pools[i].rva);
        out += std::string("{\"rva\":\"") + rva + "\",\"workers\":" + std::to_string(t.pools[i].workers) +
               ",\"capacity\":" + std::to_string(t.pools[i].capacity) + "}";
    }
    out += "],\"workers_equal\":" + std::string(Bool(t.ok && t.pools[0].workers == t.pools[1].workers));
    out += ",\"process_mask\":" + Hex(t.process_mask) + ",\"system_mask\":" + Hex(t.system_mask);
    out += ",\"process_threads\":" + std::to_string(t.process_threads);
    out += ",\"main_thread_id\":" + std::to_string(t.main_thread_id);
    if (t.main_thread_id != 0) {
        out += ",\"main_mask\":" + Hex(t.main_mask) + ",\"main_ideal_cpu\":" + std::to_string(t.main_ideal_cpu) +
               ",\"main_priority\":" + std::to_string(t.main_priority);
    }
    return out + "}";
}

std::string WindowFields(const WindowSnapshot& w) {
    if (!w.found) return std::string("\"found\":false,\"reason\":\"") + JsonEscape(w.reason ? w.reason : "") + "\"";
    std::string out = "\"found\":true";
    out += ",\"width\":" + std::to_string(w.width) + ",\"height\":" + std::to_string(w.height);
    out += ",\"screen_x\":" + std::to_string(w.screen_x) + ",\"screen_y\":" + std::to_string(w.screen_y);
    out += std::string(",\"focused\":") + Bool(w.focused) + ",\"minimized\":" + Bool(w.minimized);
    out += ",\"thread_id\":" + std::to_string(w.thread_id) + ",\"dpi\":" + std::to_string(w.dpi);
    out += ",\"handle\":" + std::to_string(w.handle);
    return out;
}

// The last mouse position sent, for the relative motion of the next move.
// Guarded by g_input_mutex.
struct MouseTrack {
    bool known = false;
    int x = 0;
    int y = 0;
};
MouseTrack g_mouse;

InputEvent MoveTo(int x, int y) {
    InputEvent e;
    e.type = InputEvent::Type::MouseMove;
    e.x = x;
    e.y = y;
    if (g_mouse.known) {
        e.xrel = x - g_mouse.x;
        e.yrel = y - g_mouse.y;
    }
    g_mouse = {true, x, y};
    return e;
}

// Checks a point against the game window's client area, when the window is known.
std::string CheckPoint(const Handlers& h, int x, int y) {
    if (x < 0 || y < 0) return "coordinates must not be negative";
    if (!h.read_window) return {};
    const WindowSnapshot w = h.read_window();
    if (!w.found) return w.reason ? w.reason : "no game window";
    if (x >= w.width || y >= w.height) {
        return "(" + std::to_string(x) + ", " + std::to_string(y) + ") is outside the " + std::to_string(w.width) +
               "x" + std::to_string(w.height) + " client area";
    }
    return {};
}

// Puts the GUI pointer at (x, y) and describes the point for the answer.
std::string PointAt(const Handlers& h, int x, int y) {
    if (h.set_cursor) h.set_cursor(x, y);
    return ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"cursor\":\"" +
           (h.set_cursor ? "virtual" : "events only") + "\"";
}

constexpr int kClickMoveSettleMs = 50;  // a few frames between moving the pointer and pressing
constexpr int kHoverAttempts = 3;       // moves before `covered` when the game reports no hovered widget
constexpr int kMaxScrollSteps = 20;

std::string MouseCommand(const Handlers& h, std::string_view command, std::string_view argument) {
    const std::vector<std::string_view> words = SplitWords(argument);
    Actions actions(h);
    if (command == "move") {
        int x = 0, y = 0;
        if (words.size() != 2 || !ParseInt(words[0], x) || !ParseInt(words[1], y)) return Error("usage: move X Y");
        if (std::string error = CheckPoint(h, x, y); !error.empty()) return Error(error);
        const std::string point = PointAt(h, x, y);
        return ActionJson(command, actions.Settled({MoveTo(x, y)}, 0, point));
    }
    if (command == "click") {
        int x = 0, y = 0;
        if (words.size() < 2 || words.size() > 4 || !ParseInt(words[0], x) || !ParseInt(words[1], y)) {
            return Error("usage: click X Y [left|right|middle] [double]");
        }
        int button = 1;
        int count = 1;
        for (std::size_t i = 2; i < words.size(); ++i) {
            if (words[i] == "left") {
                button = 1;
            } else if (words[i] == "middle") {
                button = 2;
            } else if (words[i] == "right") {
                button = 3;
            } else if (words[i] == "double") {
                count = 2;
            } else {
                return Error("usage: click X Y [left|right|middle] [double]");
            }
        }
        if (std::string error = CheckPoint(h, x, y); !error.empty()) return Error(error);
        // Move first and let the game see the pointer there, then press and release.
        const std::string point = PointAt(h, x, y);
        ActionResult moved = actions.Settled({MoveTo(x, y)}, 0);
        if (!moved.ok) return ActionJson(command, moved);
        h.sleep_ms(kClickMoveSettleMs);
        std::vector<InputEvent> presses;
        for (int c = 1; c <= count; ++c) {
            InputEvent down;
            down.type = InputEvent::Type::MouseDown;
            down.x = x;
            down.y = y;
            down.button = button;
            down.clicks = c;
            InputEvent up = down;
            up.type = InputEvent::Type::MouseUp;
            presses.push_back(down);
            presses.push_back(up);
        }
        static const char* const kNames[] = {"", "left", "middle", "right"};
        return ActionJson(command, actions.Settled(presses, count,
                                                   point + ",\"button\":\"" + kNames[button] +
                                                       "\",\"clicks\":" + std::to_string(count)));
    }
    // scroll STEPS [X Y]
    int steps = 0;
    if ((words.size() != 1 && words.size() != 3) || !ParseInt(words[0], steps) || steps == 0 ||
        steps < -kMaxScrollSteps || steps > kMaxScrollSteps) {
        return Error("usage: scroll STEPS [X Y], STEPS -20..20 and not 0 (positive scrolls up)");
    }
    std::vector<InputEvent> events;
    std::string extra = ",\"steps\":" + std::to_string(steps);
    if (words.size() == 3) {
        int x = 0, y = 0;
        if (!ParseInt(words[1], x) || !ParseInt(words[2], y)) return Error("usage: scroll STEPS [X Y]");
        if (std::string error = CheckPoint(h, x, y); !error.empty()) return Error(error);
        extra += PointAt(h, x, y);
        events.push_back(MoveTo(x, y));
    }
    const int one = steps > 0 ? 1 : -1;
    for (int i = 0; i != steps; i += one) {
        InputEvent wheel;
        wheel.type = InputEvent::Type::Wheel;
        wheel.wheel = one;
        events.push_back(wheel);
    }
    return ActionJson(command, actions.Settled(events, steps > 0 ? steps : -steps, extra));
}

std::string TextCommand(const Handlers& h, std::string_view text) {
    if (text.empty()) return Error("usage: text STRING");
    std::vector<InputEvent> events;
    if (std::string error = TextEvents(text, events); !error.empty()) return Error(error);
    Actions actions(h);
    const int count = static_cast<int>(events.size());
    return ActionJson("text", actions.Settled(events, count, ",\"characters\":" + std::to_string(count)));
}

bool InputReady(const Handlers& h) { return h.send_input && h.read_state && h.sleep_ms && h.now_ms; }

constexpr char kInputDisabled[] = "input commands are disabled (set input = true in anvil/data/tongs/config.toml)";
constexpr char kNoDispatch[] = "main-thread commands are unavailable (the frame hook is not installed)";
constexpr int kMaxDispatchTimeoutS = 600;

// Answer of a main-thread job: the job's own members, then when and where it ran.
std::string MainThreadJson(std::string_view command, const Dispatcher::Outcome& o) {
    std::string out = std::string("{\"ok\":") + Bool(o.ran);
    if (!o.ran) out += ",\"error\":\"" + JsonEscape(o.error) + "\"";
    out += ",\"command\":\"" + JsonEscape(command) + "\"";
    if (o.ran) out += o.answer;
    out += ",\"queued_ms\":" + std::to_string(o.queued_ms);
    if (o.ran) out += ",\"run_ms\":" + std::to_string(o.run_ms) + ",\"frame\":" + std::to_string(o.frame);
    return out + "}";
}

// frames [timeout=SECONDS]: frame counts, read on the main thread after an outer frame.
std::string FramesCommand(const Handlers& h, std::string_view argument) {
    if (!h.dispatcher || !h.frame_stats || !h.now_ms) return Error(kNoDispatch);
    int timeout_ms = h.dispatch_timeout_ms;
    if (!argument.empty()) {
        int seconds = 0;
        if (!argument.starts_with("timeout=") || !ParseInt(argument.substr(8), seconds) || seconds < 1 ||
            seconds > kMaxDispatchTimeoutS) {
            return Error("frames takes an optional timeout=SECONDS (1-600)");
        }
        timeout_ms = seconds * 1000;
    }
    FrameStats& stats = *h.frame_stats;
    const auto& now = h.now_ms;
    const auto& thread_id = h.current_thread_id;
    const Dispatcher::Outcome outcome = h.dispatcher->Run(
        Dispatcher::Kind::Read,
        [&stats, &now, &thread_id] {
            const FrameStats::Snapshot s = stats.Take(now());
            const std::string thread = thread_id ? ",\"thread_id\":" + std::to_string(thread_id()) : std::string();
            return thread + ",\"outer_per_s\":" + std::to_string(s.outer_per_s) + ",\"nested_per_s\":" +
                   std::to_string(s.nested_per_s) + ",\"outer_since\":" + std::to_string(s.outer_since) +
                   ",\"nested_since\":" + std::to_string(s.nested_since) + ",\"since_ms\":" + std::to_string(s.since_ms) +
                   ",\"longest_gap_ms\":" + std::to_string(s.longest_gap_ms) + ",\"outer_total\":" +
                   std::to_string(s.outer_total) + ",\"nested_total\":" + std::to_string(s.nested_total);
        },
        timeout_ms);
    return MainThreadJson("frames", outcome);
}

std::string Quote(std::string_view text) { return "\"" + JsonEscape(text) + "\""; }

std::string DispatchTimes(const Dispatcher::Outcome& o) {
    return ",\"queued_ms\":" + std::to_string(o.queued_ms) + ",\"run_ms\":" + std::to_string(o.run_ms) +
           ",\"frame\":" + std::to_string(o.frame);
}

// The widget tree, and with it the open events, read in one main-thread job.
struct Screen {
    UiTree tree;
    EventsRaw raw;
    std::vector<EventView> events;
    std::string error;  // a reader's
    Dispatcher::Outcome outcome;

    std::string Error() const { return outcome.ran ? error : outcome.error; }
};

// False when the job did not run or a reader failed; Screen::Error() says why.
bool ReadScreen(const Handlers& h, int timeout_ms, bool with_events, Screen& s) {
    s = {};
    const auto& read_ui = h.read_ui;
    const auto& read_events = h.read_events;
    s.outcome = h.dispatcher->Run(
        Dispatcher::Kind::Read,
        [&s, &read_ui, &read_events, with_events] {
            s.error = read_ui(s.tree);
            if (s.error.empty() && with_events) s.error = read_events(s.raw);
            return std::string();
        },
        timeout_ms);
    if (!s.outcome.ran || !s.error.empty()) return false;
    if (with_events) s.events = BuildEvents(s.tree, s.raw);
    return true;
}

// The answer of a command whose main-thread read failed.
std::string ScreenFailure(std::string_view command, const Screen& s) {
    if (!s.outcome.ran) return MainThreadJson(command, s.outcome);
    return "{\"ok\":false,\"error\":" + Quote(s.error) + ",\"command\":" + Quote(command) + DispatchTimes(s.outcome) + "}";
}

// What clicking a widget needs: input, the cursor hook and main-thread reads.
std::string ClickUnavailable(const Handlers& h) {
    if (!InputReady(h)) return kInputDisabled;
    if (!h.dispatcher || !h.read_ui || !h.read_hovered) return kNoDispatch;
    if (!h.set_cursor) return "clicking a widget needs the cursor hook, which is not installed";
    return {};
}

struct ClickOutcome {
    bool ok = false;
    std::string error;    // "covered", or an error text
    std::string members;  // the point and the hover check
    std::uint64_t clicked_at = 0;  // when the button events were queued
    std::uint64_t frame = 0;       // the frame of the hover check
    StateSnapshot state;
};

// Moves the GUI pointer to the centre of the widget, checks on the main thread
// that the game hovers the widget (or one inside it) there, then presses and
// releases the left button. With `settle`, the state is read a moment after the
// click; event choices watch the event instead. The caller holds the input lock.
ClickOutcome ClickWidget(const Handlers& h, std::uintptr_t target, const UiRect& rect, int timeout_ms, bool settle) {
    ClickOutcome c;
    const int x = rect.x + rect.w / 2;
    const int y = rect.y + rect.h / 2;
    c.members = ",\"point\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}";
    const auto fail = [&c, &h](std::string error) -> ClickOutcome {
        c.error = std::move(error);
        c.state = h.read_state();
        return c;
    };
    if (std::string error = CheckPoint(h, x, y); !error.empty()) return fail(error);
    h.set_cursor(x, y);
    UiCoverCheck cover;
    for (int attempt = 1;; ++attempt) {
        if (attempt > 1) {
            // A move to where the pointer already is may not make the game pick again
            // (the misses came on repeated clicks on a scroll bar button); step one pixel aside first.
            const int aside = x + 1 < rect.x + rect.w ? x + 1 : x - 1;
            h.set_cursor(aside, y);
            if (std::string error = h.send_input({MoveTo(aside, y)}); !error.empty()) return fail(error);
            h.sleep_ms(kClickMoveSettleMs);
            h.set_cursor(x, y);
        }
        if (std::string error = h.send_input({MoveTo(x, y)}); !error.empty()) return fail(error);
        h.sleep_ms(kClickMoveSettleMs);
        // The game picks the widget under the pointer once per frame. A job may run
        // after a frame that began before the move; one queued after it returned cannot.
        Dispatcher::Outcome o = h.dispatcher->Run(Dispatcher::Kind::Read, [] { return std::string(); }, timeout_ms);
        UiTree tree;
        std::uintptr_t hovered = 0;
        std::string read_error;
        if (o.ran) {
            o = h.dispatcher->Run(
                Dispatcher::Kind::Read,
                [&] {
                    read_error = h.read_ui(tree);
                    hovered = h.read_hovered();
                    return std::string();
                },
                timeout_ms);
        }
        if (!o.ran) return fail(o.error);
        if (!read_error.empty()) return fail(read_error);
        c.frame = o.frame;
        cover = CheckCover(tree, target, hovered);
        // Live, the game sometimes had no hovered widget at all for the first move
        // after a click on a scroll bar button; the same move again found the target.
        if (!cover.covered || hovered != 0 || attempt == kHoverAttempts) {
            if (attempt > 1) c.members += ",\"hover_attempts\":" + std::to_string(attempt);
            break;
        }
    }
    if (cover.covered) {
        c.members += ",\"covered_by\":" + (cover.covered_by.empty() ? std::string("null") : Quote(cover.covered_by));
        return fail("covered");
    }
    if (!cover.hovered.empty()) c.members += ",\"hovered\":" + Quote(cover.hovered);
    InputEvent down;
    down.type = InputEvent::Type::MouseDown;
    down.x = x;
    down.y = y;
    InputEvent up = down;
    up.type = InputEvent::Type::MouseUp;
    if (std::string error = h.send_input({down, up}); !error.empty()) return fail(error);
    c.clicked_at = h.now_ms();
    if (settle) h.sleep_ms(h.key_settle_ms);
    c.ok = true;
    c.state = h.read_state();
    return c;
}

int TimeoutOr(const Handlers& h, int timeout_ms) { return timeout_ms > 0 ? timeout_ms : h.dispatch_timeout_ms; }

void NoteChoice(const Handlers& h, const EventView& e, const EventOption& o, const StateSnapshot& before,
                const char* via = nullptr);

// ui click SEL [timeout=S]: resolves the selector on the main thread, then clicks.
std::string UiClick(const Handlers& handlers, const UiRequest& request) {
    if (std::string error = ClickUnavailable(handlers); !error.empty()) return Error(error);
    std::lock_guard lock(g_input_mutex);
    const CountedInput counted(handlers, "ui click");
    const Handlers& h = counted.handlers();
    const int timeout_ms = TimeoutOr(h, request.timeout_ms);
    const std::uint64_t start = h.now_ms();
    // With event windows open, the events too: a click on an option is a choice Tongs can name.
    const bool events = h.read_events && h.count_event_windows && h.count_event_windows() > 0;
    Screen s;
    if (!ReadScreen(h, timeout_ms, events, s)) return ScreenFailure("ui click", s);
    const std::string head = ",\"command\":\"ui click\",\"selector\":" + Quote(request.selector_text);
    const UiClickTarget target = ResolveClickTarget(s.tree, request.selector);
    if (!target.error.empty()) {
        std::string out = "{\"ok\":false,\"error\":\"" + target.error + "\"" + head;
        if (target.widget != -1) return out + ",\"path\":" + Quote(target.path) + "}";
        out += ",\"count\":" + std::to_string(target.count) + ",\"matches\":[";
        for (std::size_t i = 0; i < target.paths.size(); ++i) out += (i ? "," : "") + Quote(target.paths[i]);
        return out + "]}";
    }
    const UiWidget& w = s.tree.widgets[target.widget];
    const EventView* event = nullptr;
    const EventOption* option = nullptr;
    for (const EventView& e : s.events) {
        for (const EventOption& o : e.options) {
            if (o.widget == target.widget) {
                event = &e;
                option = &o;
            }
        }
    }
    const StateSnapshot before = option ? h.read_state() : StateSnapshot{};
    const ClickOutcome c = ClickWidget(h, w.address, w.rect, timeout_ms, true);
    if (c.ok && option) NoteChoice(h, *event, *option, before, "ui click");
    std::string out = std::string("{\"ok\":") + Bool(c.ok);
    if (!c.ok) out += ",\"error\":" + Quote(c.error);
    out += head + (c.ok ? ",\"clicked\":" : ",\"path\":") + Quote(target.path) + c.members;
    if (option) {
        out += ",\"event_option\":{\"id\":" + Quote(event->id) + ",\"number\":" + std::to_string(option->number) +
               ",\"option_index\":" + std::to_string(option->index) + "}";
    }
    out += ",\"waited_ms\":" + std::to_string(h.now_ms() - start);
    return out + "," + StateFields(c.state) + "}";
}

constexpr int kCloseLimitMs = 3000;   // how long a chosen event may take to close
constexpr int kMaxOptionScrolls = 30;  // scroll bar clicks to bring a hidden option into its list box
constexpr int kOptionWaitMs = 10000;  // how long events=first waits for an enabled option
constexpr int kOptionPollMs = 100;
constexpr int kEventLookMs = 250;     // events=first: between reads while the window list is not empty

std::string EventList(const Handlers& h, const EventRequest& r) {
    if (!h.dispatcher || !h.read_ui || !h.read_events || !h.now_ms) return Error(kNoDispatch);
    Screen s;
    if (!ReadScreen(h, TimeoutOr(h, r.timeout_ms), true, s)) return ScreenFailure("event list", s);
    std::string out = "{\"ok\":true,\"command\":\"event list\",\"count\":" + std::to_string(s.events.size()) + ",\"events\":[";
    for (std::size_t i = 0; i < s.events.size(); ++i) out += (i ? "," : "") + EventJson(s.events[i]);
    out += "],\"controllers\":" + std::to_string(s.raw.events.size()) +
           ",\"listed_windows\":" + std::to_string(s.raw.listed_windows.size());
    if (s.raw.has_delay) {
        char delay[32];
        std::snprintf(delay, sizeof delay, "%.3f", static_cast<double>(s.raw.option_delay));
        out += std::string(",\"option_delay\":") + delay;
    }
    return out + DispatchTimes(s.outcome) + "}";
}

struct CloseWatch {
    bool chosen = false;  // the controller stopped waiting or left the pop-up list
    bool closed = false;  // it left the pop-up list and its window the event window list
    std::uint64_t chosen_ms = 0;
    std::uint64_t closed_ms = 0;
    std::uint64_t closed_frames = 0;  // frames after the hover check
};

// Polls the open events on the main thread after a click on an option.
CloseWatch WatchClose(const Handlers& h, const EventView& event, std::uint64_t since, std::uint64_t frame) {
    CloseWatch w;
    for (;;) {
        EventsRaw raw;
        std::string error;
        const auto& read = h.read_events;
        const Dispatcher::Outcome o = h.dispatcher->Run(
            Dispatcher::Kind::Read,
            [&raw, &error, &read] {
                error = read(raw);
                return std::string();
            },
            kCloseLimitMs);
        const std::uint64_t now = h.now_ms();
        if (o.ran && error.empty()) {
            const OpenEventRaw* open = nullptr;
            for (const OpenEventRaw& e : raw.events) {
                if (e.controller == event.controller) open = &e;
            }
            bool listed = false;
            for (const std::uintptr_t window : raw.listed_windows) listed = listed || window == event.window_address;
            if (!w.chosen && (!open || !open->waiting)) {
                w.chosen = true;
                w.chosen_ms = now - since;
            }
            if (!open && !listed) {
                w.closed = true;
                w.closed_ms = now - since;
                w.closed_frames = o.frame > frame ? o.frame - frame : 0;
                return w;
            }
        }
        if (now - since >= static_cast<std::uint64_t>(kCloseLimitMs)) return w;
        h.sleep_ms(h.poll_interval_ms);
    }
}

std::string CloseFields(const CloseWatch& w) {
    std::string out = std::string(",\"closed\":") + Bool(w.closed);
    if (w.chosen) out += ",\"chosen_ms\":" + std::to_string(w.chosen_ms);
    if (w.closed) out += ",\"closed_ms\":" + std::to_string(w.closed_ms) + ",\"closed_frames\":" + std::to_string(w.closed_frames);
    return out;
}

// For the `closed` event notification: the option Tongs clicked, noted before
// the game handles the click.
// `via` names the command when it is not event choose / run_until (a plain ui click on the option).
void NoteChoice(const Handlers& h, const EventView& e, const EventOption& o, const StateSnapshot& before,
                const char* via) {
    if (!h.notifications) return;
    std::string json = "{\"number\":" + std::to_string(o.number) + ",\"text\":" + Quote(o.text) +
                       ",\"option_index\":" + std::to_string(o.index) + ",\"option_key\":" + Quote(o.key) +
                       ",\"by\":\"tongs\",\"client\":" + std::to_string(t_client);
    if (via) json += ",\"via\":" + Quote(via);
    if (before.in_game) json += ",\"choice_date\":\"" + DateText(before.date_raw) + "\"";
    h.notifications->NoteChoice(e.controller, e.id, json + "}");
}

void ForgetChoice(const Handlers& h, const EventView& e) {
    if (h.notifications) h.notifications->TakeChoice(e.controller, e.id);
}

std::string NotChosen() {
    return "the event was still waiting for a choice " + std::to_string(kCloseLimitMs) + " ms after the click";
}

// event choose N | text^="..." [event=N | event=title^="..."] [timeout=S]
std::string EventChoose(const Handlers& handlers, const EventRequest& r) {
    if (std::string error = ClickUnavailable(handlers); !error.empty()) return Error(error);
    if (!handlers.read_events) return Error(kNoDispatch);
    std::lock_guard lock(g_input_mutex);
    const CountedInput counted(handlers, "event choose");
    const Handlers& h = counted.handlers();
    const int timeout_ms = TimeoutOr(h, r.timeout_ms);
    const std::uint64_t start = h.now_ms();
    Screen s;
    if (!ReadScreen(h, timeout_ms, true, s)) return ScreenFailure("event choose", s);
    EventChoice choice = ChooseEvent(s.events, r);
    // An option outside the list box: scroll it in with the list's own scroll bar buttons.
    int scrolled = 0;
    std::string scroll_error;
    while (choice.error == "not_visible" && scrolled < kMaxOptionScrolls) {
        const int b = OptionScrollButton(s.tree, s.events[choice.hidden_event], choice.hidden_option);
        if (b == -1) break;
        const ClickOutcome sc = ClickWidget(h, s.tree.widgets[b].address, s.tree.widgets[b].rect, timeout_ms, true);
        if (!sc.ok) {
            scroll_error = sc.error;
            break;
        }
        ++scrolled;
        if (!ReadScreen(h, timeout_ms, true, s)) return ScreenFailure("event choose", s);
        choice = ChooseEvent(s.events, r);
    }
    const std::string scroll_members = (scrolled ? ",\"scrolled\":" + std::to_string(scrolled) : std::string()) +
                                       (scroll_error.empty() ? std::string() : ",\"scroll_error\":" + Quote(scroll_error));
    if (!choice.error.empty()) {
        return "{\"ok\":false,\"error\":\"" + choice.error + "\",\"command\":\"event choose\"" + choice.members +
               scroll_members + "}";
    }
    const EventView& e = s.events[choice.event];
    const EventOption& o = e.options[choice.option];
    const StateSnapshot before = h.read_state();
    const UiWidget& button = s.tree.widgets[o.widget];
    const ClickOutcome c = ClickWidget(h, button.address, button.rect, timeout_ms, false);
    std::string error = c.error;
    std::string fields = c.members;
    StateSnapshot state = c.state;
    if (c.ok) {
        NoteChoice(h, e, o, before);
        const CloseWatch w = WatchClose(h, e, c.clicked_at, c.frame);
        if (!w.chosen) {
            error = NotChosen();
            ForgetChoice(h, e);
        }
        fields += CloseFields(w);
        state = h.read_state();
    }
    std::string out = std::string("{\"ok\":") + Bool(error.empty());
    if (!error.empty()) out += ",\"error\":" + Quote(error);
    out += ",\"command\":\"event choose\"";
    if (before.in_game) out += ",\"choice_date\":\"" + DateText(before.date_raw) + "\"";
    out += ChoiceMembers(e, o) + scroll_members + fields + ",\"waited_ms\":" + std::to_string(h.now_ms() - start);
    return out + "," + StateFields(state) + "}";
}

std::string EventCommand(const Handlers& h, std::string_view argument) {
    EventRequest request;
    if (std::string error = ParseEventRequest(argument, request); !error.empty()) {
        const bool choose = argument.starts_with("choose");
        return "{\"ok\":false" + error + ",\"command\":\"" + (choose ? "event choose" : "event") + "\"}";
    }
    return request.verb == EventRequest::Verb::List ? EventList(h, request) : EventChoose(h, request);
}

// run_until events=first: between the polls of the wait, chooses the first
// enabled option of the topmost open event, as `event choose` would.
struct EventsFirst {
    const Handlers& h;
    std::string choices;  // JSON objects, comma separated
    std::string error;    // why the run ends
    std::string members;  // with the error: the event
    std::uint64_t next_look = 0;

    bool Stop(std::string why) {
        error = std::move(why);
        return false;
    }

    bool Poll() {
        // One word from the pipe thread; the main thread is asked only while events are open.
        if (h.now_ms() < next_look || h.count_event_windows() <= 0) return true;
        std::lock_guard lock(g_input_mutex);
        Screen s;
        if (!ReadScreen(h, h.dispatch_timeout_ms, true, s)) return Stop(s.Error());
        if (s.events.empty()) {
            next_look = h.now_ms() + kEventLookMs;
            return true;
        }
        const std::uintptr_t controller = s.events[0].controller;
        const EventView* e = &s.events[0];
        int option = FirstEnabledOption(*e);
        // The game keeps the options disabled for a moment after the window opens.
        const std::uint64_t deadline = h.now_ms() + kOptionWaitMs;
        while (option == -1 && h.now_ms() < deadline) {
            h.sleep_ms(kOptionPollMs);
            if (!ReadScreen(h, h.dispatch_timeout_ms, true, s)) return Stop(s.Error());
            e = nullptr;
            for (const EventView& v : s.events) {
                if (v.controller == controller) e = &v;
            }
            if (!e) return true;  // chosen meanwhile
            option = FirstEnabledOption(*e);
        }
        if (option == -1) {
            members = ",\"event\":" + EventJson(*e);
            return Stop("no_enabled_option");
        }
        const EventOption& o = e->options[option];
        const StateSnapshot before = h.read_state();
        const UiWidget& button = s.tree.widgets[o.widget];
        ClickOutcome c;
        {
            const CountedInput counted(h, "run_until events=first: choice");
            c = ClickWidget(counted.handlers(), button.address, button.rect, h.dispatch_timeout_ms, false);
        }
        if (c.ok) NoteChoice(h, *e, o, before);
        if (!c.ok) {
            members = ",\"event\":" + EventJson(*e) + c.members;
            return Stop(c.error);
        }
        if (!choices.empty()) choices += ',';
        choices += ChoiceJson(*e, o, before.in_game ? DateText(before.date_raw) : std::string());
        const CloseWatch w = WatchClose(h, *e, c.clicked_at, c.frame);
        if (!w.chosen) {
            ForgetChoice(h, *e);
            members = ",\"event\":" + EventJson(*e);
            return Stop(NotChosen());
        }
        // An event that paused the game with the player pause leaves it paused.
        const StateSnapshot after = h.read_state();
        if (after.in_game && after.paused_flag) {
            const CountedInput counted(h, "run_until events=first: resume");
            Actions actions(counted.handlers());
            const ActionResult resume = actions.SetPaused(false);
            if (!resume.ok) return Stop("resume after choosing an event failed: " + resume.error);
        }
        next_look = 0;
        return true;
    }
};

// Sets the speed, resumes, waits for the date and pauses again. The wait runs
// without the input lock, so other clients can read the state or cancel.
std::string RunUntil(const Handlers& h, std::string_view argument) {
    RunRequest request;
    if (std::string error = ParseRunRequest(argument, true, request); !error.empty()) return Error(error);
    if (request.events_first) {
        std::string missing = ClickUnavailable(h);
        if (missing.empty() && (!h.read_events || !h.count_event_windows)) missing = kNoDispatch;
        if (!missing.empty()) return Error("events=first: " + missing);
    }
    Actions actions(h);
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
    EventsFirst events{h};
    PollHook hook;
    if (request.events_first) hook = [&events](WaitResult&) { return events.Poll(); };
    const WaitResult w = WaitForDate(h, request.target, request.timeout_s, request.max_pause_s, hook);
    ActionResult pause;
    {
        std::lock_guard lock(g_input_mutex);
        pause = actions.SetPaused(true);
    }
    const char* wait_error = WaitError(w);
    std::string message = w.stopped ? events.error : wait_error ? wait_error : "";
    if (!pause.ok) message += std::string(message.empty() ? "" : "; ") + "pause after the run failed: " + pause.error;
    std::string out = std::string("{\"ok\":") + Bool(message.empty());
    if (!message.empty()) out += ",\"error\":\"" + JsonEscape(message) + "\"";
    out += ",\"command\":\"run_until\"" + WaitFields(w, request.target);
    if (request.events_first) out += ",\"events\":[" + events.choices + "]" + events.members;
    out += ",\"start_speed\":" + std::to_string(before.speed + 1);
    if (w.reached && pause.state.in_game) {
        // Game hours past the target when the pause took hold.
        out += ",\"overshoot_hours\":" + std::to_string(static_cast<std::int64_t>(pause.state.date_raw) - request.target);
    }
    out += "," + StateFields(pause.state) + "}";
    return out;
}

// ui find SEL [all] [limit=N] / ui tree [SEL] [depth=N] [limit=N] [all]: the selector is
// checked on the pipe thread, the tree is read on the main thread, and the answer is
// built back on the pipe thread.
std::string UiCommand(const Handlers& h, std::string_view argument) {
    UiRequest request;
    const std::string parse_error = ParseUiRequest(argument, request);
    const std::string_view verb = argument.substr(0, argument.find_first_of(" 	"));
    const std::string command = verb == "find" || verb == "tree" || verb == "click" ? "ui " + std::string(verb) : "ui";
    if (!parse_error.empty()) return "{\"ok\":false" + parse_error + ",\"command\":\"" + command + "\"}";
    if (request.verb == UiRequest::Verb::Click) return UiClick(h, request);
    if (!h.dispatcher || !h.read_ui || !h.now_ms) return Error(kNoDispatch);
    UiTree tree;
    std::string read_error;
    const auto& read = h.read_ui;
    const Dispatcher::Outcome outcome = h.dispatcher->Run(
        Dispatcher::Kind::Read,
        [&tree, &read_error, &read] {
            read_error = read(tree);
            return std::string();
        },
        request.timeout_ms > 0 ? request.timeout_ms : h.dispatch_timeout_ms);
    if (!outcome.ran) return MainThreadJson(command, outcome);
    bool found = true;
    const std::string members = read_error.empty() ? UiAnswer(tree, request, found) : std::string();
    std::string out = std::string("{\"ok\":") + Bool(read_error.empty() && found);
    if (!read_error.empty()) out += ",\"error\":\"" + JsonEscape(read_error) + "\"";
    if (!found) out += ",\"error\":\"not_found\"";
    out += ",\"command\":\"" + command + "\"" + members;
    out += ",\"queued_ms\":" + std::to_string(outcome.queued_ms) + ",\"run_ms\":" + std::to_string(outcome.run_ms) +
           ",\"frame\":" + std::to_string(outcome.frame);
    return out + "}";
}

constexpr int kLogInfo = 1;
constexpr int kLogWarning = 2;
constexpr int kLogError = 3;
constexpr std::size_t kLogText = 200;  // characters of a game call's text in the log

// Game calls: one console line run on the main thread (ADR 0001).
class ConsoleCall {
public:
    ConsoleCall(const Handlers& h, std::string_view line, int client) : h_(h), line_(line), client_(client) {}

    std::string Run() {
        if (!h_.game_calls.enabled) {
            return Refuse("game_calls_disabled",
                          "game calls are disabled (set game_calls = true in anvil/data/tongs/config.toml)");
        }
        if (!h_.dispatcher || !h_.read_console || !h_.call_console || !h_.read_state || !h_.now_ms) {
            return Refuse("unavailable", kNoDispatch);
        }
        if (std::string error = SplitConsoleLine(line_, tokens_); !error.empty()) return Refuse("bad_command", error);
        // The typed name first, without the main thread; the game's own name follows there.
        if (ConsoleRefusal r = CheckConsoleName(h_.game_calls, tokens_[0]); r.refused()) return Refuse(r.error, r.detail);
        if (g_game_calls_tripped.load()) return Refuse("game_calls_tripped", kTripped);

        std::lock_guard lock(g_input_mutex);  // game calls take turns with input
        outcome_ = h_.dispatcher->Run(Dispatcher::Kind::GameCall, [this] { return OnMainThread(); }, h_.dispatch_timeout_ms);
        if (!outcome_.ran) return Refuse(outcome_.error, "the main thread did not run the game call");
        if (!refusal_.error.empty()) return Refuse(refusal_.error, refusal_.detail);
        std::uint64_t total = 0;
        if (called_) {
            const std::uint64_t calls = g_game_calls.fetch_add(1) + 1;
            total = AddTotalCall(before_);
            PublishActivity(h_, ",\"what\":\"game_call\",\"command\":\"console\",\"line\":" + Quoted(line_) +
                                    ",\"name\":" + Quoted(name_) + ",\"game_calls\":" + std::to_string(calls));
        }
        const StateSnapshot after = h_.read_state();
        const std::string text = GameTextToUtf8(result_.text);
        if (result_.faulted) {
            char code[16];
            std::snprintf(code, sizeof code, "0x%08X", result_.fault_code);
            const std::string detail = std::string("exception ") + code + " in the game call; " + kTripped;
            Log(kLogError, "faulted (" + detail + ")");
            return Answer(false, "faulted", detail) + ",\"game_calls_total\":" + std::to_string(total) + Tail(after);
        }
        const bool mode = raw_.ironman || raw_.multiplayer;
        Log(mode ? kLogWarning : kLogInfo,
            std::string("called") + (mode ? " in " + ModeText() : std::string()) + ", success byte " +
                (result_.success ? "1" : "0") + ", text \"" + JsonEscape(text.substr(0, kLogText)) + "\"");
        std::string out = Answer(true, {}, {});
        out += ",\"text\":" + Quoted(text) + ",\"success_hint\":" + Bool(result_.success);
        out += ",\"game_calls_total\":" + std::to_string(total);
        return out + Tail(after);
    }

private:
    static constexpr char kTripped[] = "game calls are off until the game exits after a game call faulted";

    // Main thread, after an outer frame: checks the game, then calls.
    std::string OnMainThread() {
        before_ = h_.read_state();
        if (!before_.in_game) {
            refusal_ = {"not_in_game", before_.reason ? before_.reason : "no game loaded"};
            return {};
        }
        if (std::string error = h_.read_console(raw_); !error.empty()) {
            refusal_ = {"console_unavailable", error};
            return {};
        }
        if (!raw_.manager || raw_.entries.empty()) {
            refusal_ = {"console_unavailable", "the game's console manager is not set up"};
            return {};
        }
        if (ConsoleRefusal r = CheckConsoleMode(h_.game_calls, raw_); r.refused()) {
            refusal_ = r;
            return {};
        }
        const ConsoleEntry* entry = FindConsoleEntry(raw_.entries, tokens_[0]);
        if (!entry) {
            refusal_ = {"unknown_command", "the game has no console command " + tokens_[0]};
            return {};
        }
        name_ = entry->name;
        if (ConsoleRefusal r = CheckConsoleName(h_.game_calls, name_); r.refused()) {
            refusal_ = r;
            return {};
        }
        if (g_game_calls_tripped.load()) {
            refusal_ = {"game_calls_tripped", kTripped};
            return {};
        }
        h_.call_console(tokens_, result_);
        called_ = true;
        if (result_.faulted) g_game_calls_tripped.store(true);
        return {};
    }

    std::string ModeText() const {
        return raw_.ironman && raw_.multiplayer ? "ironman multiplayer" : raw_.ironman ? "ironman" : "multiplayer";
    }

    void Log(int level, const std::string& what) const {
        if (!h_.log) return;
        std::string date = "no game";
        const StateSnapshot& s = before_.in_game ? before_ : (h_.read_state ? h_.read_state() : before_);
        if (s.in_game) date = DateText(s.date_raw);
        h_.log(level, "console \"" + JsonEscape(line_.substr(0, kLogText)) + "\" (pipe " + std::to_string(client_) + ", " +
                          date + "): " + what);
    }

    std::string Refuse(const std::string& error, const std::string& detail) {
        Log(kLogWarning, error + " (" + detail + ")");
        const StateSnapshot state = h_.read_state ? h_.read_state() : StateSnapshot{};
        return Answer(false, error, detail) + (h_.read_state ? Tail(state) : "}");
    }

    std::string Quoted(std::string_view text) const { return "\"" + JsonEscape(text) + "\""; }

    // The members up to the command's own: ok, error, command, line, name.
    std::string Answer(bool ok, const std::string& error, const std::string& detail) const {
        std::string out = std::string("{\"ok\":") + Bool(ok);
        if (!ok) out += ",\"error\":" + Quoted(error) + ",\"detail\":" + Quoted(detail);
        out += ",\"command\":\"console\",\"line\":" + Quoted(line_);
        if (!name_.empty()) out += ",\"name\":" + Quoted(name_);
        // Only once the main thread has read the mode; a refusal before that has none.
        if ((h_.game_calls.ironman || h_.game_calls.multiplayer) && raw_.session != 0) {
            out += std::string(",\"mode\":{\"ironman\":") + Bool(raw_.ironman) + ",\"multiplayer\":" + Bool(raw_.multiplayer) +
                   ",\"game_calls_ironman\":" + Bool(h_.game_calls.ironman) +
                   ",\"game_calls_multiplayer\":" + Bool(h_.game_calls.multiplayer) + "}";
        }
        if (raw_.manager) {
            out += ",\"console\":{\"commands\":" + std::to_string(raw_.entries.size()) +
                   ",\"developer_lock\":" + std::to_string(raw_.developer_lock) +
                   ",\"mode_lock\":" + std::to_string(raw_.mode_lock) + "}";
        }
        return out;
    }

    // Dispatch times when the job ran, then the state; closes the object.
    std::string Tail(const StateSnapshot& state) const {
        std::string out = outcome_.ran ? DispatchTimes(outcome_) : std::string();
        return out + "," + StateFields(state) + "}";
    }

    const Handlers& h_;
    std::string_view line_;
    int client_;
    std::vector<std::string> tokens_;
    Dispatcher::Outcome outcome_;
    ConsoleRefusal refusal_;
    StateSnapshot before_;
    ConsoleRaw raw_;
    std::string name_;
    ConsoleCallResult result_;
    bool called_ = false;
};

std::string Ping(const Handlers& h) {
    return std::string("{\"ok\":true,\"command\":\"ping\",\"pong\":true,\"version\":\"") + TONGS_VERSION +
           "\",\"pid\":" + std::to_string(h.process_id) + "}";
}

// Splits a request line into its command word and the rest.
std::pair<std::string_view, std::string_view> SplitCommand(std::string_view line) {
    const std::string_view request = Trim(line);
    const std::size_t space = request.find_first_of(" \t");
    const std::string_view command = request.substr(0, space);
    const std::string_view argument = space == std::string_view::npos ? std::string_view{} : Trim(request.substr(space));
    return {command, argument};
}

std::string Refused(std::string_view command, std::string_view error, std::string_view detail) {
    return "{\"ok\":false,\"error\":" + Quote(error) + ",\"detail\":" + Quote(detail) + ",\"command\":" +
           Quote(command.substr(0, 64)) + "}";
}

// Whether the plugin can serve a kind (the others answer `unavailable` in `subscribe`).
bool Served(const Handlers& h, NotificationKind kind) {
    switch (kind) {
        case NotificationKind::Event: return h.dispatcher && h.read_ui && h.read_events && h.count_event_windows;
        case NotificationKind::ErrorLog: return h.locate_error_log && h.read_log_chunk;
        case NotificationKind::Activity: return true;
        default: return static_cast<bool>(h.read_state);
    }
}

}  // namespace

bool IsSubscribe(std::string_view line) { return SplitCommand(line).first == "subscribe"; }

std::string Subscribe(std::string_view line, const Handlers& h, std::shared_ptr<Subscription>& out) {
    const std::string_view argument = SplitCommand(line).second;
    if (!h.notifications) return Refused("subscribe", "unavailable", "notifications are not set up");
    SubscribeRequest request;
    if (std::string error = ParseSubscribe(argument, request); !error.empty()) {
        return Refused("subscribe", "bad_command", error);
    }
    std::string kinds;
    std::string unavailable;
    for (unsigned bit = 1; bit <= kAllNotificationKinds; bit <<= 1) {
        if ((request.kinds & bit) == 0) continue;
        const auto kind = static_cast<NotificationKind>(bit);
        const std::string name = std::string("\"") + KindName(kind) + "\"";
        kinds += (kinds.empty() ? "" : ",") + name;
        if (!Served(h, kind)) unavailable += (unavailable.empty() ? "" : ",") + name;
    }
    std::string answer = "{\"ok\":true,\"command\":\"subscribe\",\"kinds\":[" + kinds + "]";
    if (request.kinds & Bit(NotificationKind::Date)) {
        answer += std::string(",\"date_unit\":\"") + DateUnitName(request.date_unit) + "\"";
    }
    answer += ",\"queue\":" + std::to_string(kNotificationQueue);
    if (!unavailable.empty()) answer += ",\"unavailable\":[" + unavailable + "]";
    if ((request.kinds & Bit(NotificationKind::ErrorLog)) && h.locate_error_log) {
        const ErrorLogLocation where = h.locate_error_log();
        answer += where.error.empty()
                      ? ",\"error_log\":{\"path\":" + Quote(where.path) + ",\"source\":" + Quote(where.source) + "}"
                      : ",\"error_log\":{\"error\":" + Quote(where.error) + "}";
    }
    // The state the notifications start from.
    if (h.read_state) answer += "," + StateFields(h.read_state());
    out = h.notifications->Subscribe(request);
    return answer + "}";
}

std::string SubscribedRequest(std::string_view line, const Handlers& h, std::shared_ptr<Subscription>& subscription) {
    const auto [command, argument] = SplitCommand(line);
    if (command == "ping" && argument.empty()) return Ping(h);
    if (command == "unsubscribe" && argument.empty()) {
        if (h.notifications && subscription) h.notifications->Unsubscribe(subscription);
        const std::size_t discarded = subscription ? subscription->Discard() : 0;
        const Subscription::Counts counts = subscription ? subscription->counts() : Subscription::Counts{};
        subscription.reset();
        return "{\"ok\":true,\"command\":\"unsubscribe\",\"sent\":" + std::to_string(counts.sent) +
               ",\"dropped\":" + std::to_string(counts.dropped) + ",\"discarded\":" + std::to_string(discarded) + "}";
    }
    if (command.empty()) return Refused("", "bad_command", "empty command");
    return Refused(command, "subscribed", "a subscribed connection accepts only unsubscribe and ping");
}

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

std::string HandleRequest(std::string_view line, const Handlers& handlers, int client) {
    t_client = client;
    const std::string_view request = Trim(line);
    const auto [command, argument] = SplitCommand(request);

    if (command == "ping" && argument.empty()) return Ping(handlers);
    if (command == "subscribe") return Error("subscribe needs a pipe connection of its own");
    if (command == "unsubscribe" && argument.empty()) return Error("not subscribed");
    if (command == "state" && argument.empty()) {
        if (!handlers.read_state) return Error("state reader unavailable");
        const StateSnapshot s = handlers.read_state();
        std::string out = "{\"ok\":true," + StateFields(s);
        if (s.in_game) out += ",\"game_calls_total\":" + std::to_string(TotalCallsFor(s));
        return out + "}";
    }
    if (command == "screenshot") {
        if (!handlers.take_screenshot) return Error("screenshots unavailable");
        if (argument.size() > 48 || argument.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
                                        std::string_view::npos) {
            return Error("screenshot takes an optional name of 1-48 letters, digits, _ or -");
        }
        const ScreenshotResult shot = handlers.take_screenshot(argument);
        if (!shot.ok) return Error(shot.error);
        return "{\"ok\":true,\"command\":\"screenshot\",\"path\":\"" + JsonEscape(shot.path) + "\",\"width\":" +
               std::to_string(shot.width) + ",\"height\":" + std::to_string(shot.height) + ",\"bytes\":" +
               std::to_string(shot.bytes) + ",\"method\":\"" + shot.method + "\",\"blank\":" + Bool(shot.blank) + "}";
    }
    if (command == "threads" && argument.empty()) {
        if (!handlers.read_threads) return Error("thread reader unavailable");
        return ThreadsJson(handlers.read_threads());
    }
    if (command == "window" && argument.empty()) {
        if (!handlers.read_window) return Error("window reader unavailable");
        return "{\"ok\":true," + WindowFields(handlers.read_window()) + "}";
    }
    if (command == "help" && argument.empty()) {
        return std::string("{\"ok\":true,\"commands\":[\"ping\",\"state\",\"window\",\"threads\",\"screenshot\",\"frames\",\"ui\",\"event\",\"help\",") +
               "\"wait_until\",\"cancel\",\"key\",\"text\",\"move\",\"click\",\"scroll\",\"release\",\"pause\",\"resume\",\"speed\"," +
               "\"run_until\",\"console\",\"subscribe\",\"unsubscribe\"],\"input\":" + Bool(InputReady(handlers)) + ",\"main_thread\":" +
               Bool(handlers.dispatcher != nullptr) + ",\"game_calls\":" + Bool(handlers.game_calls.enabled) +
               ",\"game_calls_tripped\":" + Bool(g_game_calls_tripped.load()) + "}";
    }
    if (command == "cancel" && argument.empty()) {
        g_cancel_generation.fetch_add(1);
        const int withdrawn = handlers.dispatcher ? handlers.dispatcher->CancelQueued() : 0;
        return "{\"ok\":true,\"command\":\"cancel\",\"withdrawn\":" + std::to_string(withdrawn) + "}";
    }
    if (command == "frames") return FramesCommand(handlers, argument);
    if (command == "ui") return UiCommand(handlers, argument);
    if (command == "event") return EventCommand(handlers, argument);
    if (command == "console") return ConsoleCall(handlers, argument, client).Run();
    if (command == "wait_until") {
        if (!handlers.read_state || !handlers.sleep_ms || !handlers.now_ms) return Error("state reader unavailable");
        return WaitUntil(handlers, argument);
    }
    if (command == "run_until") {
        if (!InputReady(handlers)) return Error(kInputDisabled);
        return RunUntil(handlers, argument);
    }
    if (command == "release" && argument.empty()) {
        if (!InputReady(handlers)) return Error(kInputDisabled);
        if (handlers.release_cursor) handlers.release_cursor();
        return "{\"ok\":true,\"command\":\"release\"}";
    }
    if (command == "text" || command == "move" || command == "click" || command == "scroll") {
        if (!InputReady(handlers)) return Error(kInputDisabled);
        std::lock_guard lock(g_input_mutex);
        const CountedInput counted(handlers, command);
        if (command == "text") return TextCommand(counted.handlers(), argument);
        return MouseCommand(counted.handlers(), command, argument);
    }
    if (command == "key" || command == "pause" || command == "resume" || command == "speed") {
        if (!InputReady(handlers)) return Error(kInputDisabled);
        std::lock_guard lock(g_input_mutex);
        const CountedInput counted(handlers, command);
        Actions actions(counted.handlers());
        if (command == "key") {
            const auto key = FindKey(argument);
            if (!key) return Error("unknown key: " + std::string(argument.substr(0, 32)));
            return ActionJson(command, actions.Settled(KeyPressEvents(*key), 1));
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
