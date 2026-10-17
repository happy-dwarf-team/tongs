#include "config.hpp"
#include "game_date.hpp"
#include "protocol.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace tongs;

static bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

static void TestDate() {
    // 1719.3.17 0:00: (1719 + 5000) years * 365 + 75 days, in hours.
    const GameDate d = DecodeGameDate(58'860'240);
    assert(d.year == 1719 && d.month == 3 && d.day == 17 && d.hour == 0);
    assert(FormatGameDate(d) == "1719.3.17");

    const GameDate start = DecodeGameDate(43'800'000 + 1444 * 365 * 24 + 314 * 24);  // 11 November 1444
    assert(start.year == 1444 && start.month == 11 && start.day == 11);

    const GameDate last = DecodeGameDate(43'800'000 + 1820 * 365 * 24 + 364 * 24 + 23);
    assert(last.year == 1820 && last.month == 12 && last.day == 31 && last.hour == 23);

    const GameDate before_zero = DecodeGameDate(43'800'000 - 24);  // the day before 0.1.1
    assert(before_zero.year == -1 && before_zero.month == 12 && before_zero.day == 31 && before_zero.hour == 0);
}

static void TestConfig() {
    assert(!ParseConfig("").enabled);
    assert(ParseConfig(kDefaultConfig).notes.empty());
    assert(!ParseConfig(kDefaultConfig).enabled);
    assert(ParseConfig("enabled = true\r\n").enabled);
    assert(ParseConfig("  enabled=true # comment\n").enabled);

    const Config bad = ParseConfig("enabled = yes\nport = 5\nnonsense\n");
    assert(!bad.enabled);
    assert(bad.notes.size() == 3);

    assert(!ParseConfig(kDefaultConfig).input);
    const Config both = ParseConfig("enabled = true\ninput = true\n");
    assert(both.enabled && both.input && both.notes.empty());
    assert(ParseConfig("input = 1\n").notes.size() == 1);
}

