#include "config.hpp"
#include "console.hpp"
#include "game_date.hpp"
#include "notify_sources.hpp"
#include "protocol.hpp"
#include "ui_tree.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace tongs;

static bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

static std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Plays the game's main thread: an outer frame every few milliseconds.
class FakeMainThread {
public:
    explicit FakeMainThread(Dispatcher& d) : thread_([this, &d] {
        std::uint64_t frame = 0;
        while (!stop_) {
            d.RunBatch(++frame);
            SleepMs(2);
        }
    }) {}
    ~FakeMainThread() {
        stop_ = true;
        thread_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

static void TestDispatcher() {
    Dispatcher d(SteadyMs);

    // No frame: the job is withdrawn at its limit and never runs, even if a frame comes later.
    bool ran = false;
    Dispatcher::Outcome o = d.Run(Dispatcher::Kind::Read, [&] { ran = true; return std::string("x"); }, 30);
    assert(!o.ran && o.error == "main thread busy" && o.queued_ms >= 25);
    d.RunBatch(1);
    assert(!ran);

    // Frames run: the answer comes back with the frame that ran it.
    {
        FakeMainThread main(d);
        o = d.Run(Dispatcher::Kind::Read, [] { return std::string(",\"a\":1"); }, 5000);
        assert(o.ran && o.answer == ",\"a\":1" && o.frame > 0 && o.error.empty());
    }

    // A started job finishes past its limit.
    {
        FakeMainThread main(d);
        o = d.Run(Dispatcher::Kind::Read, [] { SleepMs(80); return std::string("late"); }, 20);
        assert(o.ran && o.answer == "late" && o.run_ms >= 70);
    }

    // A job that throws does not unwind into the frame.
    {
        FakeMainThread main(d);
        o = d.Run(Dispatcher::Kind::Read, []() -> std::string { throw std::runtime_error("x"); }, 5000);
        assert(!o.ran && o.error == "the job failed");
    }

    // cancel withdraws queued jobs.
    {
        std::atomic<bool> cancelled_ran{false};
        Dispatcher::Outcome out;
        std::thread client([&] {
            out = d.Run(Dispatcher::Kind::Read, [&] { cancelled_ran = true; return std::string(); }, 5000);
        });
        SleepMs(30);
        assert(d.CancelQueued() == 1);
        client.join();
        d.RunBatch(2);
        assert(!out.ran && out.error == "cancelled" && !cancelled_ran);
        assert(d.CancelQueued() == 0);
    }

    // FIFO; a game call ends the batch, so the read queued after it waits for the next frame.
    {
        std::vector<int> order;
        Dispatcher::Outcome call, read;
        std::thread a([&] {
            call = d.Run(Dispatcher::Kind::GameCall, [&] { order.push_back(1); return std::string(); }, 5000);
        });
        SleepMs(30);
        std::thread b([&] { read = d.Run(Dispatcher::Kind::Read, [&] { order.push_back(2); return std::string(); }, 5000); });
        SleepMs(30);
        d.RunBatch(10);
        assert(order.size() == 1 && order[0] == 1);
        d.RunBatch(11);
        a.join();
        b.join();
        assert(order.size() == 2 && order[1] == 2 && call.frame == 10 && read.frame == 11);
    }

    // Reads share one batch.
    {
        std::atomic<int> count{0};
        std::vector<std::thread> clients;
        for (int i = 0; i < 3; ++i) {
            clients.emplace_back([&] { d.Run(Dispatcher::Kind::Read, [&] { ++count; return std::string(); }, 5000); });
        }
        SleepMs(50);
        d.RunBatch(20);
        for (auto& c : clients) c.join();
        assert(count == 3);
    }
}

static void TestFrameStats() {
    FrameStats s;
    for (std::uint64_t t = 0; t < 1000; t += 10) s.OnOuter(t);  // 100 frames in the first second
    s.OnNested(500);
    s.OnNested(510);
    s.OnOuter(1000);  // closes the first second
    FrameStats::Snapshot snap = s.Take(1005);
    assert(snap.outer_per_s == 100 && snap.nested_per_s == 2);
    assert(snap.outer_since == 101 && snap.nested_since == 2 && snap.since_ms == 1005);
    assert(snap.longest_gap_ms == 10 && snap.outer_total == 101);

    s.OnOuter(3000);  // two seconds without a frame, e.g. a save loading
    snap = s.Take(3000);
    assert(snap.outer_per_s == 0 && snap.longest_gap_ms == 2000 && snap.outer_since == 1 && snap.since_ms == 1995);

    snap = s.Take(3500);  // the open gap counts too
    assert(snap.longest_gap_ms == 500 && snap.outer_since == 0);

    FrameStats empty;
    assert(empty.Take(100).longest_gap_ms == 0);
}

static void TestFramesCommand() {
    Handlers h;
    h.now_ms = SteadyMs;
    assert(Contains(HandleRequest("frames", h), "frame hook is not installed"));
    Dispatcher d(SteadyMs);
    FrameStats stats;
    h.dispatcher = &d;
    h.frame_stats = &stats;
    assert(Contains(HandleRequest("help", h), "\"frames\"") && Contains(HandleRequest("help", h), "\"main_thread\":true"));
    for (const char* bad : {"frames timeout=0", "frames timeout=601", "frames timeout=x", "frames 5"}) {
        assert(Contains(HandleRequest(bad, h), "optional timeout"));
    }

    std::string out = HandleRequest("frames timeout=1", h);  // no frame comes
    assert(Contains(out, "\"ok\":false") && Contains(out, "\"error\":\"main thread busy\"") &&
           Contains(out, "\"command\":\"frames\"") && Contains(out, "\"queued_ms\":"));

    stats.OnOuter(SteadyMs());
    h.current_thread_id = [] { return 4242u; };
    FakeMainThread main(d);
    out = HandleRequest("frames", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"command\":\"frames\"") && Contains(out, "\"outer_per_s\":"));
    assert(Contains(out, "\"longest_gap_ms\":") && Contains(out, "\"outer_total\":1") && Contains(out, "\"run_ms\":"));
    assert(Contains(out, "\"frame\":") && Contains(out, "\"queued_ms\":") && Contains(out, "\"thread_id\":4242"));
    assert(Contains(HandleRequest("cancel", h), "\"withdrawn\":0"));
}

// A small screen like the game's: the speed controls, a hidden timeline and an event.
static UiTree SampleTree() {
    UiTree t;
    const auto add = [&t](const char* name, UiType type, bool shown, UiRect rect, int parent = -1) {
        UiWidget w;
        w.name = name;
        w.type = type;
        w.shown = shown;
        w.rect = rect;
        if (type == UiType::Button) {
            w.has_enabled = true;
            w.enabled = true;
        }
        t.widgets.push_back(w);
        const int id = static_cast<int>(t.widgets.size()) - 1;
        if (parent == -1) {
            t.roots.push_back(id);
        } else {
            t.widgets[parent].children.push_back(id);
        }
        return id;
    };
    const int speed = add("speed_controls", UiType::Window, true, {3180, 0, 260, 90});
    add("icon_date_bg", UiType::Icon, true, {3186, 0, 240, 60}, speed);
    const int date = add("DateText", UiType::Text, true, {3213, 13, 120, 20}, speed);
    t.widgets[date].has_text = true;
    t.widgets[date].text = "17 March 1719";
    add("button_speedup", UiType::Button, true, {3338, 20, 24, 24}, speed);
    const int down = add("button_speeddown", UiType::Button, true, {3338, 50, 24, 24}, speed);
    t.widgets[down].enabled = false;
    const int timeline = add("timeline_controls", UiType::Window, false, {3180, 90, 260, 90});
    add("icon_date_bg", UiType::Icon, true, {3180, 90, 240, 60}, timeline);
    add("timeline_pause", UiType::Button, true, {3249, 143, 24, 24}, timeline);
    const int ev = add("EventWindow", UiType::Window, true, {1300, 300, 700, 800});
    const int list = add("event_option_listbox", UiType::Listbox, true, {1376, 900, 547, 180}, ev);
    const char* options[] = {"Accept the tariffs", "Refuse them", "Ask the merchants"};
    for (int i = 0; i < 3; ++i) {
        const int entry = add("event_option_entry", UiType::Window, true, {1376, 900 + i * 60, 547, 56}, list);
        const int button = add("event_option_button", UiType::Button, true, {1376, 900 + i * 60, 500, 56}, entry);
        t.widgets[button].has_text = true;
        t.widgets[button].text = options[i];
        t.widgets[button].enabled = i != 2;
    }
    add("", UiType::Other, true, {0, 0, 1, 1}, ev);  // no name: a positional step
    add("EventWindow", UiType::Window, false, {0, 0, 10, 10});  // a second, hidden one
    return t;
}

static std::vector<std::string> Paths(const UiTree& t, const char* selector, bool all = false) {
    UiSelector sel;
    const std::string error = ParseUiSelector(selector, sel);
    if (!error.empty()) return {"error: " + error};
    const UiView view(t, all);
    std::vector<std::string> out;
    for (const int w : view.Resolve(sel)) out.push_back(view.PathOf(w));
    return out;
}

static void TestUiSelectors() {
    UiSelector sel;
    for (const char* bad : {"", "  ", "/a", "a/", "a//", "a/[0]", "a[x=1]", "a[type=frame]", "a[type~=button]",
                            "a[text=]", "a[1][2]", "a[", "a]b", "a[1]x"}) {
        assert(!ParseUiSelector(bad, sel).empty());
    }
    assert(ParseUiSelector("a/[0]", sel) == "[n] counts from 1");
    assert(ParseUiSelector("speed_controls/[type=button]", sel).empty() && sel.steps.size() == 2 &&
           sel.steps[1].name == "*" && sel.steps[1].axis == UiSelector::Axis::Child);
    assert(ParseUiSelector("//x[text^=\"Accept [the]\"][2]", sel).empty() &&
           sel.steps[0].axis == UiSelector::Axis::Descendant && sel.steps[0].index == 2 &&
           sel.steps[0].filters[0].value == "accept [the]");

    const UiTree t = SampleTree();
    using V = std::vector<std::string>;
    // Anchored at the top-level windows; names ignore case and take * and ?.
    assert(Paths(t, "speed_controls/button_speedup") == V{"speed_controls/button_speedup"});
    assert(Paths(t, "SPEED_CONTROLS/button_speed*") ==
           V({"speed_controls/button_speedup", "speed_controls/button_speeddown"}));
    assert(Paths(t, "speed_controls/button_speed?p") == V{"speed_controls/button_speedup"});
    assert(Paths(t, "button_speedup").empty());  // not a top-level window
    assert(Paths(t, "//button_speedup") == V{"speed_controls/button_speedup"});
    // Filters.
    assert(Paths(t, "speed_controls/[type=button]") ==
           V({"speed_controls/button_speedup", "speed_controls/button_speeddown"}));
    assert(Paths(t, "speed_controls/*[disabled]") == V{"speed_controls/button_speeddown"});
    assert(Paths(t, "speed_controls/[type=button][enabled]") == V{"speed_controls/button_speedup"});
    assert(Paths(t, "speed_controls/[2]") == V{"speed_controls/DateText"});
    assert(Paths(t, "//[text~=MARCH]") == V{"speed_controls/DateText"});
    assert(Paths(t, "//*[text=\"17 march 1719\"]") == V{"speed_controls/DateText"});
    assert(Paths(t, "//[text^=17 m]") == V{"speed_controls/DateText"});
    assert(Paths(t, "//[text^=march]").empty());
    // Repeated names get [n]; the path finds the widget again.
    const V options = Paths(t, "EventWindow//event_option_button");
    assert(options == V({"EventWindow/event_option_listbox/event_option_entry[1]/event_option_button",
                         "EventWindow/event_option_listbox/event_option_entry[2]/event_option_button",
                         "EventWindow/event_option_listbox/event_option_entry[3]/event_option_button"}));
    for (const auto& path : options) assert(Paths(t, path.c_str()) == V{path});
    assert(Paths(t, "//event_option_button[text^=refuse]") == V{options[1]});
    assert(Paths(t, "//event_option_button[2]") == V{options[1]});
    assert(Paths(t, "EventWindow/event_option_listbox/[2]/[1]") == V{options[1]});
    assert(Paths(t, "EventWindow/[2]") == V{"EventWindow/[2]"});  // the widget without a name
    // Hidden widgets and everything in a hidden window only with `all`; then paths count them too.
    assert(Paths(t, "//timeline_pause").empty());
    assert(Paths(t, "//timeline_pause", true) == V{"timeline_controls/timeline_pause"});
    assert(Paths(t, "//icon_date_bg") == V{"speed_controls/icon_date_bg"});
    assert(Paths(t, "EventWindow") == V{"EventWindow"});
    assert(Paths(t, "EventWindow", true) == V({"EventWindow[1]", "EventWindow[2]"}));
    assert(Paths(t, "EventWindow[2]", true) == V{"EventWindow[2]"});

    const UiView view(t, false);
    assert(view.Visible(0) && !view.Visible(5) && !view.Visible(6));  // timeline_controls and its child
    assert(GameTextToUtf8("Krak\xF3w \x80 \x81") == "Krak\xC3\xB3w \xE2\x82\xAC \xC2\x81");
    // Colour codes are not displayed; the ducat sign is.
    assert(GameTextToUtf8("\xA7GM\xFCnster\xA7! \xA4") == "M\xC3\xBCnster \xC2\xA4" && GameTextToUtf8("x\xA7") == "x");
}

static void TestUiRequests() {
    UiRequest r;
    assert(ParseUiRequest("find //x[text=\"a b\"] all limit=3", r).empty() && r.verb == UiRequest::Verb::Find &&
           r.selector_text == "//x[text=\"a b\"]" && r.all && r.limit == 3);
    assert(ParseUiRequest("find //x[text^=a b]", r).empty() && r.selector_text == "//x[text^=a b]" &&
           r.limit == kUiFindLimit);
    assert(ParseUiRequest("tree", r).empty() && r.selector.steps.empty() && r.limit == kUiTreeLimit && r.depth == -1);
    assert(ParseUiRequest("tree depth=1 all", r).empty() && r.depth == 1 && r.all && r.selector_text.empty());
    assert(ParseUiRequest("tree speed_controls depth=0 timeout=2", r).empty() && r.depth == 0 && r.timeout_ms == 2000);
    assert(Contains(ParseUiRequest("find", r), "\"error\":\"bad_selector\""));
    assert(Contains(ParseUiRequest("find a//", r), "\"error\":\"bad_selector\""));
    assert(Contains(ParseUiRequest("find a depth=1", r), "\"error\":\"bad_command\""));
    assert(Contains(ParseUiRequest("tree limit=0", r), "\"error\":\"bad_command\""));
    assert(Contains(ParseUiRequest("tree a b", r), "\"error\":\"bad_command\""));
    assert(Contains(ParseUiRequest("press a", r), "\"error\":\"bad_command\""));

    const UiTree t = SampleTree();
    bool found = false;
    assert(ParseUiRequest("find speed_controls/[type=button] limit=1", r).empty());
    std::string out = UiAnswer(t, r, found);
    assert(found &&
           Contains(out, "\"count\":2,\"truncated\":true,\"matches\":[{\"path\":\"speed_controls/button_speedup\"") &&
           Contains(out, "\"type\":\"button\",\"visible\":true,\"rect\":{\"x\":3338,\"y\":20,\"w\":24,\"h\":24}") &&
           Contains(out, "\"click_point\":{\"x\":3350,\"y\":32},\"enabled\":true}]"));
    assert(ParseUiRequest("find //DateText", r).empty());
    out = UiAnswer(t, r, found);
    assert(Contains(out, "\"text\":\"17 March 1719\"") && !Contains(out, "\"enabled\""));
    assert(ParseUiRequest("find //nothing", r).empty());
    assert(Contains(UiAnswer(t, r, found), "\"count\":0,\"truncated\":false,\"matches\":[]") && found);

    // The whole tree: visible windows only, nested.
    assert(ParseUiRequest("tree", r).empty());
    out = UiAnswer(t, r, found);
    assert(Contains(out, "\"count\":14,\"truncated\":false") && !Contains(out, "timeline") &&
           Contains(out, "{\"name\":\"speed_controls\",\"path\":\"speed_controls\"") && Contains(out, "\"children\":[{"));
    assert(ParseUiRequest("tree all", r).empty());
    out = UiAnswer(t, r, found);
    assert(Contains(out, "\"count\":18,") && Contains(out, "\"visible\":false"));
    // depth=N replaces deeper levels with children_count.
    assert(ParseUiRequest("tree depth=0", r).empty());
    out = UiAnswer(t, r, found);
    assert(Contains(out, "\"count\":2,") && Contains(out, "\"children_count\":4") && Contains(out, "\"children_count\":2") &&
           !Contains(out, "\"children\":"));
    assert(ParseUiRequest("tree limit=3", r).empty());
    assert(Contains(UiAnswer(t, r, found), "\"count\":3,\"truncated\":true"));
    // From a selector; a match inside another match is shown once.
    assert(ParseUiRequest("tree //event_option_entry[2]", r).empty());
    out = UiAnswer(t, r, found);
    assert(Contains(out, "\"selector\":\"//event_option_entry[2]\",\"count\":2,") && Contains(out, "Refuse them"));
    assert(ParseUiRequest("tree EventWindow//*", r).empty());
    assert(Contains(UiAnswer(t, r, found), "\"count\":8,"));
    assert(ParseUiRequest("tree //nothing", r).empty());
    UiAnswer(t, r, found);
    assert(!found);
}

static void TestUiCommand() {
    Handlers h;
    h.now_ms = SteadyMs;
    assert(Contains(HandleRequest("ui tree", h), "frame hook is not installed"));
    std::string out = HandleRequest("ui find a//", h);  // a bad selector never reaches the main thread
    assert(Contains(out, "\"ok\":false,\"error\":\"bad_selector\"") && Contains(out, "\"command\":\"ui find\""));
    assert(Contains(HandleRequest("ui", h), "\"command\":\"ui\""));

    Dispatcher d(SteadyMs);
    h.dispatcher = &d;
    std::thread::id reader;
    h.read_ui = [&reader](UiTree& tree) {
        reader = std::this_thread::get_id();
        tree = SampleTree();
        return std::string();
    };
    out = HandleRequest("ui find //button_speedup timeout=1", h);  // no frame comes
    assert(Contains(out, "\"error\":\"main thread busy\"") && Contains(out, "\"command\":\"ui find\""));
    {
        FakeMainThread main(d);
        out = HandleRequest("ui find //button_speedup", h);
        assert(Contains(out, "{\"ok\":true,\"command\":\"ui find\",\"selector\":\"//button_speedup\",\"count\":1,") &&
               Contains(out, "\"queued_ms\":") && Contains(out, "\"run_ms\":") && Contains(out, "\"frame\":"));
        assert(reader != std::this_thread::get_id());  // read on the "main thread"
        out = HandleRequest("ui tree //nothing", h);
        assert(Contains(out, "{\"ok\":false,\"error\":\"not_found\",\"command\":\"ui tree\""));
        h.read_ui = [](UiTree&) { return std::string("access violation while reading the widget tree"); };
        out = HandleRequest("ui tree", h);
        assert(Contains(out, "{\"ok\":false,\"error\":\"access violation") && Contains(out, "\"frame\":"));
    }
    assert(Contains(HandleRequest("help", h), "\"ui\""));
}

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