static void TestProtocol() {
    Handlers h;
    h.process_id = 1234;
    StateSnapshot snapshot;
    h.read_state = [&] { return snapshot; };

    assert(Contains(HandleRequest("ping\r\n", h), "\"pong\":true"));
    assert(Contains(HandleRequest("ping", h), "\"pid\":1234"));
    assert(Contains(HandleRequest("help", h), "\"state\""));
    assert(Contains(HandleRequest("", h), "\"ok\":false"));
    assert(Contains(HandleRequest("pausee", h), "unknown command: pausee"));

    snapshot.reason = "root is null";
    const std::string out_menu = HandleRequest("state", h);
    assert(Contains(out_menu, "\"in_game\":false") && Contains(out_menu, "root is null"));

    snapshot = {};
    snapshot.in_game = true;
    snapshot.date_raw = 58'860'240;
    snapshot.speed = 4;
    snapshot.paused = true;
    snapshot.paused_flag = true;
    const std::string out = HandleRequest(" state ", h);
    assert(Contains(out, "\"date\":\"1719.3.17\""));
    assert(Contains(out, "\"speed\":5"));
    assert(Contains(out, "\"speed_internal\":4"));
    assert(Contains(out, "\"paused\":true"));

    assert(JsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
}

static void TestKeys() {
    assert(FindKey("space")->sym == ' ' && FindKey("SPACE")->scancode == 44);
    assert(FindKey("a")->sym == 'a' && FindKey("a")->scancode == 4);
    assert(FindKey("Z")->sym == 'z' && FindKey("z")->scancode == 29);
    assert(FindKey("1")->scancode == 30 && FindKey("9")->scancode == 38 && FindKey("0")->scancode == 39);
    assert(FindKey("f1")->scancode == 58 && FindKey("f1")->sym == (58 | (1 << 30)));
    assert(FindKey("F12")->scancode == 69);
    assert(FindKey("f")->sym == 'f');
    assert(!FindKey("f13") && !FindKey("f0") && !FindKey("fx") && !FindKey("") && !FindKey("spacebar"));
    assert(FindKey("kp_plus")->sym == 0x40000057 && FindKey("kp_plus")->scancode == 87);
    assert(FindKey("kp_minus")->sym == 0x40000056 && FindKey("kp_minus")->scancode == 86);
    assert(FindKey("escape")->sym == 27 && FindKey("return")->sym == '\r');
    assert(FindKey(kPauseKeyName) && FindKey(kSpeedUpKeyName) && FindKey(kSpeedDownKeyName));
}

// A game that reacts to pause and speed keys a few polls after each press,
// like the real main loop does on its next frame.
struct FakeGame {
    std::uint64_t clock = 0;
    StateSnapshot state;
    bool held_by_game = false;  // a pause reason other than the player pause
    int react_after_ms = 30;
    bool ignore_keys = false;
    std::string push_error;
    std::vector<std::pair<std::uint64_t, KeyPress>> pending;
    std::vector<std::int32_t> pressed;
    bool clock_runs_date = false;  // the date advances (UI speed) hours per ms while not paused
    std::uint64_t date_clock = 0;
    std::function<void()> on_sleep;

    void Apply() {
        if (clock_runs_date) {
            if (!state.paused) state.date_raw += static_cast<std::int32_t>((clock - date_clock) * (state.speed + 1));
            date_clock = clock;
        }
        while (!pending.empty() && pending.front().first <= clock) {
            const KeyPress key = pending.front().second;
            pending.erase(pending.begin());
            if (ignore_keys) continue;
            if (key.sym == ' ') state.paused_flag = !state.paused_flag;
            if (key.sym == FindKey(kSpeedUpKeyName)->sym && state.speed < 4) ++state.speed;
            if (key.sym == FindKey(kSpeedDownKeyName)->sym && state.speed > 0) --state.speed;
        }
        state.paused = state.paused_flag || held_by_game;
    }

    Handlers Make() {
        Handlers h;
        h.read_state = [this] { Apply(); return state; };
        h.press_key = [this](const KeyPress& key) -> std::string {
            if (!push_error.empty()) return push_error;
            pressed.push_back(key.sym);
            pending.emplace_back(clock + react_after_ms, key);
            return {};
        };
        h.sleep_ms = [this](int ms) {
            Apply();  // the date runs up to now at the old pause state
            clock += static_cast<std::uint64_t>(ms);
            if (on_sleep) on_sleep();
        };
        h.now_ms = [this] { return clock; };
        return h;
    }
};

static FakeGame LoadedGame() {
    FakeGame game;
    game.state.in_game = true;
    game.state.date_raw = 58'860'240;
    game.state.speed = 1;
    game.state.paused_flag = true;
    game.state.paused = true;
    return game;
}

static void TestInputDisabled() {
    FakeGame game = LoadedGame();
    Handlers h = game.Make();
    h.press_key = nullptr;
    for (const char* command : {"key space", "pause", "resume", "speed 3"}) {
        const std::string out = HandleRequest(command, h);
        assert(Contains(out, "\"ok\":false") && Contains(out, "input = true"));
    }
    assert(Contains(HandleRequest("help", h), "\"input\":false"));
    assert(game.pressed.empty());
}

static void TestPauseResume() {
    FakeGame game = LoadedGame();
    const Handlers h = game.Make();
    assert(Contains(HandleRequest("help", h), "\"input\":true"));

    // Already paused: nothing is pressed.
    std::string out = HandleRequest("pause", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"presses\":0") && Contains(out, "\"paused_flag\":true"));
    assert(game.pressed.empty());

    out = HandleRequest("resume", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"command\":\"resume\"") && Contains(out, "\"presses\":1"));
    assert(Contains(out, "\"paused\":false") && Contains(out, "\"date\":\"1719.3.17\""));
    assert(game.pressed.size() == 1 && game.pressed[0] == ' ');

    out = HandleRequest("pause", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"presses\":1") && Contains(out, "\"paused\":true"));

    // Paused by the game, not the player: Space would add a player pause, so nothing is pressed.
    game.state.paused_flag = false;
    game.held_by_game = true;
    const std::size_t before = game.pressed.size();
    out = HandleRequest("resume", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "not by the player pause"));
    assert(game.pressed.size() == before);
    // `pause` still sets the player pause on top of it.
    out = HandleRequest("pause", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"paused_flag\":true"));

    // Player pause cleared, but the game holds the pause.
    out = HandleRequest("resume", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "still paused") && Contains(out, "\"paused_flag\":false"));
}