    // Game calls: off by default, each switch on its own line.
    const Config defaults = ParseConfig(kDefaultConfig);
    assert(!defaults.game_calls.enabled && !defaults.game_calls.ironman && !defaults.game_calls.multiplayer);
    assert(defaults.game_calls.allow.empty());
    const Config calls = ParseConfig(
        "game_calls = true\ngame_calls_ironman = true\ngame_calls_multiplayer = false\nconsole_allow = [\"leak\", \"oos\"] # two\n");
    assert(calls.game_calls.enabled && calls.game_calls.ironman && !calls.game_calls.multiplayer && calls.notes.empty());
    assert(calls.game_calls.allow == std::vector<std::string>({"leak", "oos"}));
    assert(ParseConfig("console_allow = []\n").notes.empty());
    const Config bad_list = ParseConfig("console_allow = leak\nconsole_allow = [\"a b\"]\n");
    assert(bad_list.notes.size() == 2 && bad_list.game_calls.allow.empty());
    // Names that console_allow cannot lift, or that need no entry, are noted.
    const Config noted = ParseConfig("console_allow = [\"enable_all_commands\", \"speed\", \"leak\"]\n");
    assert(noted.notes.size() == 2 && noted.game_calls.allow.size() == 3);
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
    assert(FindKey("grave")->sym == '`' && FindKey("grave")->scancode == 53 && FindKey("grave")->text == '`');
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
    std::vector<InputEvent> events;
    WindowSnapshot window;
    std::pair<int, int> cursor{-1, -1};  // the virtual GUI cursor
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
        h.send_input = [this](const std::vector<InputEvent>& batch) -> std::string {
            if (!push_error.empty()) return push_error;
            for (const InputEvent& e : batch) {
                events.push_back(e);
                if (e.type != InputEvent::Type::KeyDown) continue;
                pressed.push_back(e.key.sym);
                pending.emplace_back(clock + react_after_ms, e.key);
            }
            return {};
        };
        h.read_window = [this] { return window; };
        h.set_cursor = [this](int x, int y) { cursor = {x, y}; };
        h.release_cursor = [this] { cursor = {-1, -1}; };
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
    h.send_input = nullptr;
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

    // Paused with max_pause: stops after it instead of the timeout.
    out = HandleRequest("wait_until 1719.3.18 timeout=100 max_pause=1", h);
    assert(Contains(out, "longer than max_pause") && Contains(out, "\"paused_date\":\"1719.3.17\""));
    assert(Contains(out, "\"wall_ms\":10"));  // 1000 ms and a poll, not 100 s

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
    // An event pauses the game in the middle of the stretch: max_pause stops the wait early.
    int ticks = 0;
    game.on_sleep = [&] {
        if (++ticks == 30) game.held_by_game = true;
    };
    out = HandleRequest("run_until 1800.1.1 timeout=600 max_pause=2", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "longer than max_pause") && Contains(out, "\"paused_date\":"));
    assert(!Contains(out, "\"paused_ms\":0,") && game.clock < 600'000);
    game.on_sleep = nullptr;
    game.held_by_game = false;
    assert(Contains(HandleRequest("run_until 1800.1.1 max_pause=0", h), "bad option"));
    assert(Contains(HandleRequest("wait_until 1800.1.1 max_pause=x", h), "bad option"));

    out = HandleRequest("run_until 1800.1.1 timeout=1", h);
    assert(Contains(out, "timeout before the target date") && game.state.paused_flag);

    // A game that does not react to Space: the resume fails and nothing waits.
    game.ignore_keys = true;
    out = HandleRequest("run_until 1800.1.1", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "resume: no change"));
    game.ignore_keys = false;

    assert(Contains(HandleRequest("run_until 1800.1.1 speed=6", h), "bad option"));
    Handlers no_input = h;
    no_input.send_input = nullptr;
    assert(Contains(HandleRequest("run_until 1800.1.1", no_input), "input = true"));
    assert(Contains(HandleRequest("wait_until 1700.1.1", no_input), "\"ok\":true"));
}

static void TestTextEvents() {
    std::vector<InputEvent> events;
    assert(TextEvents("Ab 1", events).empty() && events.size() == 4);
    assert(events[0].type == InputEvent::Type::Text && std::string(events[0].text) == "A");
    assert(std::string(events[2].text) == " ");
    events.clear();
    assert(TextEvents("\xC3\xA4\xC3\xBF", events).empty() && events.size() == 2);  // U+00E4, U+00FF
    assert(std::string(events[0].text) == "\xC3\xA4");
    events.clear();
    assert(!TextEvents("\xC5\x82", events).empty());      // U+0142 does not fit the game's 8-bit decoding
    assert(!TextEvents("\xE2\x82\xAC", events).empty());  // three-byte UTF-8
    assert(!TextEvents("a\tb", events).empty());          // control character
    assert(!TextEvents("\xC3", events).empty());          // cut off
    assert(!TextEvents("\xC2\x85", events).empty());      // C1 control

    const std::vector<InputEvent> space = KeyPressEvents(*FindKey("space"));
    assert(space.size() == 3 && space[0].type == InputEvent::Type::KeyDown && space[1].type == InputEvent::Type::Text &&
           space[2].type == InputEvent::Type::KeyUp && space[1].text[0] == ' ');
    assert(KeyPressEvents(*FindKey("escape")).size() == 2);
}

static void TestWindowAndMouse() {
    FakeGame game = LoadedGame();
    game.window.found = true;
    game.window.width = 1920;
    game.window.height = 1080;
    game.window.thread_id = 42;
    game.window.dpi = 96;
    const Handlers h = game.Make();

    std::string out = HandleRequest("window", h);
    assert(Contains(out, "\"found\":true") && Contains(out, "\"width\":1920") && Contains(out, "\"thread_id\":42"));

    out = HandleRequest("move 100 200", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"x\":100") && Contains(out, "\"presses\":0"));
    assert(game.events.size() == 1 && game.events[0].type == InputEvent::Type::MouseMove);

    assert(game.cursor == std::make_pair(100, 200) && Contains(out, "\"cursor\":\"virtual\""));
    out = HandleRequest("click 110 190 right", h);
    assert(game.cursor == std::make_pair(110, 190));
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"button\":\"right\"") && Contains(out, "\"clicks\":1"));
    assert(game.events.size() == 4);
    assert(game.events[1].type == InputEvent::Type::MouseMove && game.events[1].xrel == 10 && game.events[1].yrel == -10);
    assert(game.events[2].type == InputEvent::Type::MouseDown && game.events[2].button == 3 && game.events[2].x == 110);
    assert(game.events[3].type == InputEvent::Type::MouseUp && game.events[3].y == 190);

    game.events.clear();
    out = HandleRequest("click 5 5 double", h);
    assert(Contains(out, "\"clicks\":2") && Contains(out, "\"presses\":2") && game.events.size() == 5);
    assert(game.events[3].type == InputEvent::Type::MouseDown && game.events[3].clicks == 2 && game.events[3].button == 1);

    game.events.clear();
    out = HandleRequest("scroll -3 50 60", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"steps\":-3") && game.events.size() == 4);
    assert(game.events[1].type == InputEvent::Type::Wheel && game.events[1].wheel == -1);
    game.events.clear();
    assert(game.cursor == std::make_pair(50, 60));
    assert(Contains(HandleRequest("scroll 2", h), "\"ok\":true") && game.events.size() == 2 && game.events[0].wheel == 1);
    assert(Contains(HandleRequest("release", h), "\"ok\":true") && game.cursor == std::make_pair(-1, -1));
    Handlers events_only = h;
    events_only.set_cursor = nullptr;
    assert(Contains(HandleRequest("move 1 1", events_only), "\"cursor\":\"events only\""));
    assert(game.cursor == std::make_pair(-1, -1));

    for (const char* bad : {"move", "move 1", "move a b", "move 1920 5", "move 5 1080", "move -1 5", "click 1 2 left x",
                            "click 1 2 up", "scroll 0", "scroll 21", "scroll 1 2", "scroll 1 5000 5", "text"}) {
        assert(Contains(HandleRequest(bad, h), "\"ok\":false"));
    }
    assert(Contains(HandleRequest("move 1920 5", h), "outside the 1920x1080 client area"));

    game.events.clear();
    out = HandleRequest("text Hello World", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"characters\":11") && game.events.size() == 11);
    assert(Contains(HandleRequest("text \xC5\x82", h), "cannot be typed"));

    game.window = {};
    game.window.reason = "no visible game window (SDL_app) yet";
    assert(Contains(HandleRequest("click 1 1", h), "no visible game window"));
    assert(Contains(HandleRequest("window", h), "\"found\":false"));
}

static void TestThreads() {
    Handlers h;
    ThreadsSnapshot t;
    t.ok = true;
    t.threads_setting = -1;
    t.pools[0] = {0x242B8E0, 8, 8};
    t.pools[1] = {0x242B950, 8, 16};
    t.process_mask = 0xFFFF;
    t.system_mask = 0xFFFFFFFF;
    t.process_threads = 70;
    t.main_thread_id = 1234;
    t.main_mask = 0x10;
    t.main_ideal_cpu = 4;
    h.read_threads = [&] { return t; };
    std::string out = HandleRequest("threads", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"rva\":\"0x242B8E0\",\"workers\":8,\"capacity\":8"));
    assert(Contains(out, "\"workers_equal\":true") && Contains(out, "\"process_mask\":\"0xFFFF\""));
    assert(Contains(out, "\"main_mask\":\"0x10\"") && Contains(out, "\"main_ideal_cpu\":4") && Contains(out, "\"threads_setting\":-1"));
    t.pools[1].workers = 7;
    assert(Contains(HandleRequest("threads", h), "\"workers_equal\":false"));
    t.ok = false;
    t.reason = "a pool's worker vector is not readable yet";
    t.main_thread_id = 0;
    out = HandleRequest("threads", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "not readable yet") && !Contains(out, "main_mask"));
    assert(Contains(HandleRequest("threads", Handlers{}), "thread reader unavailable"));
}

static void TestScreenshot() {
    Handlers h;
    std::string asked;
    ScreenshotResult shot;
    shot.ok = true;
    shot.path = "C:\\game\\anvil\\data\\tongs\\screenshots\\menu.png";
    shot.width = 1920;
    shot.height = 1080;
    shot.bytes = 12345;
    shot.method = "print_window";
    h.take_screenshot = [&](std::string_view name) {
        asked = std::string(name);
        return shot;
    };
    std::string out = HandleRequest("screenshot menu", h);
    assert(asked == "menu" && Contains(out, "\"ok\":true") && Contains(out, "screenshots\\\\menu.png"));
    assert(Contains(out, "\"width\":1920") && Contains(out, "\"method\":\"print_window\"") && Contains(out, "\"blank\":false"));
    HandleRequest("screenshot", h);
    assert(asked.empty());
    for (const char* bad : {"screenshot ../x", "screenshot a b", "screenshot a.png",
                            "screenshot 0123456789012345678901234567890123456789012345678"}) {
        assert(Contains(HandleRequest(bad, h), "optional name"));
    }
    shot.ok = false;
    shot.error = "the game window is minimized";
    assert(Contains(HandleRequest("screenshot", h), "minimized"));
    assert(Contains(HandleRequest("screenshot", Handlers{}), "unavailable"));
}

static int AddWidget(UiTree& t, const char* name, UiType type, bool shown, UiRect rect, int parent = -1) {
    UiWidget w;
    w.name = name;
    w.type = type;
    w.shown = shown;
    w.rect = rect;
    if (type == UiType::Button) {
        w.has_enabled = true;
        w.enabled = true;
    }
    t.widgets.push_back(w);
    const int id = static_cast<int>(t.widgets.size()) - 1;
    t.widgets[id].address = 0x1000 + static_cast<std::uintptr_t>(id) * 0x10;
    if (parent == -1) {
        t.roots.push_back(id);
    } else {
        t.widgets[parent].children.push_back(id);
    }
    return id;
}

static std::uintptr_t Address(int widget) { return 0x1000 + static_cast<std::uintptr_t>(widget) * 0x10; }

// SampleTree with two event windows on screen: "Tariffs on Fish" (widget 8, options
// 11, 13 and the disabled 15) and "Plague" (widget 17, one option).
static UiTree EventTree() {
    UiTree t = SampleTree();
    for (int i = 0; i < static_cast<int>(t.widgets.size()); ++i) t.widgets[i].address = Address(i);
    const auto text = [&t](int parent, const char* name, const char* value) {
        const UiRect& r = t.widgets[parent].rect;  // a strip at the top of the window
        const int w = AddWidget(t, name, UiType::Text, true, {r.x, r.y + 10, r.w, 40}, parent);
        t.widgets[w].has_text = true;
        t.widgets[w].text = value;
    };
    text(8, "Title", "Tariffs on Fish");
    text(8, "Description", "The merchants of Novgorod...");
    t.widgets[17].shown = true;
    t.widgets[17].rect = {100, 100, 700, 800};
    text(17, "Title", "Plague");
    const int list = AddWidget(t, "event_option_listbox", UiType::Listbox, true, {150, 700, 547, 60}, 17);
    const int entry = AddWidget(t, "event_option_entry", UiType::Window, true, {150, 700, 547, 56}, list);
    const int button = AddWidget(t, "event_option_button", UiType::Button, true, {150, 700, 500, 56}, entry);
    t.widgets[button].has_text = true;
    t.widgets[button].text = "Pray";
    return t;
}

static int FindWidget(const UiTree& t, const char* text) {
    for (int i = 0; i < static_cast<int>(t.widgets.size()); ++i) {
        if (t.widgets[i].text == text) return i;
    }
    return -1;
}