static void TestPauseTimeoutAndErrors() {
    FakeGame game = LoadedGame();
    game.ignore_keys = true;
    Handlers h = game.Make();
    h.wait_timeout_ms = 500;
    std::string out = HandleRequest("resume", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "no change within 500 ms") && Contains(out, "\"paused\":true"));
    assert(Contains(out, "\"waited_ms\":5"));  // 500 ms and a poll interval, from the fake clock

    game.ignore_keys = false;
    game.push_error = "SDL is not ready";
    out = HandleRequest("resume", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "SDL is not ready") && Contains(out, "\"presses\":0"));

    FakeGame menu;
    menu.state.reason = "no game loaded (root object is null)";
    const Handlers hm = menu.Make();
    out = HandleRequest("pause", hm);
    assert(Contains(out, "\"ok\":false") && Contains(out, "no game loaded") && Contains(out, "\"in_game\":false"));
    // A raw key press works without a loaded game (menus).
    out = HandleRequest("key escape", hm);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"presses\":1") && Contains(out, "\"in_game\":false"));
}

static void TestSpeed() {
    FakeGame game = LoadedGame();  // UI speed 2
    const Handlers h = game.Make();
    std::string out = HandleRequest("speed 5", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"presses\":3") && Contains(out, "\"speed\":5"));
    out = HandleRequest("speed 1", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"presses\":4") && Contains(out, "\"speed\":1"));
    out = HandleRequest("speed 1", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"presses\":0"));
    // Speed changes while paused, as with the UI buttons; the pause stays.
    assert(Contains(out, "\"paused\":true"));

    for (const char* bad : {"speed", "speed 0", "speed 6", "speed 12", "speed x"}) {
        assert(Contains(HandleRequest(bad, h), "from 1 to 5"));
    }
    assert(Contains(HandleRequest("pause now", h), "takes no argument"));
    assert(Contains(HandleRequest("key nosuchkey", h), "unknown key: nosuchkey"));

    game.ignore_keys = true;
    out = HandleRequest("speed 3", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "speed: no change") && Contains(out, "\"presses\":1"));
}

static void TestParseDate() {
    assert(ParseGameDate("1719.3.17") == 58'860'240);
    assert(ParseGameDate("1719.3.17.5") == 58'860'245);
    assert(ParseGameDate("1444.11.11") == 43'800'000 + 1444 * 365 * 24 + 314 * 24);
    assert(*ParseGameDate("1720.1.1") - *ParseGameDate("1719.1.1") == 365 * 24);
    assert(ParseGameDate("-1.12.31") == 43'800'000 - 24);
    for (const char* bad : {"", "1720", "1720.1", "1720.13.1", "1720.2.29", "1720.0.1", "1720.1.0", "1720.1.1.24",
                            "1720.1.1.1.1", "1720..1", "1720.1.1x", "x.1.1", "1720.1.1.", "1234567.1.1"}) {
        assert(!ParseGameDate(bad));
    }
    for (std::int32_t raw : {58'860'240, 58'860'245, 43'800'000, 43'799'999}) {
        const GameDate d = DecodeGameDate(raw);
        assert(EncodeGameDate(d) == raw);
    }
}