// Plague opened first, Tariffs on Fish later (on top); one more event was already chosen.
static EventsRaw EventRaw(const UiTree& t) {
    EventsRaw raw;
    OpenEventRaw chosen;
    chosen.controller = 0xC0;
    chosen.window = 0x9990;
    raw.events.push_back(chosen);
    OpenEventRaw plague;
    plague.controller = 0xC1;
    plague.waiting = true;
    plague.window = Address(17);
    plague.id = "flavor_x.2";
    plague.numeric_id = 7;
    plague.option_keys = {"flavor_x.2.a"};
    plague.buttons = {{Address(FindWidget(t, "Pray")), "7_0"}};
    raw.events.push_back(plague);
    OpenEventRaw fish;
    fish.controller = 0xC2;
    fish.waiting = true;
    fish.window = Address(8);
    fish.id = "flavor_rus.1";
    fish.numeric_id = 5;
    fish.option_keys = {"flavor_rus.1.a", "flavor_rus.1.b", "flavor_rus.1.hidden", ""};
    fish.buttons = {{Address(11), "5_0"}, {Address(13), "5_1"}, {Address(15), "5_3"}};
    raw.events.push_back(fish);
    raw.listed_windows = {Address(17), Address(8)};
    raw.has_delay = true;
    raw.option_delay = 0.5f;
    return raw;
}

static void TestEvents() {
    assert(ParseOptionScope("5_3", 5) == 3 && ParseOptionScope("5_-1", 5) == -1 && ParseOptionScope("6_0", 5) == -1);
    assert(ParseOptionScope("5", 5) == -1 && ParseOptionScope("x_1", 5) == -1 && ParseOptionScope("5_1x", 5) == -1);

    const UiTree t = EventTree();
    const std::vector<EventView> events = BuildEvents(t, EventRaw(t));
    assert(events.size() == 2);  // the chosen one is left out; the newest comes first
    const EventView& fish = events[0];
    assert(fish.number == 1 && fish.id == "flavor_rus.1" && fish.title == "Tariffs on Fish" &&
           fish.description == "The merchants of Novgorod..." && fish.path == "EventWindow[1]");
    assert(fish.options.size() == 3 && fish.options[1].number == 2 && fish.options[1].index == 1 &&
           fish.options[1].key == "flavor_rus.1.b" && fish.options[1].text == "Refuse them" && fish.options[1].enabled);
    assert(fish.options[1].path == "EventWindow[1]/event_option_listbox/event_option_entry[2]/event_option_button");
    // A hidden option uses up an index; the last one has no name and is disabled.
    assert(fish.options[2].index == 3 && fish.options[2].key.empty() && !fish.options[2].enabled);
    assert(events[1].id == "flavor_x.2" && events[1].title == "Plague" && events[1].number == 2);
    assert(FirstEnabledOption(fish) == 0);
    const std::string json = EventJson(fish);
    assert(Contains(json, "{\"number\":1,\"id\":\"flavor_rus.1\",\"title\":\"Tariffs on Fish\",\"description\":") &&
           Contains(json, "{\"number\":3,\"text\":\"Ask the merchants\",\"enabled\":false,\"option_index\":3,\"option_key\":\"\""));

    EventRequest r;
    for (const char* bad : {"", "foo", "choose", "choose 0", "choose 1 2", "choose text", "choose text^=\"a", "choose 1 event=0",
                            "choose 1 event=x", "list 1", "choose 1 timeout=0", "choose 1 event=1 event=2", "choose text^="}) {
        assert(Contains(ParseEventRequest(bad, r), "\"error\":\"bad_command\""));
    }
    assert(ParseEventRequest("list timeout=2", r).empty() && r.verb == EventRequest::Verb::List && r.timeout_ms == 2000);
    assert(ParseEventRequest("choose text^=\"Refuse THEM\" event=title~=fish", r).empty() &&
           r.option == EventRequest::Match::Starts && r.option_text == "refuse them" &&
           r.event == EventRequest::Match::Contains && r.event_title == "fish");

    const auto choose = [&events](const char* request) {
        EventRequest q;
        const std::string error = ParseEventRequest(request, q);
        assert(error.empty());
        return ChooseEvent(events, q);
    };
    EventChoice c = choose("choose 1");
    assert(c.error == "ambiguous" && Contains(c.members, "\"events\":[{\"number\":1,\"id\":\"flavor_rus.1\""));
    c = choose("choose 1 event=1");
    assert(c.error.empty() && c.event == 0 && c.option == 0);
    c = choose("choose 1 event=2");
    assert(c.error.empty() && c.event == 1 && c.option == 0);
    assert(choose("choose 3 event=1").error == "disabled");
    c = choose("choose 4 event=1");
    assert(c.error == "not_found" && Contains(c.members, "\"options\":["));
    assert(choose("choose 1 event=3").error == "not_found");
    c = choose("choose text^=refuse event=title^=TARIFFS");
    assert(c.error.empty() && c.event == 0 && c.option == 1);
    assert(choose("choose text~=e event=1").error == "ambiguous");
    assert(choose("choose text=pray event=title=plague").option == 0);
    assert(choose("choose 1 event=title~=x").error == "not_found");
    assert(choose("choose text=nothing event=1").error == "not_found");
    assert(ChooseEvent({}, r).error == "not_found");
    const std::vector<EventView> one(events.begin() + 1, events.end());
    EventRequest q;
    assert(ParseEventRequest("choose 1", q).empty() && ChooseEvent(one, q).error.empty());
    assert(Contains(ChoiceMembers(fish, fish.options[1]), "\"option\":{\"number\":2,\"text\":\"Refuse them\",\"option_index\":1"));
    assert(Contains(ChoiceJson(fish, fish.options[0], "1719.3.17"),
                    "{\"date\":\"1719.3.17\",\"id\":\"flavor_rus.1\",\"title\":\"Tariffs on Fish\",\"option\":1,"));
}

static void TestUiClickTarget() {
    UiTree t = EventTree();
    const auto target = [&t](const char* selector) {
        UiSelector sel;
        assert(ParseUiSelector(selector, sel).empty());
        return ResolveClickTarget(t, sel);
    };
    UiClickTarget c = target("speed_controls/button_speedup");
    assert(c.error.empty() && c.widget == 3 && c.path == "speed_controls/button_speedup");
    assert(target("speed_controls/button_speeddown").error == "disabled");
    c = target("//nothing");
    assert(c.error == "not_found" && c.count == 0);
    c = target("//timeline_pause");
    assert(c.error == "not_visible" && c.paths == std::vector<std::string>{"timeline_controls/timeline_pause"});
    c = target("//event_option_button");
    assert(c.error == "ambiguous" && c.count == 4 && c.paths.size() == 4);
    for (int i = 0; i < 12; ++i) AddWidget(t, "many", UiType::Button, true, {0, 0, 1, 1});
    c = target("many");
    assert(c.error == "ambiguous" && c.count == 12 && c.paths.size() == 10 && c.paths[9] == "many[10]");

    // The hovered widget is the target, inside it, something else, or nothing.
    assert(!CheckCover(t, Address(3), Address(3)).covered);
    UiCoverCheck cover = CheckCover(t, Address(10), Address(11));  // the button inside the option entry
    assert(!cover.covered && cover.hovered == "EventWindow[1]/event_option_listbox/event_option_entry[1]/event_option_button");
    cover = CheckCover(t, Address(11), Address(10));
    assert(cover.covered && cover.covered_by == "EventWindow[1]/event_option_listbox/event_option_entry[1]");
    cover = CheckCover(t, Address(3), 0);
    assert(cover.covered && cover.covered_by.empty());
    cover = CheckCover(t, Address(3), 0x77777);  // not in the tree
    assert(cover.covered && cover.covered_by.empty());
}

// A game with a widget tree and events: the GUI hovers the last visible widget
// under the virtual cursor, and a click on an option button chooses that option.
struct FakeScreen {
    FakeGame game = LoadedGame();
    UiTree tree = EventTree();
    EventsRaw raw = EventRaw(tree);
    std::vector<std::string> chosen;  // scopes of the clicked option buttons
    Dispatcher dispatcher{SteadyMs};

    std::uintptr_t Hovered() const {
        if (game.cursor.first < 0) return 0;
        const UiView view(tree, false);
        std::uintptr_t hit = 0;
        for (int i = 0; i < static_cast<int>(tree.widgets.size()); ++i) {
            const UiRect& r = tree.widgets[i].rect;
            if (view.Visible(i) && game.cursor.first >= r.x && game.cursor.first < r.x + r.w &&
                game.cursor.second >= r.y && game.cursor.second < r.y + r.h) {
                hit = tree.widgets[i].address;
            }
        }
        return hit;
    }

    Handlers Make() {
        game.window.found = true;
        game.window.width = 3440;
        game.window.height = 1440;
        Handlers h = game.Make();
        const auto send = h.send_input;
        h.send_input = [this, send](const std::vector<InputEvent>& batch) {
            std::string error = send(batch);
            for (const InputEvent& e : batch) {
                if (e.type != InputEvent::Type::MouseUp) continue;
                const std::uintptr_t under = Hovered();
                for (OpenEventRaw& ev : raw.events) {
                    for (const EventButtonRaw& b : ev.buttons) {
                        const int w = static_cast<int>((b.widget - 0x1000) / 0x10);
                        if (b.widget == under && ev.waiting && tree.widgets[w].enabled) {
                            ev.waiting = false;
                            chosen.push_back(b.scope);
                        }
                    }
                }
            }
            return error;
        };
        h.dispatcher = &dispatcher;
        h.read_ui = [this](UiTree& out) {
            out = tree;
            return std::string();
        };
        h.read_hovered = [this] { return Hovered(); };
        // A chosen event is gone by the next read, with its window.
        h.read_events = [this](EventsRaw& out) {
            out = raw;
            for (std::size_t i = raw.events.size(); i-- > 0;) {
                if (raw.events[i].waiting || raw.events[i].controller == 0xC0) continue;
                const std::uintptr_t window = raw.events[i].window;
                std::erase(raw.listed_windows, window);
                raw.events.erase(raw.events.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return std::string();
        };
        h.count_event_windows = [this] { return static_cast<int>(raw.listed_windows.size()); };
        return h;
    }
};

static void TestUiClickCommand() {
    FakeScreen screen;
    // A tooltip over the speed-up button, drawn last.
    AddWidget(screen.tree, "tooltip", UiType::Window, true, {3300, 0, 140, 40});
    const Handlers h = screen.Make();
    FakeMainThread main(screen.dispatcher);

    std::string out = HandleRequest("ui click speed_controls/button_speedup", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"covered\",\"command\":\"ui click\"") &&
           Contains(out, "\"covered_by\":\"tooltip\"") && Contains(out, "\"point\":{\"x\":3350,\"y\":32}"));
    assert(screen.game.events.size() == 1 && screen.game.events[0].type == InputEvent::Type::MouseMove);  // no press
    screen.tree.widgets.back().shown = false;
    screen.game.events.clear();
    out = HandleRequest("ui click speed_controls/button_speedup timeout=5", h);
    assert(Contains(out, "{\"ok\":true,\"command\":\"ui click\",\"selector\":\"speed_controls/button_speedup\"") &&
           Contains(out, "\"clicked\":\"speed_controls/button_speedup\"") && !Contains(out, "target_hovered") &&
           Contains(out, "\"hovered\":\"speed_controls/button_speedup\"") && Contains(out, "\"in_game\":true"));
    assert(screen.game.events.size() == 3 && screen.game.events[1].type == InputEvent::Type::MouseDown &&
           screen.game.events[1].x == 3350 && screen.game.events[2].type == InputEvent::Type::MouseUp);
    screen.game.events.clear();
    out = HandleRequest("ui click speed_controls/button_speeddown", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"disabled\"") && Contains(out, "\"path\":\"speed_controls/button_speeddown\""));
    out = HandleRequest("ui click //event_option_button", h);
    assert(Contains(out, "\"error\":\"ambiguous\"") && Contains(out, "\"count\":4,\"matches\":[\"EventWindow[1]/"));
    assert(Contains(HandleRequest("ui click //timeline_pause", h), "\"error\":\"not_visible\""));
    assert(Contains(HandleRequest("ui click //nothing", h), "\"error\":\"not_found\",\"command\":\"ui click\""));
    assert(screen.game.events.empty());  // refusals send nothing
    assert(Contains(HandleRequest("ui click", h), "\"error\":\"bad_selector\""));
    assert(Contains(HandleRequest("ui click a all", h), "\"error\":\"bad_command\""));

    Handlers no_input = h;
    no_input.send_input = nullptr;
    assert(Contains(HandleRequest("ui click speed_controls/button_speedup", no_input), "input = true"));
    Handlers no_hook = h;
    no_hook.set_cursor = nullptr;
    assert(Contains(HandleRequest("ui click speed_controls/button_speedup", no_hook), "cursor hook"));
}

static void TestEventCommands() {
    FakeScreen screen;
    const Handlers h = screen.Make();
    assert(Contains(HandleRequest("event list", Handlers{}), "frame hook is not installed"));
    assert(Contains(HandleRequest("event choose 0", h), "{\"ok\":false,\"error\":\"bad_command\""));
    FakeMainThread main(screen.dispatcher);

    std::string out = HandleRequest("event list", h);
    assert(Contains(out, "{\"ok\":true,\"command\":\"event list\",\"count\":2,\"events\":[{\"number\":1,\"id\":\"flavor_rus.1\"") &&
           Contains(out, "\"controllers\":3,\"listed_windows\":2,\"option_delay\":0.500") && Contains(out, "\"frame\":"));

    out = HandleRequest("event choose 1", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"ambiguous\",\"command\":\"event choose\""));
    assert(Contains(HandleRequest("event choose 3 event=1", h), "\"error\":\"disabled\""));
    assert(screen.game.events.empty() && screen.chosen.empty());

    out = HandleRequest("event choose text^=refuse event=title^=tariffs", h);
    std::fprintf(stderr, "%s\n", out.c_str());
    assert(Contains(out, "{\"ok\":true,\"command\":\"event choose\",\"choice_date\":\"1719.3.17\",\"event\":{\"number\":1,\"id\":\"flavor_rus.1\"") &&
           Contains(out, "\"option\":{\"number\":2,\"text\":\"Refuse them\",\"option_index\":1,\"option_key\":\"flavor_rus.1.b\"") &&
           Contains(out, "\"closed\":true,\"chosen_ms\":") && Contains(out, "\"closed_frames\":") && Contains(out, "\"in_game\":true"));
    assert(screen.chosen == std::vector<std::string>{"5_1"});

    // One event left: no event= needed.
    out = HandleRequest("event choose 1", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"id\":\"flavor_x.2\"") && screen.chosen.back() == "7_0");
    out = HandleRequest("event list", h);
    assert(Contains(out, "\"count\":0,\"events\":[],\"controllers\":1,\"listed_windows\":0"));
    assert(Contains(HandleRequest("event choose 1", h), "\"error\":\"not_found\""));

    // A click the game ignores: the event stays open.
    screen.raw = EventRaw(screen.tree);
    screen.tree.widgets[11].enabled = false;  // the game sees it disabled, the read before the click did not
    Handlers stale = h;
    stale.read_ui = [&screen](UiTree& out) {
        out = screen.tree;
        out.widgets[11].enabled = true;
        return std::string();
    };
    out = HandleRequest("event choose 1 event=1", stale);
    assert(Contains(out, "\"ok\":false,\"error\":\"the event was still waiting for a choice 3000 ms after the click\"") &&
           Contains(out, "\"closed\":false"));
}

static void TestInputsCount() {
    FakeGame game = LoadedGame();
    game.clock_runs_date = true;
    game.state.paused_flag = false;
    const Handlers h = game.Make();
    // Another client presses a key while wait_until waits.
    int ticks = 0;
    game.on_sleep = [&] {
        if (++ticks == 5) assert(Contains(HandleRequest("key a", h), "\"ok\":true"));
    };
    std::string out = HandleRequest("wait_until 1719.4.1", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"inputs\":1,"));
    game.on_sleep = nullptr;
    out = HandleRequest("wait_until 1719.5.1", h);
    assert(Contains(out, "\"inputs\":0,"));
    // run_until does not count its own presses.
    assert(Contains(HandleRequest("pause", h), "\"ok\":true"));
    out = HandleRequest("run_until 1719.6.1 speed=3", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"inputs\":0,"));
    assert(Contains(HandleRequest("run_until 1719.7.1 events=never", h), "bad option"));
    assert(Contains(HandleRequest("wait_until 1719.7.1 events=first", h), "bad option"));
}

static void TestRunUntilEventsFirst() {
    FakeScreen screen;
    screen.game.clock_runs_date = true;
    const EventsRaw opened = screen.raw;
    screen.raw = {};
    const Handlers h = screen.Make();
    FakeMainThread main(screen.dispatcher);
    assert(Contains(HandleRequest("run_until 1719.4.1 events=first", Handlers{}), "input = true"));

    // Two events open (and pause the game) on 1719.3.25; both are chosen, topmost first.
    const std::int32_t opens = *ParseGameDate("1719.3.25");
    bool open = false;
    screen.game.on_sleep = [&] {
        if (!open && screen.game.state.date_raw >= opens) {
            open = true;
            screen.raw = opened;
            screen.game.state.paused_flag = true;
        }
    };
    std::string out = HandleRequest("run_until 1719.4.1 speed=1 events=first", h);
    assert(Contains(out, "{\"ok\":true,\"command\":\"run_until\"") && Contains(out, "\"reached\":true"));
    assert(Contains(out, "\"events\":[{\"date\":\"1719.3.25\",\"id\":\"flavor_rus.1\",\"title\":\"Tariffs on Fish\",\"option\":1,") &&
           Contains(out, "\"id\":\"flavor_x.2\",\"title\":\"Plague\",\"option\":1,\"text\":\"Pray\""));
    // Two choices and one resume after the pause the first event set.
    assert(Contains(out, "\"inputs\":3,") && screen.chosen == std::vector<std::string>({"5_0", "7_0"}));
    assert(screen.game.state.paused_flag);

    // No option enabled: the run ends with the event.
    screen.game.on_sleep = nullptr;
    screen.raw = opened;
    screen.raw.events.erase(screen.raw.events.begin(), screen.raw.events.begin() + 2);  // only Tariffs on Fish
    screen.raw.listed_windows = {Address(8)};
    for (const int b : {11, 13}) screen.tree.widgets[b].enabled = false;
    screen.game.state.date_raw = *ParseGameDate("1719.4.2");
    out = HandleRequest("run_until 1719.5.1 events=first", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"no_enabled_option\"") && Contains(out, "\"events\":[],\"event\":{\"number\":1,") &&
           screen.game.state.paused_flag);
    // events=stop (the default) leaves events alone.
    screen.game.state.date_raw = *ParseGameDate("1719.4.2");
    out = HandleRequest("run_until 1719.5.1 events=stop", h);
    assert(Contains(out, "\"ok\":true") && !Contains(out, "\"events\":"));
}

static void TestConsoleRules() {
    std::vector<std::string> tokens;
    assert(SplitConsoleLine("  speed\t 3  ", tokens).empty() && tokens == std::vector<std::string>({"speed", "3"}));
    assert(!SplitConsoleLine("   ", tokens).empty() && tokens.empty());
    assert(!SplitConsoleLine("echo M\xC3\xBCnster", tokens).empty());
    assert(!SplitConsoleLine("echo a\nb", tokens).empty());
    assert(!SplitConsoleLine(std::string(1025, 'a'), tokens).empty());
    std::string many;
    for (int i = 0; i < 33; ++i) many += "a ";
    assert(!SplitConsoleLine(many, tokens).empty());

    // As the game's lookup: name or alias, case ignored, the first entry wins.
    const std::vector<ConsoleEntry> entries = {
        {"echo", {}}, {"debug_crash", {"crash"}}, {"resetandreloadmissions", {"crash"}}, {"window", {"wnd"}}};
    assert(FindConsoleEntry(entries, "ECHO") == &entries[0]);
    assert(FindConsoleEntry(entries, "crash") == &entries[1]);
    assert(FindConsoleEntry(entries, "Wnd") == &entries[3]);
    assert(FindConsoleEntry(entries, "nothing") == nullptr);

    assert(ClassifyConsoleCommand("enable_all_commands") == ConsoleClass::AlwaysRefused);
    assert(ClassifyConsoleCommand("Enable_All_Commands") == ConsoleClass::AlwaysRefused);
    assert(ClassifyConsoleCommand("social_addfriend") == ConsoleClass::AlwaysRefused);
    assert(ClassifyConsoleCommand("crashreporter.uploadcrash") == ConsoleClass::AlwaysRefused);
    assert(ClassifyConsoleCommand("debug_achievements_clear") == ConsoleClass::AlwaysRefused);
    assert(ClassifyConsoleCommand("oos") == ConsoleClass::Danger);
    assert(ClassifyConsoleCommand("debug_crash") == ConsoleClass::Danger);
    assert(ClassifyConsoleCommand("run_commands") == ConsoleClass::Danger);
    assert(ClassifyConsoleCommand("crash") == ConsoleClass::Normal);  // an alias: the game's own name decides
    assert(ClassifyConsoleCommand("speed") == ConsoleClass::Normal);

    GameCallPolicy policy;
    policy.enabled = true;
    assert(!CheckConsoleName(policy, "speed").refused());
    assert(CheckConsoleName(policy, "leak").error == "refused");
    policy.allow = {"LEAK", "enable_all_commands"};
    assert(!CheckConsoleName(policy, "leak").refused());
    assert(CheckConsoleName(policy, "enable_all_commands").refused());  // console_allow cannot lift it
    assert(CheckConsoleName(policy, "oos").refused());

    ConsoleRaw raw;
    assert(!CheckConsoleMode(policy, raw).refused());
    raw.ironman = true;
    assert(CheckConsoleMode(policy, raw).error == "ironman");
    policy.ironman = true;
    assert(!CheckConsoleMode(policy, raw).refused());
    raw.multiplayer = true;
    assert(CheckConsoleMode(policy, raw).error == "multiplayer");
    policy.multiplayer = true;
    assert(!CheckConsoleMode(policy, raw).refused());

    assert(CheckConsoleAllow({"leak", "social_joinroom", "speed"}).size() == 2);
}