static void TestWaitUntil() {
    FakeGame game = LoadedGame();
    game.clock_runs_date = true;
    const Handlers h = game.Make();
    // Paused: the date does not move, so the wait times out.
    std::string out = HandleRequest("wait_until 1719.3.18 timeout=1", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "timeout before the target date"));
    assert(Contains(out, "\"reached\":false") && Contains(out, "\"paused_ms\":1"));

    game.state.paused_flag = false;
    out = HandleRequest("wait_until 1719.3.18", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"reached\":true") && Contains(out, "\"command\":\"wait_until\""));
    assert(Contains(out, "\"target\":\"1719.3.18\"") && Contains(out, "\"start_date\":\"1719.3.17\""));
    assert(Contains(out, "\"reached_date\":\"1719.3.18\"") && Contains(out, "\"paused_ms\":0"));
    assert(Contains(out, "\"days_per_second\":"));
    assert(game.pressed.empty());  // wait_until never presses keys

    // Already reached: answers at once.
    out = HandleRequest("wait_until 1719.1.1", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"wall_ms\":0"));

    for (const char* bad : {"wait_until", "wait_until 1720", "wait_until 1720.1.1 speed=5", "wait_until 1720.1.1 timeout=0",
                            "wait_until 1720.1.1 timeout=x", "wait_until 1720.1.1 extra"}) {
        assert(Contains(HandleRequest(bad, h), "\"ok\":false"));
    }

    FakeGame menu;
    const Handlers hm = menu.Make();
    assert(Contains(HandleRequest("wait_until 1720.1.1", hm), "no game loaded"));
}

static void TestRunUntil() {
    FakeGame game = LoadedGame();  // paused, UI speed 2
    game.clock_runs_date = true;
    const Handlers h = game.Make();
    std::string out = HandleRequest("run_until 1719.4.1 speed=5", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"command\":\"run_until\"") && Contains(out, "\"reached\":true"));
    assert(Contains(out, "\"start_speed\":2") && Contains(out, "\"speed\":5"));
    assert(Contains(out, "\"paused\":true") && Contains(out, "\"paused_flag\":true"));
    assert(game.state.paused && game.state.date_raw >= *ParseGameDate("1719.4.1"));
    assert(!Contains(out, "\"overshoot_hours\":-"));

    // Already there.
    out = HandleRequest("run_until 1719.4.1", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "already at or past 1719.4.1"));

    // Cancelled from "another client" while waiting; the game is paused again.
    int sleeps = 0;
    game.on_sleep = [&] {
        if (++sleeps == 20) assert(Contains(HandleRequest("cancel", h), "\"ok\":true"));
    };
    out = HandleRequest("run_until 1730.1.1", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "\"error\":\"cancelled\"") && Contains(out, "\"reached\":false"));
    assert(!Contains(out, "overshoot_hours"));
    assert(game.state.paused_flag);
    game.on_sleep = nullptr;

    // Timeout.
    out = HandleRequest("run_until 1800.1.1 timeout=1", h);
    assert(Contains(out, "timeout before the target date") && game.state.paused_flag);

    // A game that does not react to Space: the resume fails and nothing waits.
    game.ignore_keys = true;
    out = HandleRequest("run_until 1800.1.1", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "resume: no change"));
    game.ignore_keys = false;

    assert(Contains(HandleRequest("run_until 1800.1.1 speed=6", h), "bad option"));
    Handlers no_input = h;
    no_input.press_key = nullptr;
    assert(Contains(HandleRequest("run_until 1800.1.1", no_input), "input = true"));
    assert(Contains(HandleRequest("wait_until 1700.1.1", no_input), "\"ok\":true"));
}

int main() {
    TestDate();
    TestParseDate();
    TestWaitUntil();
    TestRunUntil();
    TestConfig();
    TestProtocol();
    TestKeys();
    TestInputDisabled();
    TestPauseResume();
    TestPauseTimeoutAndErrors();
    TestSpeed();
    std::puts("ok");
    return 0;
}