// A loaded game with a console: entries as the manager lists them, and a
// dispatcher that answers a few commands.
struct FakeConsole {
    FakeGame game = LoadedGame();
    Dispatcher dispatcher{SteadyMs};
    ConsoleRaw raw;
    std::string read_error;
    std::vector<std::vector<std::string>> calls;
    bool fault = false;
    std::vector<std::pair<int, std::string>> log;

    FakeConsole() {
        game.state.session = 0x5000;
        raw.manager = true;
        raw.developer_lock = 1;
        raw.session = 0x5000;
        raw.entries = {{"echo", {}}, {"speed", {}}, {"enable_all_commands", {}}, {"oos", {}},
                       {"debug_crash", {"crash"}}, {"help", {}}};
    }

    Handlers Make() {
        Handlers h = game.Make();
        h.dispatcher = &dispatcher;
        h.game_calls.enabled = true;
        h.read_console = [this](ConsoleRaw& out) {
            out = raw;
            return read_error;
        };
        h.call_console = [this](const std::vector<std::string>& tokens, ConsoleCallResult& out) {
            calls.push_back(tokens);
            out = {};
            if (fault) {
                out.faulted = true;
                out.fault_code = 0xC0000005;
                return;
            }
            if (tokens[0] == "echo" && tokens.size() == 2) {
                out.success = true;
                out.text = tokens[1];
            } else if (tokens[0] == "SPEED" && tokens.size() == 2) {
                game.state.speed = std::stoi(tokens[1]) - 1;  // never sets the success byte
            } else if (tokens[0] == "help") {
                out.text = "\xA7YM\xFCnster\xA7!\nsecond line";  // game text: Windows-1252 with a colour code
            }
        };
        h.log = [this](int level, const std::string& message) { log.emplace_back(level, message); };
        return h;
    }
};

static void TestConsoleCommand() {
    FakeConsole console;
    Handlers h = console.Make();
    FakeMainThread main(console.dispatcher);

    // Off unless game_calls = true; the refusal is logged with the pipe instance.
    Handlers off = h;
    off.game_calls.enabled = false;
    std::string out = HandleRequest("console echo x", off, 2);
    assert(Contains(out, "{\"ok\":false,\"error\":\"game_calls_disabled\"") && console.calls.empty());
    assert(console.log.size() == 1 && console.log[0].first == 2 && Contains(console.log[0].second, "(pipe 2, 1719.3.17)"));
    assert(Contains(HandleRequest("help", h), "\"console\"") && Contains(HandleRequest("help", h), "\"game_calls\":true"));

    // A harmless round trip: the text, the success byte as a hint, the state afterwards.
    out = HandleRequest("console echo x", h, 3);
    assert(Contains(out, "{\"ok\":true,\"command\":\"console\",\"line\":\"echo x\",\"name\":\"echo\""));
    assert(Contains(out, "\"text\":\"x\",\"success_hint\":true,\"game_calls_total\":1") && Contains(out, "\"frame\":"));
    assert(Contains(out, "\"console\":{\"commands\":6,\"developer_lock\":1,\"mode_lock\":0}"));
    assert(!Contains(out, "\"mode\""));
    assert(console.calls.back() == std::vector<std::string>({"echo", "x"}));
    assert(console.log.back().first == 1 && Contains(console.log.back().second, "console \"echo x\" (pipe 3, 1719.3.17): called"));

    // speed: no success byte, but the state read afterwards shows the change. Names ignore case.
    out = HandleRequest("console SPEED 4", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"name\":\"speed\"") && Contains(out, "\"success_hint\":false"));
    assert(Contains(out, "\"speed\":4") && Contains(out, "\"game_calls_total\":2"));
    // Game text comes back as UTF-8 without colour codes.
    assert(Contains(HandleRequest("console help", h), "\"text\":\"M\xC3\xBCnster\\nsecond line\""));
    assert(console.log.back().second.find('\n') == std::string::npos);  // one log line, also for multi-line text
    assert(Contains(HandleRequest("state", h), "\"game_calls_total\":3"));

    // Refusals, none of which calls the game.
    const std::size_t made = console.calls.size();
    out = HandleRequest("console enable_all_commands 1234", h);
    assert(Contains(out, "\"error\":\"refused\"") && Contains(out, "always refused"));
    assert(Contains(HandleRequest("console Social_SendMessage hi", h), "\"error\":\"refused\""));
    out = HandleRequest("console oos", h);
    assert(Contains(out, "\"error\":\"refused\"") && Contains(out, "console_allow"));
    // An alias is refused by the command it names, which only the main thread knows.
    out = HandleRequest("console crash", h);
    assert(Contains(out, "\"error\":\"refused\"") && Contains(out, "\"name\":\"debug_crash\"") && Contains(out, "\"run_ms\":"));
    assert(Contains(HandleRequest("console nosuch", h), "\"error\":\"unknown_command\""));
    assert(Contains(HandleRequest("console", h), "\"error\":\"bad_command\""));
    assert(Contains(HandleRequest("console echo \xC3\xA9", h), "\"error\":\"bad_command\""));
    assert(console.calls.size() == made);
    assert(Contains(console.log.back().second, "bad_command") && console.log.back().first == 2);

    // console_allow lets a danger command through.
    Handlers allow = h;
    allow.game_calls.allow = {"oos"};
    assert(Contains(HandleRequest("console oos", allow), "\"ok\":true") && console.calls.size() == made + 1);

    // Ironman and multiplayer refuse every game call, unless the switch lifts it; then `mode` shows it.
    console.raw.ironman = true;
    assert(Contains(HandleRequest("console echo x", h), "\"error\":\"ironman\""));
    Handlers ironman = h;
    ironman.game_calls.ironman = true;
    out = HandleRequest("console echo x", ironman);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"mode\":{\"ironman\":true,\"multiplayer\":false,"));
    assert(console.log.back().first == 2 && Contains(console.log.back().second, "called in ironman"));
    assert(!Contains(HandleRequest("console leak", ironman), "\"mode\""));  // refused before the mode is read
    console.raw.ironman = false;
    console.raw.multiplayer = true;
    assert(Contains(HandleRequest("console echo x", ironman), "\"error\":\"multiplayer\""));
    console.raw.multiplayer = false;

    // Only in game, and only with a console manager that has its commands.
    console.game.state.in_game = false;
    console.game.state.reason = "no game loaded (session is null)";
    assert(Contains(HandleRequest("console echo x", h), "\"error\":\"not_in_game\""));
    console.game.state.in_game = true;
    console.raw.entries.clear();
    assert(Contains(HandleRequest("console echo x", h), "\"error\":\"console_unavailable\""));
    console.raw = FakeConsole().raw;
    console.read_error = "cannot read the console manager";
    assert(Contains(HandleRequest("console echo x", h), "\"error\":\"console_unavailable\""));
    console.read_error.clear();

    // A game call by another client during the wait marks the run.
    console.game.clock_runs_date = true;
    console.game.state.paused_flag = false;
    int ticks = 0;
    console.game.on_sleep = [&] {
        if (++ticks == 5) assert(Contains(HandleRequest("console echo y", h), "\"ok\":true"));
    };
    out = HandleRequest("wait_until 1719.4.1", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"inputs\":0,\"game_calls\":1,"));
    console.game.on_sleep = nullptr;
    assert(Contains(HandleRequest("wait_until 1719.5.1", h), "\"game_calls\":0,"));
    console.game.clock_runs_date = false;
    console.game.state.paused_flag = true;

    // game_calls_total counts since the save was loaded: another session object, or an earlier date.
    const std::string total = HandleRequest("state", h);
    assert(Contains(total, "\"game_calls_total\":") && !Contains(total, "\"game_calls_total\":0"));
    console.game.state.session = 0x6000;
    assert(Contains(HandleRequest("state", h), "\"game_calls_total\":0"));
    console.raw.session = 0x6000;
    assert(Contains(HandleRequest("console echo z", h), "\"game_calls_total\":1"));
    console.game.state.date_raw = *ParseGameDate("1719.1.1");
    assert(Contains(HandleRequest("state", h), "\"game_calls_total\":0"));

    // A fault trips game calls until the game exits; reads keep working.
    console.fault = true;
    out = HandleRequest("console echo boom", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"faulted\"") && Contains(out, "0xC0000005"));
    assert(console.log.back().first == 3);
    console.fault = false;
    const std::size_t before_trip = console.calls.size();
    out = HandleRequest("console echo x", h);
    assert(Contains(out, "\"error\":\"game_calls_tripped\"") && console.calls.size() == before_trip);
    assert(Contains(HandleRequest("state", h), "\"ok\":true") && Contains(HandleRequest("help", h), "\"game_calls_tripped\":true"));
}

static std::int32_t Raw(const char* date) { return *ParseGameDate(date); }

static std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t start = 0; start < text.size();) {
        const std::size_t eol = text.find('\n', start);
        assert(eol != std::string::npos);  // every notification ends its line
        out.push_back(text.substr(start, eol - start));
        start = eol + 1;
    }
    return out;
}

static void TestSubscribeParsing() {
    SubscribeRequest r;
    assert(ParseSubscribe("state,date date=day", r).empty() && r.kinds == (Bit(NotificationKind::State) | Bit(NotificationKind::Date)) &&
           r.date_unit == DateUnit::Day);
    assert(ParseSubscribe("all", r).empty() && r.kinds == kAllNotificationKinds && r.date_unit == DateUnit::Month);
    assert(ParseSubscribe("event  error_log,activity", r).empty() &&
           r.kinds == (Bit(NotificationKind::Event) | Bit(NotificationKind::ErrorLog) | Bit(NotificationKind::Activity)));
    assert(ParseSubscribe("date date=year", r).empty() && r.date_unit == DateUnit::Year);
    assert(Contains(ParseSubscribe("", r), "usage: subscribe"));
    assert(Contains(ParseSubscribe("states", r), "unknown kind: states"));
    assert(Contains(ParseSubscribe("state,,date", r), "empty kind"));
    assert(Contains(ParseSubscribe("date date=week", r), "day, month or year"));
    assert(Contains(ParseSubscribe("state date=day", r), "needs the date kind"));
    assert(Contains(ParseSubscribe("date date=day date=month", r), "twice"));
}

static void TestDateCrosses() {
    assert(DateCrosses(Raw("1719.3.17"), Raw("1719.3.18"), DateUnit::Day));
    assert(!DateCrosses(Raw("1719.3.17"), Raw("1719.3.17.12"), DateUnit::Day));
    assert(!DateCrosses(Raw("1719.3.17"), Raw("1719.3.31"), DateUnit::Month));
    assert(DateCrosses(Raw("1719.3.31"), Raw("1719.4.1"), DateUnit::Month));
    assert(DateCrosses(Raw("1719.3.31"), Raw("1719.4.1"), DateUnit::Day));
    assert(!DateCrosses(Raw("1719.3.31"), Raw("1719.12.31"), DateUnit::Year));
    assert(DateCrosses(Raw("1719.12.31"), Raw("1720.1.1"), DateUnit::Year));
    // Only forwards: a date that goes back (another save) is not a new day.
    assert(!DateCrosses(Raw("1720.1.1"), Raw("1719.1.1"), DateUnit::Year));
    assert(!DateCrosses(Raw("1720.1.1"), Raw("1720.1.1"), DateUnit::Day));
}

static void TestSubscriptionQueue() {
    SubscribeRequest request;
    request.kinds = kAllNotificationKinds;
    Subscription s(request, 1000, 3);
    assert(s.Take(0).empty());
    s.Push(NotificationKind::State, Raw("1719.3.17"), 1010, ",\"change\":\"paused\"");
    s.Push(NotificationKind::Activity, std::nullopt, 1020, ",\"what\":\"input\"");
    std::vector<std::string> lines = Lines(s.Take(0));
    assert(lines.size() == 2);
    assert(lines[0] == "{\"notification\":\"state\",\"seq\":1,\"date\":\"1719.3.17\",\"wall_ms\":10,\"change\":\"paused\"}");
    assert(lines[1] == "{\"notification\":\"activity\",\"seq\":2,\"date\":null,\"wall_ms\":20,\"what\":\"input\"}");

    // Five into a queue of three: the two oldest are dropped, and an overflow
    // notice with the time and date of the last dropped one comes first.
    for (int i = 0; i < 5; ++i) {
        s.Push(NotificationKind::ErrorLog, Raw("1719.4.1"), 2000 + i, ",\"line\":\"" + std::to_string(i) + "\"");
    }
    lines = Lines(s.Take(0, 2));
    assert(lines.size() == 2);
    assert(lines[0] == "{\"notification\":\"overflow\",\"seq\":3,\"date\":\"1719.4.1\",\"wall_ms\":1001,\"dropped\":2}");
    assert(Contains(lines[1], "\"seq\":4,") && Contains(lines[1], "\"line\":\"2\""));
    lines = Lines(s.Take(0));
    assert(lines.size() == 2 && Contains(lines[0], "\"seq\":5,") && Contains(lines[1], "\"line\":\"4\""));
    Subscription::Counts c = s.counts();
    assert(c.sent == 6 && c.dropped == 2 && c.pending == 0);

    // A reader that waits is woken by a push from another thread.
    std::thread pusher([&s] {
        SleepMs(30);
        s.Push(NotificationKind::Date, Raw("1719.5.1"), 3000, ",\"unit\":\"month\"");
    });
    const std::uint64_t before = SteadyMs();
    lines = Lines(s.Take(5000));
    pusher.join();
    assert(lines.size() == 1 && Contains(lines[0], "\"notification\":\"date\",\"seq\":7,") && SteadyMs() - before < 4000);

    s.Push(NotificationKind::State, std::nullopt, 3100, "");
    s.Push(NotificationKind::State, std::nullopt, 3100, "");
    assert(s.Discard() == 2 && s.Take(0).empty() && s.counts().sent == 7);
}

static void TestNotificationHub() {
    std::uint64_t clock = 500;
    NotificationHub hub([&clock] { return clock; });
    assert(hub.wanted() == 0 && !hub.WaitFor(kAllNotificationKinds, 1));
    SubscribeRequest by_day;
    by_day.kinds = Bit(NotificationKind::Date) | Bit(NotificationKind::State);
    by_day.date_unit = DateUnit::Day;
    SubscribeRequest activity;
    activity.kinds = Bit(NotificationKind::Activity) | Bit(NotificationKind::Date);
    const auto a = hub.Subscribe(by_day);
    clock = 600;
    const auto b = hub.Subscribe(activity);
    assert(hub.subscriptions() == 2 && hub.Wants(NotificationKind::Activity) && !hub.Wants(NotificationKind::Event));
    assert(hub.WaitFor(Bit(NotificationKind::State), 1));

    clock = 700;
    hub.PublishDate(Raw("1719.3.17"), Raw("1719.3.18"));  // a new day, not a new month
    hub.PublishDate(Raw("1719.3.31"), Raw("1719.4.1"));
    hub.Publish(NotificationKind::Activity, Raw("1719.4.1"), ",\"what\":\"input\"");
    std::vector<std::string> la = Lines(a->Take(0));
    std::vector<std::string> lb = Lines(b->Take(0));
    assert(la.size() == 2 && lb.size() == 2);
    assert(la[0] == "{\"notification\":\"date\",\"seq\":1,\"date\":\"1719.3.18\",\"wall_ms\":200,\"unit\":\"day\",\"previous\":\"1719.3.17\"}");
    assert(Contains(la[1], "\"date\":\"1719.4.1\"") && Contains(la[1], "\"unit\":\"day\""));
    assert(lb[0] == "{\"notification\":\"date\",\"seq\":1,\"date\":\"1719.4.1\",\"wall_ms\":100,\"unit\":\"month\",\"previous\":\"1719.3.31\"}");
    assert(Contains(lb[1], "\"notification\":\"activity\",\"seq\":2,"));

    hub.Unsubscribe(b);
    assert(hub.subscriptions() == 1 && !hub.Wants(NotificationKind::Activity) && hub.Wants(NotificationKind::Date));
    hub.Publish(NotificationKind::Activity, std::nullopt, "");
    assert(a->Take(0).empty() && b->Take(0).empty());
    hub.Unsubscribe(a);
    assert(hub.wanted() == 0);

    // Choices made through Tongs, by controller and event id.
    hub.NoteChoice(0xC1, "a.1", "{\"number\":1}");
    assert(!hub.TakeChoice(0xC1, "a.2") && !hub.TakeChoice(0xC2, "a.1"));
    assert(*hub.TakeChoice(0xC1, "a.1") == "{\"number\":1}" && !hub.TakeChoice(0xC1, "a.1"));
    for (int i = 0; i < 40; ++i) hub.NoteChoice(static_cast<std::uintptr_t>(i), "e", "x");
    assert(!hub.TakeChoice(0, "e") && hub.TakeChoice(39, "e"));  // only the newest 32 are kept
}

static void TestStateChanges() {
    StateSnapshot menu;
    menu.reason = "session is null";
    StateSnapshot game = LoadedGame().state;  // paused, UI speed 2
    game.session = 0x5E55;

    std::vector<std::string> c = StateChanges(menu, game);
    assert(c.size() == 1 && c[0] == ",\"change\":\"entered_game\",\"speed\":2,\"paused\":true,\"paused_flag\":true");
    assert(StateChanges(game, game).empty());
    assert(StateChanges(menu, menu).empty());

    StateSnapshot running = game;
    running.paused = running.paused_flag = false;
    running.speed = 4;
    running.date_raw += 48;  // a date change alone is a `date` notification, not a `state` one
    c = StateChanges(game, running);
    assert(c.size() == 2 && c[0] == ",\"change\":\"resumed\",\"speed\":5,\"paused\":false,\"paused_flag\":false");
    assert(c[1] == ",\"change\":\"speed\",\"speed\":5,\"paused\":false,\"paused_flag\":false,\"previous_speed\":2");
    c = StateChanges(running, game);
    assert(c.size() == 2 && Contains(c[0], "\"change\":\"paused\"") && Contains(c[1], "\"previous_speed\":5"));

    c = StateChanges(game, menu);
    assert(c.size() == 1 && c[0] == ",\"change\":\"left_game\",\"reason\":\"session is null\"");
    // Another save in a session at another address: left, then entered.
    StateSnapshot other = game;
    other.session = 0x5E56;
    c = StateChanges(game, other);
    assert(c.size() == 2 && c[0] == ",\"change\":\"left_game\"" && Contains(c[1], "entered_game"));
}

static void TestEventChanges() {
    const UiTree t = EventTree();
    EventsRaw raw = EventRaw(t);
    const std::vector<EventView> both = BuildEvents(t, raw);  // Tariffs on Fish (0xC2) on top of Plague (0xC1)
    std::vector<KnownEvent> known;
    std::vector<std::string> c = EventChanges(known, both, {});
    assert(c.size() == 2 && known.size() == 2);
    assert(c[0] == ",\"change\":\"opened\",\"id\":\"flavor_x.2\",\"title\":\"Plague\",\"options\":[{\"number\":1,\"text\":\"Pray\","
                   "\"option_index\":0,\"option_key\":\"flavor_x.2.a\"}]");
    assert(Contains(c[1], "\"id\":\"flavor_rus.1\",\"title\":\"Tariffs on Fish\",\"options\":[{\"number\":1,") &&
           Contains(c[1], "{\"number\":2,\"text\":\"Refuse them\",\"option_index\":1,\"option_key\":\"flavor_rus.1.b\"}"));
    assert(EventChanges(known, both, {}).empty());

    // Plague chosen through Tongs, Tariffs on Fish by someone else.
    raw.events[1].waiting = false;
    std::vector<std::uintptr_t> asked;
    const ChoiceLookup lookup = [&asked](std::uintptr_t controller, const std::string& id) -> std::optional<std::string> {
        asked.push_back(controller);
        if (controller == 0xC1 && id == "flavor_x.2") return std::string("{\"number\":1,\"by\":\"tongs\"}");
        return std::nullopt;
    };
    c = EventChanges(known, BuildEvents(t, raw), lookup);
    assert(c.size() == 1 && c[0] == ",\"change\":\"closed\",\"id\":\"flavor_x.2\",\"title\":\"Plague\",\"chosen\":{\"number\":1,\"by\":\"tongs\"}");
    raw.events[2].waiting = false;
    c = EventChanges(known, BuildEvents(t, raw), lookup);
    assert(c.size() == 1 && Contains(c[0], "\"title\":\"Tariffs on Fish\",\"chosen\":null") && known.empty());
    assert((asked == std::vector<std::uintptr_t>{0xC1, 0xC2}));

    // The same controller with another event: the old one closed, the new one opened.
    raw = EventRaw(t);
    known.clear();
    EventChanges(known, BuildEvents(t, raw), {});
    raw.events[1].id = "flavor_x.3";
    c = EventChanges(known, BuildEvents(t, raw), {});
    assert(c.size() == 2 && Contains(c[0], "\"change\":\"closed\",\"id\":\"flavor_x.2\"") &&
           Contains(c[1], "\"change\":\"opened\",\"id\":\"flavor_x.3\""));
}

static void TestLineTail() {
    LineTail tail;
    // The first chunk sets the start: what was there before is not reported.
    LineTail::Lines got = tail.Feed({120, 7, "", false});
    assert(got.lines.empty() && !got.restarted && tail.started() && tail.offset() == 120);
    got = tail.Feed({140, 7, "first line\r\nsecond", false});
    assert(got.lines == std::vector<std::string>{"first line"} && tail.offset() == 138);
    got = tail.Feed({150, 7, " half\n\n", false});
    assert((got.lines == std::vector<std::string>{"second half", ""}));
    // Windows-1252 bytes become UTF-8; UTF-8 stays as it is.
    got = tail.Feed({160, 7, "M\xFCnster \xA7Y\n" "K\xC3\xB6ln\n", false});
    assert((got.lines == std::vector<std::string>{"M\xC3\xBCnster \xC2\xA7Y", "K\xC3\xB6ln"}));
    // A long line is cut at kMaxLine.
    got = tail.Feed({0, 7, std::string(LineTail::kMaxLine + 100, 'x') + "\n", false});
    assert(got.restarted);  // the size went down: truncated, read again from 0
    assert(tail.offset() == 0);
    got = tail.Feed({LineTail::kMaxLine + 101, 7, std::string(LineTail::kMaxLine + 100, 'x') + "\n", false});
    assert(got.lines.size() == 1 && got.lines[0].size() == LineTail::kMaxLine);
    // Another file under the same name (the game moves error.log aside at start).
    got = tail.Feed({LineTail::kMaxLine + 500, 8, "old offset", false});
    assert(got.restarted && got.lines.empty() && tail.offset() == 0);
    got = tail.Feed({20, 8, "new file\n", false});
    assert(got.lines == std::vector<std::string>{"new file"});
    tail.Reset();
    assert(!tail.started() && tail.offset() == 0);
    assert(LogTextToUtf8("plain") == "plain" && LogTextToUtf8("\x80") == "\xE2\x82\xAC" && LogTextToUtf8("\xC3") == "\xC3\x83");
}

static void TestNotificationSources() {
    // State and date.
    FakeGame game = LoadedGame();
    game.state.session = 0x5E55;
    Handlers h = game.Make();
    NotificationHub hub(h.now_ms);
    h.notifications = &hub;
    SubscribeRequest request;
    request.kinds = Bit(NotificationKind::State) | Bit(NotificationKind::Date);
    const auto sub = hub.Subscribe(request);
    NotificationSources sources(h, hub);
    sources.PollState();  // the baseline
    assert(sub->Take(0).empty());
    game.state.paused = game.state.paused_flag = false;
    game.state.date_raw = Raw("1719.3.31");
    sources.PollState();
    std::vector<std::string> lines = Lines(sub->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"notification\":\"state\",\"seq\":1,\"date\":\"1719.3.31\"") &&
           Contains(lines[0], "\"change\":\"resumed\""));
    game.state.date_raw = Raw("1719.4.2");
    sources.PollState();
    lines = Lines(sub->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"notification\":\"date\",\"seq\":2,\"date\":\"1719.4.2\"") &&
           Contains(lines[0], "\"previous\":\"1719.3.31\""));
    game.state = {};
    game.state.reason = "session is null";
    sources.PollState();
    lines = Lines(sub->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"date\":null") && Contains(lines[0], "\"change\":\"left_game\""));
    hub.Unsubscribe(sub);

    // Events: read on the main thread when the window count changes.
    FakeScreen screen;
    Handlers hs = screen.Make();
    bool no_session = false;
    hs.count_event_windows = [&screen, &no_session] {
        return no_session ? -1 : static_cast<int>(screen.raw.listed_windows.size());
    };
    NotificationHub event_hub(hs.now_ms);
    hs.notifications = &event_hub;
    request.kinds = Bit(NotificationKind::Event);
    const auto events = event_hub.Subscribe(request);
    NotificationSources event_sources(hs, event_hub);
    FakeMainThread main(screen.dispatcher);
    event_sources.PollEvents();  // the two open events are the baseline
    event_sources.PollEvents();
    assert(events->Take(0).empty());
    assert(Contains(HandleRequest("event choose 1 event=title^=tariffs", hs, 3), "\"ok\":true"));
    event_sources.PollEvents();
    lines = Lines(events->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"change\":\"closed\",\"id\":\"flavor_rus.1\"") &&
           Contains(lines[0], "\"chosen\":{\"number\":1,\"text\":\"Accept the tariffs\",\"option_index\":0,\"option_key\":\"flavor_rus.1.a\",\"by\":\"tongs\",\"client\":3,\"choice_date\":\"1719.3.17\"}"));
    // An event opens: the count goes up.
    screen.raw = EventRaw(screen.tree);
    screen.raw.events.erase(screen.raw.events.begin() + 1);  // without Plague, which is still known: it closed
    event_sources.PollEvents();
    lines = Lines(events->Take(0));
    assert(lines.size() == 2 && Contains(lines[0], "\"change\":\"closed\",\"id\":\"flavor_x.2\",\"title\":\"Plague\",\"chosen\":null") &&
           Contains(lines[1], "\"change\":\"opened\",\"id\":\"flavor_rus.1\""));
    // No session: what was known closes without the main thread.
    no_session = true;
    event_sources.PollEvents();
    lines = Lines(events->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"date\":null") && Contains(lines[0], "\"change\":\"closed\",\"id\":\"flavor_rus.1\""));
    event_sources.PollEvents();
    assert(events->Take(0).empty());

    // error.log: an in-memory file the game appends to.
    FakeGame log_game = LoadedGame();
    Handlers hl = log_game.Make();
    NotificationHub log_hub(hl.now_ms);
    std::string file = "old line\n";
    std::uint64_t file_id = 1;
    int locates = 0;
    hl.locate_error_log = [&locates] {
        ErrorLogLocation where;
        if (++locates == 1) {
            where.error = "not yet";
            return where;
        }
        where.path = "C:\\user\\logs\\error.log";
        where.source = "physfs_write_dir";
        return where;
    };
    hl.read_log_chunk = [&](const std::string& path, std::uint64_t offset, std::size_t max, LineTail::Chunk& chunk) {
        assert(path == "C:\\user\\logs\\error.log");
        chunk = {file.size(), file_id, offset < file.size() ? file.substr(offset, max) : std::string(), false};
        return std::string();
    };
    request.kinds = Bit(NotificationKind::ErrorLog);
    const auto log = log_hub.Subscribe(request);
    NotificationSources log_sources(hl, log_hub);
    assert(log_sources.PollErrorLog() == 0 && locates == 1);  // not located: tried again a second later
    log_sources.PollErrorLog();
    assert(locates == 1);
    log_game.clock += NotificationSources::kLocateRetryMs;
    log_sources.PollErrorLog();  // located; the end of the file is the start
    assert(locates == 2 && log->Take(0).empty());
    file += "[culture.cpp:1]: Unknown culture group \"x\"\npartial";
    log_sources.PollErrorLog();
    lines = Lines(log->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"notification\":\"error_log\",\"seq\":1,\"date\":\"1719.3.17\"") &&
           Contains(lines[0], "\"line\":\"[culture.cpp:1]: Unknown culture group \\\"x\\\"\""));
    file = "fresh\n";
    file_id = 2;
    log_sources.PollErrorLog();
    log_sources.PollErrorLog();
    lines = Lines(log->Take(0));
    assert(lines.size() == 2 && Contains(lines[0], "\"restarted\":true") && Contains(lines[1], "\"line\":\"fresh\""));
}

static void TestSubscribeCommands() {
    FakeGame game = LoadedGame();
    Handlers h = game.Make();
    std::shared_ptr<Subscription> sub;
    assert(IsSubscribe(" subscribe state") && !IsSubscribe("subscribed") && !IsSubscribe("state"));
    assert(Contains(Subscribe("subscribe state", h, sub), "\"error\":\"unavailable\"") && !sub);
    NotificationHub hub(h.now_ms);
    h.notifications = &hub;
    std::string out = Subscribe("subscribe state,date", h, sub);
    assert(out == "{\"ok\":true,\"command\":\"subscribe\",\"kinds\":[\"state\",\"date\"],\"date_unit\":\"month\",\"queue\":1000,"
                  "\"in_game\":true,\"date\":\"1719.3.17\",\"date_raw\":58860240,\"hour\":0,\"speed\":2,\"speed_internal\":1,"
                  "\"paused\":true,\"paused_flag\":true}");
    assert(sub && hub.subscriptions() == 1);
    assert(Contains(SubscribedRequest("ping", h, sub), "{\"ok\":true,\"command\":\"ping\",\"pong\":true"));
    out = SubscribedRequest("state", h, sub);
    assert(out == "{\"ok\":false,\"error\":\"subscribed\",\"detail\":\"a subscribed connection accepts only unsubscribe and ping\",\"command\":\"state\"}");
    assert(Contains(SubscribedRequest("subscribe all", h, sub), "\"error\":\"subscribed\""));
    hub.Publish(NotificationKind::State, std::nullopt, "");
    hub.Publish(NotificationKind::State, std::nullopt, "");
    assert(Lines(sub->Take(0, 1)).size() == 1);
    out = SubscribedRequest("unsubscribe", h, sub);
    assert(out == "{\"ok\":true,\"command\":\"unsubscribe\",\"sent\":1,\"dropped\":0,\"discarded\":1}" && !sub && hub.subscriptions() == 0);

    // Kinds the plugin cannot serve are named; error_log says where it reads.
    h.locate_error_log = [] { return ErrorLogLocation{"C:\\docs\\logs\\error.log", "physfs_write_dir", ""}; };
    out = Subscribe("subscribe event,error_log,activity", h, sub);
    assert(Contains(out, "\"kinds\":[\"event\",\"error_log\",\"activity\"],\"queue\":1000,\"unavailable\":[\"event\",\"error_log\"],"
                         "\"error_log\":{\"path\":\"C:\\\\docs\\\\logs\\\\error.log\",\"source\":\"physfs_write_dir\"}"));
    assert(Contains(Subscribe("subscribe nothing", h, sub), "{\"ok\":false,\"error\":\"bad_command\",\"detail\":\"unknown kind: nothing"));

    // Without a subscription: subscribe needs the pipe server, unsubscribe has nothing to end.
    assert(Contains(HandleRequest("subscribe state", h), "pipe connection of its own"));
    assert(Contains(HandleRequest("unsubscribe", h), "not subscribed"));
    assert(Contains(HandleRequest("help", h), "\"subscribe\",\"unsubscribe\""));
    assert(Contains(HandleRequest("ping", h), "\"command\":\"ping\""));

    // activity: every input of any client, with the client and the running count.
    const std::shared_ptr<Subscription> activity = sub;
    assert(Contains(HandleRequest("resume", h, 4), "\"ok\":true"));
    assert(Contains(HandleRequest("pause", h, 2), "\"ok\":true"));
    assert(Contains(HandleRequest("pause", h, 2), "\"presses\":0"));  // nothing pressed: no input
    std::vector<std::string> lines = Lines(activity->Take(0));
    assert(lines.size() == 2);
    assert(Contains(lines[0], "\"notification\":\"activity\",\"seq\":1,\"date\":\"1719.3.17\"") &&
           Contains(lines[0], "\"what\":\"input\",\"command\":\"resume\",\"inputs\":") && Contains(lines[0], ",\"client\":4}"));
    assert(Contains(lines[1], "\"command\":\"pause\"") && Contains(lines[1], ",\"client\":2}"));
    hub.Unsubscribe(activity);
}

int main() {
    TestSubscribeParsing();
    TestDateCrosses();
    TestSubscriptionQueue();
    TestNotificationHub();
    TestStateChanges();
    TestEventChanges();
    TestLineTail();
    TestNotificationSources();
    TestSubscribeCommands();
    TestEvents();
    TestUiClickTarget();
    TestUiClickCommand();
    TestEventCommands();
    TestInputsCount();
    TestRunUntilEventsFirst();
    TestDate();
    TestScreenshot();
    TestThreads();
    TestTextEvents();
    TestWindowAndMouse();
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
    TestDispatcher();
    TestFrameStats();
    TestFramesCommand();
    TestUiSelectors();
    TestUiRequests();
    TestUiCommand();
    TestConsoleRules();
    TestConsoleCommand();  // last: it trips game calls for the rest of the process
    std::puts("ok");
    return 0;
}
