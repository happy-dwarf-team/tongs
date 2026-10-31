#include "config.hpp"
#include "console.hpp"
#include "game_date.hpp"
#include "load_watch.hpp"
#include "navigation.hpp"
#include "notify_sources.hpp"
#include "protocol.hpp"
#include "ui_definitions.hpp"
#include "ui_service.hpp"
#include "ui_structure.hpp"
#include "ui_tree.hpp"
#include "world.hpp"

#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
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
    assert(Contains(HandleRequest("frames", h), "outer frames are not available"));
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
    assert(!Contains(out, "\"in_game\""));  // no state reader
    // With one, the state read on the same frame: which session the frame belongs to.
    StateSnapshot game;
    game.in_game = true;
    game.date_raw = 58'860'240;
    game.load = 2;
    h.read_state = [&game] { return game; };
    out = HandleRequest("frames", h);
    assert(Contains(out, "\"in_game\":true,\"date\":\"1719.3.17\"") && Contains(out, "\"session\":2"));
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

// SampleTree with a ticked check box and an empty edit box as top-level widgets.
static UiTree SampleTreeWithBoxes() {
    UiTree t = SampleTree();
    UiWidget box;
    box.name = "show_unsafe_checkbox";
    box.type = UiType::Checkbox;
    box.shown = true;
    box.rect = {33, 70, 38, 39};
    box.has_checked = true;
    box.checked = true;
    t.widgets.push_back(box);
    t.roots.push_back(static_cast<int>(t.widgets.size()) - 1);
    UiWidget edit;
    edit.name = "console_edit";
    edit.type = UiType::Editbox;
    edit.shown = true;
    edit.rect = {6, 972, 840, 48};
    edit.has_text = true;  // an edit box shows even an empty text
    t.widgets.push_back(edit);
    t.roots.push_back(static_cast<int>(t.widgets.size()) - 1);
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
    UiTree boxes = SampleTreeWithBoxes();
    assert(Paths(boxes, "//[checked]") == V{"show_unsafe_checkbox"} && Paths(boxes, "//[unchecked]").empty());
    boxes.widgets[boxes.widgets.size() - 2].checked = false;
    assert(Paths(boxes, "//[unchecked]") == V{"show_unsafe_checkbox"} && Paths(boxes, "//[checked]").empty());
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
    assert(Contains(HandleRequest("ui tree", h), "outer frames are not available"));
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
        h.read_ui = [](UiTree& tree) {
            tree = SampleTreeWithBoxes();
            return std::string();
        };
        out = HandleRequest("ui find //[type=checkbox]", h);
        assert(Contains(out, "\"path\":\"show_unsafe_checkbox\",\"type\":\"checkbox\"") && Contains(out, "\"checked\":true"));
        out = HandleRequest("ui find console_edit", h);
        assert(Contains(out, "\"type\":\"editbox\"") && Contains(out, "\"text\":\"\"") && !Contains(out, "checked"));
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

    // While a save is read into the session, its date is 1.1.1.
    assert(IsUnsetGameDate(43'800'000 + 365 * 24) && FormatGameDate(DecodeGameDate(43'800'000 + 365 * 24)) == "1.1.1");
    assert(!IsUnsetGameDate(43'800'000 + 365 * 24 + 1) && !IsUnsetGameDate(58'860'240) && !IsUnsetGameDate(0));
}

static void TestLoadWatch() {
    LoadWatch watch;
    const auto read = [&watch](bool in_game, std::uintptr_t session, const char* date) {
        StateSnapshot s;
        s.in_game = in_game;
        s.session = session;
        if (in_game) s.date_raw = *ParseGameDate(date);
        watch.Observe(s);
        return s.load;
    };
    assert(read(false, 0, nullptr) == 0);              // main menu
    const std::uint64_t first = read(true, 0x5000, "1719.3.17");
    assert(first != 0 && read(true, 0x5000, "1719.3.17") == first && read(true, 0x5000, "1719.4.1") == first);
    // The same save again: the session object stays, the date goes back to where the save is.
    assert(read(false, 0, nullptr) == 0);
    const std::uint64_t second = read(true, 0x5000, "1719.3.17");
    assert(second != first && read(true, 0x5000, "1719.3.18") == second);
    // Without a read in the gap: another session object, or an earlier date.
    const std::uint64_t third = read(true, 0x6000, "1719.3.18");
    assert(third != second);
    assert(read(true, 0x6000, "1719.3.1") != third);
}

// The 0.12 default config.toml, which a player may still have.
constexpr char kOldDefaultConfig[] =
    "# Tongs\n"
    "enabled = false\n"
    "input = false\n"
    "game_calls = false\n"
    "console_allow = []\n"
    "game_calls_ironman = false\n"
    "game_calls_multiplayer = false\n";

static void TestConfig() {
    // config.toml of Tongs 0.12 and older, read once to move it into settings.toml.
    assert(!ParseConfig("").enabled);
    assert(ParseConfig(kOldDefaultConfig).notes.empty());
    assert(!ParseConfig(kOldDefaultConfig).enabled);
    assert(ParseConfig("enabled = true\r\n").enabled);
    assert(ParseConfig("  enabled=true # comment\n").enabled);

    const Config bad = ParseConfig("enabled = yes\nport = 5\nnonsense\n");
    assert(!bad.enabled);
    assert(bad.notes.size() == 3);

    assert(!ParseConfig(kOldDefaultConfig).input);
    const Config both = ParseConfig("enabled = true\ninput = true\n");
    assert(both.enabled && both.input && both.notes.empty());
    assert(ParseConfig("input = 1\n").notes.size() == 1);

    // Game calls: off by default, each switch on its own line.
    const Config defaults = ParseConfig(kOldDefaultConfig);
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

    // The player's settings.toml written from it: only what differs from the defaults (all false, "").
    assert(SettingsFromConfig(defaults).empty());
    assert(SettingsFromConfig(both) == "enabled = true\ninput = true\n");
    assert(SettingsFromConfig(calls) ==
           "game_calls = true\nconsole_allow = \"leak, oos\"\ngame_calls_ironman = true\n");

    // console_allow in settings.toml: names separated by commas or white space.
    std::vector<std::string> names;
    std::vector<std::string> notes;
    assert(ParseConsoleAllow("", names, notes) && names.empty() && notes.empty());
    assert(ParseConsoleAllow("leak, oos", names, notes) && notes.empty());
    assert(names == std::vector<std::string>({"leak", "oos"}));
    assert(ParseConsoleAllow(" leak\toos,,", names, notes) && names.size() == 2 && notes.empty());
    assert(!ParseConsoleAllow("leak; oos", names, notes) && names.empty() && notes.size() == 1);
    notes.clear();
    assert(ParseConsoleAllow("enable_all_commands speed leak", names, notes) && names.size() == 3 && notes.size() == 2);
    // What SettingsFromConfig writes reads back as the same list.
    notes.clear();
    assert(ParseConsoleAllow("leak, oos", names, notes) && names == calls.game_calls.allow);
}

static void TestProtocol() {
    Handlers h;
    h.process_id = 1234;
    StateSnapshot snapshot;
    h.read_state = [&] { return snapshot; };

    assert(Contains(HandleRequest("ping\r\n", h), "\"pong\":true"));
    assert(Contains(HandleRequest("ping", h), "\"pid\":1234}"));
    // The process identity: in ping, and in every answer that carries the state.
    h.process_start_ms = 1'792'418'572'041ULL;
    assert(Contains(HandleRequest("ping", h), "\"pid\":1234,\"process_start_ms\":1792418572041}"));
    assert(!Contains(HandleRequest("state", h), "\"pid\""));
    snapshot.pid = 1234;
    snapshot.process_start_ms = 1'792'418'572'041ULL;
    assert(Contains(HandleRequest("state", h), "\"in_game\":false,\"reason\":\"\",\"pid\":1234,\"process_start_ms\":1792418572041"));
    snapshot.in_game = true;
    snapshot.date_raw = 58'860'240;
    assert(Contains(HandleRequest("state", h), "\"paused_flag\":false,\"pid\":1234,\"process_start_ms\":1792418572041"));
    snapshot = StateSnapshot{};
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
    assert(!Contains(out, "\"session\""));  // not observed by LoadWatch
    snapshot.load = 3;
    assert(Contains(HandleRequest("state", h), "\"paused_flag\":true,\"session\":3,"));
    assert(Contains(HandleRequest("state", h), "\"game_calls_total\":0,\"client_calls_total\":0"));

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

// While Anvil suspends Tongs (ironman or multiplayer without the player's opt-in),
// input commands answer an error and send nothing; reads go on.
static void TestSuspended() {
    FakeGame game = LoadedGame();
    Handlers h = game.Make();
    bool suspended = true;
    h.suspended = [&suspended] { return suspended; };
    for (const char* command : {"key space", "pause", "resume", "speed 3", "text a", "move 1 1", "click 1 1",
                                "scroll 1", "run_until 1719.4.1", "ui click button"}) {
        const std::string out = HandleRequest(command, h);
        assert(Contains(out, "\"ok\":false") && Contains(out, "Anvil suspended Tongs") && Contains(out, "allow_ironman"));
    }
    assert(game.pressed.empty() && game.events.empty());
    // Handing the pointer back to the player changes nothing in the game.
    assert(Contains(HandleRequest("release", h), "\"ok\":true"));
    assert(Contains(HandleRequest("state", h), "\"ok\":true"));
    assert(Contains(HandleRequest("help", h), "\"input\":true") && Contains(HandleRequest("help", h), "\"suspended\":true"));
    suspended = false;
    assert(Contains(HandleRequest("key space", h), "\"ok\":true") && game.pressed.size() == 1);
    assert(Contains(HandleRequest("help", h), "\"suspended\":false"));
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

    // A load between two polls: the session number changes, and the wait ends.
    game.state.load = 1;
    int polls = 0;
    game.on_sleep = [&] {
        if (++polls == 3) game.state.load = 2;
    };
    out = HandleRequest("wait_until 1800.1.1", h);
    assert(Contains(out, "\"ok\":false") && Contains(out, "the game was left while waiting") && Contains(out, "\"session\":2"));
    game.on_sleep = nullptr;
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
    // No press, but the state afterwards is still read (it answered in_game false before).
    assert(Contains(out, "\"in_game\":true") && Contains(out, "\"date\":"));
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
    std::size_t sent_before_first_sleep = 0;
    game.on_sleep = [&] {
        if (sent_before_first_sleep == 0) sent_before_first_sleep = game.events.size();
    };
    out = HandleRequest("scroll -3 50 60", h);
    game.on_sleep = nullptr;
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"steps\":-3") && game.events.size() == 4);
    // The move is sent alone and given time to reach the interface before the wheel turns.
    assert(sent_before_first_sleep == 1 && game.events[0].type == InputEvent::Type::MouseMove);
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
    std::function<void(std::uintptr_t)> on_click;  // called with the widget under every click
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
                if (on_click) on_click(under);
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

static void TestChildrenBounds() {
    UiTree t;
    // A scroll bar as read live: its own position relative, its buttons in client pixels.
    const int bar = AddWidget(t, "standardlistbox_slider", UiType::Other, true, {425, 0, 15, 876});
    assert(ChildrenBounds(t, bar).x == 425);  // no children: its own rect
    AddWidget(t, "listboxSliderButton", UiType::Button, true, {444, 130, 19, 19}, bar);
    AddWidget(t, "listboxTrackButton", UiType::Button, true, {446, 130, 15, 838}, bar);
    AddWidget(t, "downButton", UiType::Button, true, {444, 968, 19, 19}, bar);
    AddWidget(t, "upButton", UiType::Button, true, {444, 111, 19, 19}, bar);
    AddWidget(t, "empty", UiType::Button, true, {0, 0, 0, 19}, bar);  // no size: left out
    const UiRect r = ChildrenBounds(t, bar);
    assert(r.x == 444 && r.y == 111 && r.w == 19 && r.h == 876);
}

static void TestFitRects() {
    UiTree t;
    // The main menu as read live (3440 x 1440): a fullscreen window placed by its .gui
    // position, a container without a size, a hidden icon in it, a text parked off the screen.
    const int bottom = AddWidget(t, "mainmenu_panel_bottom", UiType::Window, true, {1057, 1339, 3440, 1440});
    AddWidget(t, "single_player_button", UiType::Button, true, {1440, 1309, 331, 48}, bottom);
    const int cont = AddWidget(t, "continue_container", UiType::Window, true, {1518, 1226, 0, 0}, bottom);
    AddWidget(t, "frontend_continue_bg", UiType::Icon, true, {1473, 1197, 497, 105}, cont);
    AddWidget(t, "ironman_icon", UiType::Icon, false, {1720, 677, 30, 43}, cont);  // hidden: left out
    const int tall = AddWidget(t, "games", UiType::Listbox, true, {1534, 352, 384, 3724}, bottom);
    const int parked = AddWidget(t, "observe_mode_title", UiType::Text, true, {119970, 1233, 240, 24});
    const int corner = AddWidget(t, "speed_controls", UiType::Window, true, {3440, 0, 0, 0});
    UiTree unknown = t;
    FitRects(t, {0, 0, 3440, 1440});
    const UiRect& c = t.widgets[cont].rect;
    assert(c.x == 1473 && c.y == 1197 && c.w == 497 && c.h == 105);
    const UiRect& b = t.widgets[bottom].rect;  // its shown children, clipped first
    assert(b.x == 1440 && b.y == 352 && b.w == 1970 - 1440 && b.h == 1440 - 352);
    assert(t.widgets[tall].rect.h == 1440 - 352);
    assert(!t.widgets[parked].shown && t.widgets[parked].rect.x == 119970);
    assert(t.widgets[corner].shown && t.widgets[corner].rect.x == 3440);  // no size, no children: on the edge
    // Without a client area only windows without a size change.
    FitRects(unknown, {});
    assert(unknown.widgets[cont].rect.w == 497 && unknown.widgets[bottom].rect.w == 3440 && unknown.widgets[parked].shown);
}

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
    // No hovered widget at first (seen live after a click on a scroll bar button): the move is sent again.
    screen.game.events.clear();
    int misses = 1;
    Handlers flaky = h;
    flaky.read_hovered = [&screen, &misses] { return misses-- > 0 ? std::uintptr_t{0} : screen.Hovered(); };
    out = HandleRequest("ui click speed_controls/button_speedup", flaky);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"hover_attempts\":2"));
    // The second try steps one pixel aside and back, so the game sees the pointer move.
    assert(screen.game.events.size() == 5 && screen.game.events[1].type == InputEvent::Type::MouseMove &&
           screen.game.events[1].x == 3351 && screen.game.events[2].x == 3350 &&
           screen.game.events[3].type == InputEvent::Type::MouseDown && screen.game.cursor.first == 3350);
    // Nothing under the pointer at all: three moves, then `covered` without a press.
    screen.game.events.clear();
    flaky.read_hovered = [] { return std::uintptr_t{0}; };
    out = HandleRequest("ui click speed_controls/button_speedup", flaky);
    assert(Contains(out, "\"error\":\"covered\"") && Contains(out, "\"hover_attempts\":3") &&
           Contains(out, "\"covered_by\":null"));
    assert(screen.game.events.size() == 5 && screen.game.events[4].type == InputEvent::Type::MouseMove);
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

static void TestUiClickChoice() {
    FakeScreen screen;
    NotificationHub hub(SteadyMs);
    Handlers h = screen.Make();
    h.notifications = &hub;
    // The note must be there before any waiting after the click: the event reader
    // may see the event close within a few frames.
    bool clicked = false;
    std::optional<std::string> note;
    screen.on_click = [&clicked](std::uintptr_t) { clicked = true; };
    const auto sleep = h.sleep_ms;
    h.sleep_ms = [&, sleep](int ms) {
        if (clicked && !note) note = hub.TakeChoice(0xC2, "flavor_rus.1").value_or("none at the first wait");
        sleep(ms);
    };
    FakeMainThread main(screen.dispatcher);
    // A plain ui click on an option button: Tongs knows which option it clicked.
    std::string out = HandleRequest("ui click EventWindow[1]/event_option_listbox/event_option_entry[2]/event_option_button", h);
    assert(Contains(out, "\"ok\":true") &&
           Contains(out, "\"event_option\":{\"id\":\"flavor_rus.1\",\"number\":2,\"option_index\":1}"));
    assert(screen.chosen == std::vector<std::string>{"5_1"});
    assert(note && Contains(*note, "\"option_index\":1") && Contains(*note, "\"by\":\"tongs\"") &&
           Contains(*note, "\"via\":\"ui click\"") && Contains(*note, "\"choice_date\":\"1719.3.17\""));
    // Any other widget: no event_option, nothing noted.
    out = HandleRequest("ui click speed_controls/button_speedup", h);
    assert(Contains(out, "\"ok\":true") && !Contains(out, "event_option"));
    // The events cannot be read: the click still happens, without the note.
    Handlers broken = h;
    broken.read_events = [](EventsRaw&) { return std::string("access violation while reading events"); };
    out = HandleRequest("ui click speed_controls/button_speedup", broken);
    assert(Contains(out, "\"ok\":true") && !Contains(out, "event_option"));
}

static void TestEventChooseScrolls() {
    FakeScreen screen;
    UiTree& t = screen.tree;
    // Tariffs on Fish with a two-row list box: its third option (enabled here) is a hidden row.
    t.widgets[15].enabled = true;
    t.widgets[14].shown = false;
    const Handlers h = screen.Make();
    FakeMainThread main(screen.dispatcher);
    std::string out = HandleRequest("event choose 3 event=title^=tariffs", h);
    assert(Contains(out, "\"error\":\"not_visible\"") && !Contains(out, "scrolled"));  // no scroll bar
    assert(screen.game.events.empty());

    const int bar = AddWidget(t, "standardlistbox_slider", UiType::Other, true, {1923, 900, 19, 180}, 9);
    AddWidget(t, "upButton", UiType::Button, true, {1923, 900, 19, 19}, bar);
    const int down = AddWidget(t, "downButton", UiType::Button, true, {1923, 1061, 19, 19}, bar);
    int downs = 0;
    screen.on_click = [&](std::uintptr_t under) {
        if (under != Address(down)) return;
        ++downs;  // the list moves one row: the first option leaves, the third comes in
        t.widgets[10].shown = false;
        t.widgets[14].shown = true;
    };
    out = HandleRequest("event choose 3 event=title^=tariffs", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"scrolled\":1") && Contains(out, "\"option_index\":3"));
    assert(downs == 1 && screen.chosen == std::vector<std::string>{"5_3"});
}

static void TestEventCommands() {
    FakeScreen screen;
    const Handlers h = screen.Make();
    assert(Contains(HandleRequest("event list", Handlers{}), "outer frames are not available"));
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
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"inputs\":1,\"client_calls\":0,\"game_calls\":0"));
    // A client call of another client during the wait (ADR 0003).
    ticks = 0;
    game.on_sleep = [&] {
        if (++ticks == 5) assert(CountClientCall(h, "province open", game.state) >= 1);
    };
    out = HandleRequest("wait_until 1719.4.15", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"inputs\":0,\"client_calls\":1,\"game_calls\":0"));
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

    // Suspended by Anvil: refused before the main thread, whatever Tongs' own switches say.
    Handlers suspended = h;
    suspended.suspended = [] { return true; };
    out = HandleRequest("console echo x", suspended);
    assert(Contains(out, "{\"ok\":false,\"error\":\"suspended\"") && Contains(out, "allow_multiplayer") && console.calls.empty());

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
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"inputs\":0,\"client_calls\":0,\"game_calls\":1,"));
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
    // Reloading the same save keeps the session object and the date; the load the state reads saw tells it.
    assert(Contains(HandleRequest("console echo y", h), "\"game_calls_total\":1"));
    assert(Contains(HandleRequest("state", h), "\"game_calls_total\":1"));
    console.game.state.load = 7;
    assert(Contains(HandleRequest("state", h), "\"game_calls_total\":0"));
    assert(Contains(HandleRequest("console echo y", h), "\"game_calls_total\":1"));

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
    // Another session at the same address (a reload LoadWatch saw): left, then entered with its number.
    game.load = 1;
    other = game;
    other.load = 2;
    c = StateChanges(game, other);
    assert(c.size() == 2 && c[0] == ",\"change\":\"left_game\"" &&
           c[1] == ",\"change\":\"entered_game\",\"speed\":2,\"paused\":true,\"paused_flag\":true,\"session\":2");
}

static void TestEventChanges() {
    const UiTree t = EventTree();
    EventsRaw raw = EventRaw(t);
    const std::vector<EventView> both = BuildEvents(t, raw);  // Tariffs on Fish (0xC2) on top of Plague (0xC1)
    std::vector<KnownEvent> known;
    EventDiff d = DiffEvents(known, both);
    assert(d.closed.empty() && d.opened.size() == 2 && known.size() == 2);
    assert(d.opened[0] == ",\"change\":\"opened\",\"id\":\"flavor_x.2\",\"title\":\"Plague\",\"options\":[{\"number\":1,\"text\":\"Pray\","
                          "\"option_index\":0,\"option_key\":\"flavor_x.2.a\"}]");
    assert(Contains(d.opened[1], "\"id\":\"flavor_rus.1\",\"title\":\"Tariffs on Fish\",\"options\":[{\"number\":1,") &&
           Contains(d.opened[1], "{\"number\":2,\"text\":\"Refuse them\",\"option_index\":1,\"option_key\":\"flavor_rus.1.b\"}"));
    assert(known[1].options.size() == 3 && known[1].options[2].index == 3);  // kept for `chosen`
    d = DiffEvents(known, both);
    assert(d.closed.empty() && d.opened.empty());

    // Plague closes, then Tariffs on Fish.
    raw.events[1].waiting = false;
    d = DiffEvents(known, BuildEvents(t, raw));
    assert(d.closed.size() == 1 && d.closed[0].controller == 0xC1 && d.opened.empty());
    assert(ClosedMembers(d.closed[0], "{\"number\":1,\"by\":\"tongs\"}") ==
           ",\"change\":\"closed\",\"id\":\"flavor_x.2\",\"title\":\"Plague\",\"chosen\":{\"number\":1,\"by\":\"tongs\"}");
    raw.events[2].waiting = false;
    d = DiffEvents(known, BuildEvents(t, raw));
    assert(d.closed.size() == 1 && d.closed[0].title == "Tariffs on Fish" && known.empty());
    assert(Contains(ClosedMembers(d.closed[0], "null"), "\"title\":\"Tariffs on Fish\",\"chosen\":null"));

    // The same controller with another event: the old one closed, the new one opened.
    raw = EventRaw(t);
    known.clear();
    DiffEvents(known, BuildEvents(t, raw));
    raw.events[1].id = "flavor_x.3";
    d = DiffEvents(known, BuildEvents(t, raw));
    assert(d.closed.size() == 1 && d.closed[0].id == "flavor_x.2" && d.opened.size() == 1 &&
           Contains(d.opened[0], "\"change\":\"opened\",\"id\":\"flavor_x.3\""));
}

static void TestChoiceLog() {
    // The lines the game writes (live, EU4 1.37.5).
    std::optional<LoggedChoice> c = ParseChoiceLine("[eventwindow.cpp:486]: Selected event option 10 for event 5060");
    assert(c && !c->automatic && c->option_index == 10 && c->id == "5060");
    c = ParseChoiceLine("[eventwindow.cpp:486]: Selected event option 2 for event flavor_rus.129");
    assert(c && c->option_index == 2 && c->id == "flavor_rus.129");
    c = ParseChoiceLine("[eventwindow.cpp:494]: Auto-selected event option 0 for event parlaments.1 ");
    assert(c && c->automatic && c->option_index == 0 && c->id == "parlaments.1");
    assert(!ParseChoiceLine("[eventwindow.cpp:486]: Selected event option x for event a.1"));
    assert(!ParseChoiceLine("[eventwindow.cpp:486]: Selected event option -1 for event a.1"));
    assert(!ParseChoiceLine("[eventwindow.cpp:486]: Selected event option 1 for event "));
    assert(!ParseChoiceLine("[effect.cpp:12]: Selected event option 1 for event a.1"));  // only the event window's own line
    assert(!ParseChoiceLine("Selected event option 1 for event a.1"));

    // The option as the event opened: hidden options keep their index.
    const UiTree t = EventTree();
    std::vector<KnownEvent> known;
    DiffEvents(known, BuildEvents(t, EventRaw(t)));
    const KnownEvent plague = known[0], fish = known[1];
    assert(LoggedChoiceJson(fish, {false, 3, "flavor_rus.1"}) ==
           "{\"number\":3,\"text\":\"Ask the merchants\",\"option_index\":3,\"option_key\":\"\",\"by\":\"player\"}");
    assert(LoggedChoiceJson(fish, {true, 2, "flavor_rus.1"}) ==
           "{\"number\":null,\"text\":null,\"option_index\":2,\"option_key\":null,\"by\":\"auto\"}");

    using Closed = std::vector<std::pair<KnownEvent, std::optional<std::string>>>;
    ChoiceMatcher m;
    // The usual order: the line is there before the event is seen closed.
    m.Logged({false, 1, "flavor_rus.1"}, 100);
    m.Closed(Closed{{fish, std::nullopt}}, 110);
    std::vector<std::string> due = m.Due(110, true);
    assert(due.size() == 1 && Contains(due[0], "\"title\":\"Tariffs on Fish\",\"chosen\":{\"number\":2,\"text\":\"Refuse them\"") &&
           Contains(due[0], "\"by\":\"player\"}") && !m.waiting());
    // The line after the event: it waits for it, then for nothing more than kWaitMs.
    m.Closed(Closed{{plague, std::nullopt}}, 200);
    assert(m.Due(250, true).empty() && m.waiting());
    m.Logged({true, 0, "flavor_x.2"}, 260);
    due = m.Due(260, true);
    assert(due.size() == 1 && Contains(due[0], "\"chosen\":{\"number\":1,\"text\":\"Pray\",\"option_index\":0,\"option_key\":\"flavor_x.2.a\",\"by\":\"auto\"}"));
    m.Closed(Closed{{plague, std::nullopt}}, 300);
    assert(m.Due(300 + ChoiceMatcher::kWaitMs - 1, true).empty());
    due = m.Due(300 + ChoiceMatcher::kWaitMs, true);
    assert(due.size() == 1 && Contains(due[0], "\"chosen\":null"));
    // Without game.log nothing waits.
    m.Closed(Closed{{plague, std::nullopt}}, 2000);
    due = m.Due(2000, false);
    assert(due.size() == 1 && Contains(due[0], "\"chosen\":null"));

    // A choice Tongs noted comes at once and takes its line when the line comes,
    // so the line does not go to the next event with that id.
    m.Closed(Closed{{fish, std::string("{\"by\":\"tongs\"}")}}, 3000);
    due = m.Due(3000, true);
    assert(due.size() == 1 && Contains(due[0], "\"chosen\":{\"by\":\"tongs\"}") && m.waiting());
    m.Logged({false, 0, "flavor_rus.1"}, 3010);
    assert(m.Due(3010, true).empty() && !m.waiting());
    m.Closed(Closed{{fish, std::nullopt}}, 3100);
    m.Logged({false, 1, "flavor_rus.1"}, 3110);
    due = m.Due(3110, true);
    assert(due.size() == 1 && Contains(due[0], "\"option_index\":1,"));
    // A later event waits behind an earlier one, so `closed` keeps its order.
    m.Closed(Closed{{plague, std::nullopt}}, 4000);
    m.Closed(Closed{{fish, std::nullopt}}, 4010);
    m.Logged({false, 0, "flavor_rus.1"}, 4020);
    assert(m.Due(4020, true).empty());
    m.Logged({false, 0, "flavor_x.2"}, 4030);
    due = m.Due(4030, true);
    assert(due.size() == 2 && Contains(due[0], "\"title\":\"Plague\",\"chosen\":{") && Contains(due[1], "\"title\":\"Tariffs on Fish\",\"chosen\":{"));

    // Two events with one id. Closed in different reads, each takes the line
    // written before it; closed in one read, they are named only when their lines agree.
    KnownEvent first = fish, second = fish;
    second.controller = 0xC3;
    m.Logged({false, 0, "flavor_rus.1"}, 5000);
    m.Closed(Closed{{first, std::nullopt}}, 5000);
    m.Logged({false, 1, "flavor_rus.1"}, 5100);
    m.Closed(Closed{{second, std::nullopt}}, 5100);
    due = m.Due(5100, true);
    assert(due.size() == 2 && Contains(due[0], "\"option_index\":0,") && Contains(due[1], "\"option_index\":1,"));
    m.Logged({true, 0, "flavor_rus.1"}, 6000);
    m.Logged({true, 0, "flavor_rus.1"}, 6000);
    m.Closed(Closed{{first, std::nullopt}, {second, std::nullopt}}, 6000);
    due = m.Due(6000, true);
    assert(due.size() == 2 && Contains(due[0], "\"by\":\"auto\"") && Contains(due[1], "\"by\":\"auto\""));
    m.Logged({false, 0, "flavor_rus.1"}, 7000);
    m.Logged({false, 1, "flavor_rus.1"}, 7000);
    m.Closed(Closed{{first, std::nullopt}, {second, std::nullopt}}, 7000);
    due = m.Due(7000, true);
    assert(due.size() == 2 && Contains(due[0], "\"chosen\":null") && Contains(due[1], "\"chosen\":null"));

    // A line no closed event takes (an event Tongs did not know) is dropped after kKeepLineMs.
    m.Logged({false, 0, "flavor_x.2"}, 8000);
    m.Due(8000 + ChoiceMatcher::kKeepLineMs, true);
    m.Closed(Closed{{plague, std::nullopt}}, 8000 + ChoiceMatcher::kKeepLineMs);
    due = m.Due(8000 + ChoiceMatcher::kKeepLineMs, false);
    assert(due.size() == 1 && Contains(due[0], "\"chosen\":null"));
    m.Logged({false, 0, "flavor_x.2"}, 20000);
    m.Closed(Closed{{plague, std::nullopt}}, 20000);
    m.Reset();
    assert(!m.waiting() && m.Due(20000, false).empty());
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
    // game.log, which names the choices Tongs does not make.
    std::string game_log = "[main.cpp:527]: \n";
    hs.locate_game_log = [] { return ErrorLogLocation{"C:\\user\\logs\\game.log", "physfs_write_dir", ""}; };
    hs.read_log_chunk = [&game_log](const std::string& path, std::uint64_t offset, std::size_t max, LineTail::Chunk& chunk) {
        assert(path == "C:\\user\\logs\\game.log");
        chunk = {game_log.size(), 1, offset < game_log.size() ? game_log.substr(offset, max) : std::string(), false};
        return std::string();
    };
    request.kinds = Bit(NotificationKind::Event);
    const auto events = event_hub.Subscribe(request);
    NotificationSources event_sources(hs, event_hub);
    FakeMainThread main(screen.dispatcher);
    game_log += "[eventwindow.cpp:486]: Selected event option 0 for event flavor_x.2\n";  // before the baseline
    event_sources.PollEvents();  // the two open events and the end of game.log are the baseline
    event_sources.PollEvents();
    assert(events->Take(0).empty());
    assert(Contains(HandleRequest("event choose 1 event=title^=tariffs", hs, 3), "\"ok\":true"));
    game_log += "[eventwindow.cpp:486]: Selected event option 0 for event flavor_rus.1\n";
    event_sources.PollEvents();
    lines = Lines(events->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"change\":\"closed\",\"id\":\"flavor_rus.1\"") &&
           Contains(lines[0], "\"chosen\":{\"number\":1,\"text\":\"Accept the tariffs\",\"option_index\":0,\"option_key\":\"flavor_rus.1.a\",\"by\":\"tongs\",\"client\":3,\"choice_date\":\"1719.3.17\"}"));
    // An event opens (the count goes up) as the game chooses Plague at its deadline.
    screen.raw = EventRaw(screen.tree);
    screen.raw.events.erase(screen.raw.events.begin() + 1);  // without Plague, which is still known: it closed
    game_log += "[eventwindow.cpp:494]: Auto-selected event option 0 for event flavor_x.2\n";
    event_sources.PollEvents();
    lines = Lines(events->Take(0));
    assert(lines.size() == 2 &&
           Contains(lines[0], "\"change\":\"closed\",\"id\":\"flavor_x.2\",\"title\":\"Plague\",\"chosen\":{\"number\":1,\"text\":\"Pray\",\"option_index\":0,\"option_key\":\"flavor_x.2.a\",\"by\":\"auto\"}") &&
           Contains(lines[1], "\"change\":\"opened\",\"id\":\"flavor_rus.1\""));
    // The player clicks the second option; the line is read after the event is seen closed.
    screen.raw.events[1].waiting = false;
    screen.raw.listed_windows.pop_back();
    event_sources.PollEvents();
    assert(events->Take(0).empty());
    game_log += "[eventwindow.cpp:486]: Selected event option 1 for event flavor_rus.1\n";
    event_sources.PollEvents();
    lines = Lines(events->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"title\":\"Tariffs on Fish\",\"chosen\":{\"number\":2,\"text\":\"Refuse them\",\"option_index\":1,\"option_key\":\"flavor_rus.1.b\",\"by\":\"player\"}"));
    screen.raw = EventRaw(screen.tree);
    screen.raw.events.erase(screen.raw.events.begin() + 1);
    event_sources.PollEvents();
    lines = Lines(events->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"change\":\"opened\",\"id\":\"flavor_rus.1\""));
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

    // Kinds the plugin cannot serve are named; error_log and event say which log they read.
    h.locate_error_log = [] { return ErrorLogLocation{"C:\\docs\\logs\\error.log", "physfs_write_dir", ""}; };
    h.locate_game_log = [] { return ErrorLogLocation{"", "", "not yet"}; };
    out = Subscribe("subscribe event,error_log,activity", h, sub);
    assert(Contains(out, "\"kinds\":[\"event\",\"error_log\",\"activity\"],\"queue\":1000,\"unavailable\":[\"event\",\"error_log\"],"
                         "\"error_log\":{\"path\":\"C:\\\\docs\\\\logs\\\\error.log\",\"source\":\"physfs_write_dir\"},"
                         "\"game_log\":{\"error\":\"not yet\"}"));
    std::shared_ptr<Subscription> log_only;
    assert(!Contains(Subscribe("subscribe error_log", h, log_only), "game_log"));
    hub.Unsubscribe(log_only);
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
    // A client call: its own `what`, the running count of client calls, and client_calls_total.
    game.state.load = 9;
    const std::uint64_t total = CountClientCall(h, "province goto", game.state);
    assert(total == 1);
    lines = Lines(activity->Take(0));
    assert(lines.size() == 1 && Contains(lines[0], "\"what\":\"client_call\",\"command\":\"province goto\",\"client_calls\":"));
    assert(Contains(HandleRequest("state", h), "\"client_calls_total\":1"));
    game.state.load = 10;  // another session: the total starts again
    assert(Contains(HandleRequest("state", h), "\"client_calls_total\":0"));
    hub.Unsubscribe(activity);
}

// Fake GUI definitions in this process's memory. Maps are built the way the game's
// insert (0x14ED760) builds them: ternary search trees of lower-cased characters.
struct FakeTypeNode {
    std::uintptr_t value = 0;
    std::uint64_t character = 0;
    std::uintptr_t lower = 0;
    std::uintptr_t higher = 0;
    std::uintptr_t next = 0;
};
static_assert(sizeof(FakeTypeNode) == 0x28);

struct FakeTypeMap {
    std::uintptr_t vtable = 0;
    std::uintptr_t root = 0;  // +0x08
};

// A definition: its class's vtable at +0, pointer fields at +0x08..+0x18 for scroll
// bars and windows (a window's background), a nested map at +0x40 for window classes.
struct FakeDefinition {
    std::uintptr_t vtable = 0;
    std::uintptr_t other[7] = {};
    FakeTypeMap map;
};
static_assert(offsetof(FakeDefinition, map) == 0x40);

struct FakeGui {
    static constexpr std::uintptr_t kMapVtable = 0x7000;
    static constexpr std::uintptr_t kWindowVtable = 0x7100;
    static constexpr std::uintptr_t kIconVtable = 0x7200;
    static constexpr std::uintptr_t kScrollVtable = 0x7500;
    static constexpr std::uintptr_t kButtonVtable = 0x7600;

    FakeTypeMap registry{kMapVtable, 0};
    std::deque<FakeTypeNode> nodes;
    std::deque<FakeDefinition> definitions;
    std::vector<std::uintptr_t> reads;
    int refused = 0;

    TypeTreeLayout Layout() const {
        TypeTreeLayout l{0x08, 0x28, 0x00, 0x10, 0x18, 0x20, kMapVtable, {}};
        l.nested.push_back({kWindowVtable, 0x40});
        l.pointers.push_back({kScrollVtable, 0x08, kButtonVtable});
        l.pointers.push_back({kScrollVtable, 0x10, kButtonVtable});
        l.pointers.push_back({kWindowVtable, 0x18, kButtonVtable});
        return l;
    }

    std::uintptr_t Define(std::uintptr_t vtable) {
        FakeDefinition& d = definitions.emplace_back();
        d.vtable = vtable;
        d.map.vtable = kMapVtable;
        return reinterpret_cast<std::uintptr_t>(&d);
    }

    static FakeTypeMap& MapOf(std::uintptr_t definition) { return reinterpret_cast<FakeDefinition*>(definition)->map; }
    // The pointer field at +0x08 * (1 + i).
    static std::uintptr_t& FieldOf(std::uintptr_t definition, int i) {
        return reinterpret_cast<FakeDefinition*>(definition)->other[i];
    }

    void Insert(FakeTypeMap& map, const char* key, std::uintptr_t value) {
        std::uintptr_t* link = &map.root;
        for (;;) {
            const auto c = static_cast<std::uint64_t>(std::tolower(static_cast<unsigned char>(*key)));
            if (*link == 0) {
                FakeTypeNode& n = nodes.emplace_back();
                n.character = c;
                *link = reinterpret_cast<std::uintptr_t>(&n);
            }
            auto* node = reinterpret_cast<FakeTypeNode*>(*link);
            if (c == node->character) {
                if (*++key == '\0') {
                    node->value = value;
                    return;
                }
                link = &node->next;
            } else {
                link = c < node->character ? &node->lower : &node->higher;
            }
        }
    }

    template <class T>
    static bool Inside(std::uintptr_t address, std::size_t size, const T& object) {
        const auto at = reinterpret_cast<std::uintptr_t>(&object);
        return address >= at && address + size <= at + sizeof object;
    }

    // Only the registry, the nodes and the definitions are memory; everything else is refused.
    ReadMemory Reader() {
        return [this](std::uintptr_t address, void* out, std::size_t size) {
            reads.push_back(address);
            bool ok = Inside(address, size, registry);
            for (const FakeTypeNode& n : nodes) ok = ok || Inside(address, size, n);
            for (const FakeDefinition& d : definitions) ok = ok || Inside(address, size, d);
            if (!ok) {
                ++refused;
                return false;
            }
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            return true;
        };
    }

    std::string Collect(std::unordered_set<std::uintptr_t>& live) {
        live.clear();
        return CollectLiveDefinitions(reinterpret_cast<std::uintptr_t>(&registry), Layout(), Reader(), live);
    }
};

static void TestLiveDefinitions() {
    FakeGui gui;
    // topbar.gui: a window with an icon, a text and a nested window with its own icon;
    // alerts_parent, a window with nothing inside; and "TOP", a prefix of topbar.
    const std::uintptr_t topbar = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t icon = gui.Define(FakeGui::kIconVtable);
    const std::uintptr_t text = gui.Define(FakeGui::kIconVtable);
    const std::uintptr_t inner = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t deep = gui.Define(FakeGui::kIconVtable);
    const std::uintptr_t alerts = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t top = gui.Define(FakeGui::kIconVtable);
    gui.Insert(FakeGui::MapOf(topbar), "icon", icon);
    gui.Insert(FakeGui::MapOf(topbar), "text", text);
    gui.Insert(FakeGui::MapOf(topbar), "inner", inner);
    gui.Insert(FakeGui::MapOf(inner), "deep", deep);
    gui.Insert(gui.registry, "topbar", topbar);
    gui.Insert(gui.registry, "alerts_parent", alerts);
    gui.Insert(gui.registry, "TOP", top);
    // A class Tongs does not list, and a listed one whose map is not where it should be:
    // neither map is walked, though both hold a value.
    const std::uintptr_t other = gui.Define(0x7300);
    const std::uintptr_t odd = gui.Define(FakeGui::kWindowVtable);
    FakeGui::MapOf(odd).vtable = 0x7400;
    gui.Insert(FakeGui::MapOf(other), "hidden", 0xDEF00010);
    gui.Insert(FakeGui::MapOf(odd), "hidden", 0xDEF00020);
    gui.Insert(gui.registry, "other", other);
    gui.Insert(gui.registry, "odd", odd);

    std::unordered_set<std::uintptr_t> live;
    assert(gui.Collect(live).empty());
    assert((live == std::unordered_set<std::uintptr_t>{topbar, icon, text, inner, deep, alerts, top, other, odd}));
    assert(gui.refused == 0);  // nothing outside the registry, the nodes and live definitions was read

    // A reload replaces the registry's value in place and deletes the old
    // definition with everything nested in it: none of it is live, and none is read.
    const std::uintptr_t topbar2 = gui.Define(FakeGui::kWindowVtable);
    gui.Insert(gui.registry, "topbar", topbar2);
    gui.reads.clear();
    assert(gui.Collect(live).empty());
    assert(live.contains(topbar2) && !live.contains(topbar) && !live.contains(icon) && !live.contains(deep) &&
           live.contains(top) && live.size() == 5);
    for (const std::uintptr_t a : gui.reads) assert(!FakeGui::Inside(a, 1, *reinterpret_cast<FakeDefinition*>(topbar)));

    // An empty registry holds nothing; that is not an error.
    FakeGui empty;
    assert(empty.Collect(live).empty() && live.empty());

    // Unreadable memory is an error, not a smaller set.
    const ReadMemory none = [](std::uintptr_t, void*, std::size_t) { return false; };
    assert(Contains(CollectLiveDefinitions(0x1000, gui.Layout(), none, live), "unreadable"));
    gui.Insert(gui.registry, "gone", 0xDEF00030);  // a registry value that is not readable
    gui.refused = 0;
    assert(Contains(gui.Collect(live), "definition is unreadable") && gui.refused == 1);
    gui.Insert(gui.registry, "gone", alerts);
    FakeTypeNode& last = gui.nodes.back();
    last.lower = 0xBAD00000;
    assert(Contains(gui.Collect(live), "registry is unreadable"));
    // A loop (never seen in the game) ends at the node limit.
    last.lower = gui.registry.root;
    assert(Contains(gui.Collect(live), "too many nodes"));
    last.lower = 0;
    assert(gui.Collect(live).empty());
    // A layout whose fields do not fit a node is refused before anything is read.
    TypeTreeLayout bad = gui.Layout();
    bad.next = 0x28;
    gui.reads.clear();
    assert(Contains(CollectLiveDefinitions(reinterpret_cast<std::uintptr_t>(&gui.registry), bad, gui.Reader(), live),
                    "layout") &&
           gui.reads.empty());
}

static void TestLivePointerDefinitions() {
    FakeGui gui;
    // musicplayer.gui: a window holding an extended scroll bar (volume_slider)
    // whose buttons are in pointer fields, a top-level scroll bar
    // (right_vertical_slider), and pops_login.gui: a window whose background
    // button is in a pointer field. None of these buttons is in a map.
    const std::uintptr_t mini = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t volume = gui.Define(FakeGui::kScrollVtable);
    const std::uintptr_t decrease = gui.Define(FakeGui::kButtonVtable);
    const std::uintptr_t increase = gui.Define(FakeGui::kButtonVtable);
    const std::uintptr_t right = gui.Define(FakeGui::kScrollVtable);
    const std::uintptr_t track = gui.Define(FakeGui::kButtonVtable);
    const std::uintptr_t blocker = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t hitbox = gui.Define(FakeGui::kButtonVtable);
    gui.Insert(FakeGui::MapOf(mini), "volume_slider", volume);
    FakeGui::FieldOf(volume, 0) = decrease;
    FakeGui::FieldOf(volume, 1) = increase;
    FakeGui::FieldOf(right, 1) = track;  // field 0 stays empty
    FakeGui::FieldOf(blocker, 2) = hitbox;
    gui.Insert(gui.registry, "musicminicontroller_window", mini);
    gui.Insert(gui.registry, "right_vertical_slider", right);
    gui.Insert(gui.registry, "pops_login_blocker", blocker);
    // Fields Tongs does not follow: a class not listed, a field not listed, and
    // a target of another class (the target is read for its class, nothing more).
    const std::uintptr_t other = gui.Define(0x7300);
    FakeGui::FieldOf(other, 0) = 0xDEF00040;
    FakeGui::FieldOf(volume, 3) = 0xDEF00050;
    const std::uintptr_t icon = gui.Define(FakeGui::kIconVtable);
    FakeGui::FieldOf(icon, 0) = 0xDEF00060;
    FakeGui::FieldOf(icon, 2) = 0xDEF00070;
    const std::uintptr_t odd = gui.Define(FakeGui::kScrollVtable);
    FakeGui::FieldOf(odd, 0) = icon;
    gui.Insert(gui.registry, "other", other);
    gui.Insert(gui.registry, "odd", odd);

    std::unordered_set<std::uintptr_t> live;
    assert(gui.Collect(live).empty());
    assert((live == std::unordered_set<std::uintptr_t>{mini, volume, decrease, increase, right, track, blocker, hitbox,
                                                       other, odd}));
    assert(gui.refused == 0);

    // A reload deletes the old scroll bar with the buttons it owns: none of them is live or read.
    const std::uintptr_t right2 = gui.Define(FakeGui::kScrollVtable);
    gui.Insert(gui.registry, "right_vertical_slider", right2);
    gui.reads.clear();
    assert(gui.Collect(live).empty());
    assert(live.contains(right2) && !live.contains(right) && !live.contains(track) && live.contains(hitbox));
    for (const std::uintptr_t a : gui.reads) {
        assert(!FakeGui::Inside(a, 1, *reinterpret_cast<FakeDefinition*>(right)) &&
               !FakeGui::Inside(a, 1, *reinterpret_cast<FakeDefinition*>(track)));
    }

    // A target that is also in a map counts once; a loop of pointer fields
    // (never seen in the game) ends.
    gui.Insert(FakeGui::MapOf(mini), "hitbox", hitbox);
    assert(gui.Collect(live).empty() && live.size() == 9);
    const std::uintptr_t first = gui.Define(FakeGui::kButtonVtable);
    const std::uintptr_t second = gui.Define(FakeGui::kButtonVtable);
    FakeGui::FieldOf(right2, 0) = first;
    FakeGui::FieldOf(first, 0) = second;
    FakeGui::FieldOf(second, 0) = first;
    TypeTreeLayout looping = gui.Layout();
    looping.pointers.push_back({FakeGui::kButtonVtable, 0x08, FakeGui::kButtonVtable});
    live.clear();
    const std::uintptr_t registry = reinterpret_cast<std::uintptr_t>(&gui.registry);
    assert(CollectLiveDefinitions(registry, looping, gui.Reader(), live).empty());
    assert(live.contains(first) && live.contains(second) && live.size() == 11);

    // A field or target of a live holder that is unreadable is an error, not a smaller set.
    FakeGui::FieldOf(right2, 1) = 0xDEF00080;
    gui.refused = 0;
    assert(Contains(gui.Collect(live), "definition is unreadable") && gui.refused == 1);
}

static void TestWidgetNamePlan() {
    using Source = UiNamePlan::Source;
    const auto plan = [](bool live, bool held, bool own) {
        const UiNamePlan p = PlanWidgetName(live, held, own);
        return std::pair{p.source, p.stale};
    };
    // A live definition gives the name, as always.
    assert(plan(true, true, true) == std::pair(Source::Definition, false));
    assert(plan(true, false, false) == std::pair(Source::Definition, false));
    // Without one the widget is stale: its holder's list names it, else its own name, else nothing.
    assert(plan(false, true, true) == std::pair(Source::Holder, true));
    assert(plan(false, false, true) == std::pair(Source::Override, true));
    assert(plan(false, false, false) == std::pair(Source::None, true));
}

// SampleTree after a reload of topbar.gui: a leaked hotkey_assign_parent with no name
// (its definition was freed), its icon and text, and a second, live one.
static UiTree StaleTree() {
    UiTree t = SampleTree();
    for (int i = 0; i < static_cast<int>(t.widgets.size()); ++i) t.widgets[i].address = Address(i);
    const int old = AddWidget(t, "", UiType::Window, true, {0, 0, 300, 40});
    t.widgets[old].stale = true;
    AddWidget(t, "icon", UiType::Icon, true, {0, 0, 40, 40}, old);
    const int text = AddWidget(t, "text", UiType::Text, true, {40, 0, 260, 40}, old);
    t.widgets[text].has_text = true;
    t.widgets[text].text = "F1";
    const int live = AddWidget(t, "hotkey_assign_parent", UiType::Window, true, {0, 40, 300, 40});
    AddWidget(t, "icon", UiType::Icon, true, {0, 40, 40, 40}, live);
    PropagateStale(t);
    return t;
}

static void TestStaleWidgets() {
    const UiTree t = StaleTree();
    const int old = 18;
    assert(t.widgets[old].stale && t.widgets[old + 1].stale && t.widgets[old + 2].stale && !t.widgets[old + 3].stale &&
           !t.widgets[0].stale);

    // find leaves stale widgets out unless asked; their paths are positional.
    assert(Paths(t, "//icon") == std::vector<std::string>{"hotkey_assign_parent/icon"});
    UiRequest r;
    bool found = false;
    assert(ParseUiRequest("find //text stale", r).empty() && r.stale);
    std::string out = UiAnswer(t, r, found);
    assert(Contains(out, "\"count\":1,") && Contains(out, "\"path\":\"[3]/text\"") && Contains(out, "\"stale\":true"));
    assert(ParseUiRequest("find //text", r).empty() && !r.stale);
    assert(Contains(UiAnswer(t, r, found), "\"count\":0,"));
    assert(ParseUiRequest("find //icon* all stale limit=5", r).empty() && r.all && r.stale && r.limit == 5);
    assert(Contains(UiAnswer(t, r, found), "\"count\":4,"));
    assert(Contains(ParseUiRequest("tree stale", r), "\"error\":\"bad_command\""));
    assert(Contains(ParseUiRequest("click //text stale", r), "\"error\":\"bad_command\""));

    // tree shows them, marked; live widgets carry no stale member.
    assert(ParseUiRequest("tree", r).empty());
    out = UiAnswer(t, r, found);
    assert(Contains(out, "{\"name\":\"\",\"path\":\"[3]\",\"type\":\"window\"") &&
           Contains(out, "{\"name\":\"text\",\"path\":\"[3]/text\"") && Contains(out, "\"text\":\"F1\",\"stale\":true}"));
    assert(Contains(out, "{\"name\":\"hotkey_assign_parent\",\"path\":\"hotkey_assign_parent\",\"type\":\"window\","
                         "\"visible\":true,\"rect\":{\"x\":0,\"y\":40,\"w\":300,\"h\":40},"
                         "\"click_point\":{\"x\":150,\"y\":60},\"children\""));

    // click never takes a stale widget: hovering one may read its freed definition.
    UiSelector sel;
    assert(ParseUiSelector("//text", sel).empty());
    UiClickTarget c = ResolveClickTarget(t, sel);
    assert(c.error == "stale" && c.count == 1 && c.paths == std::vector<std::string>{"[5]/text"} && c.widget == -1);
    assert(ParseUiSelector("//icon", sel).empty());
    c = ResolveClickTarget(t, sel);  // a live match wins over a stale one
    assert(c.error.empty() && c.path == "hotkey_assign_parent/icon");
    assert(ParseUiSelector("//nothing", sel).empty());
    assert(ResolveClickTarget(t, sel).error == "not_found");
    // A hidden live match still says not_visible.
    assert(ParseUiSelector("timeline_controls", sel).empty());
    assert(ResolveClickTarget(t, sel).error == "not_visible");
    // The hovered widget is named even when stale.
    const UiCoverCheck cover = CheckCover(t, Address(19), Address(20));
    assert(cover.covered && cover.covered_by == "[3]/text");
}

// What one tongs.ui visit gave, copied: the widget's strings live only for the visit.
struct SeenWidget {
    TongsUiWidget raw{};
    std::string name;
    bool has_path = false;
    std::string path;
    bool has_text = false;
    std::string text;
    bool has_file = false;
    std::string file;
};

struct SeenVisits {
    std::vector<SeenWidget> widgets;
    int stop_after = -1;  // return 0 from this visit on

    static int Visit(void* context, const TongsUiWidget* widget) {
        auto& self = *static_cast<SeenVisits*>(context);
        SeenWidget w;
        w.raw = *widget;
        w.name = widget->name;
        w.has_path = widget->path != nullptr;
        if (w.has_path) w.path = widget->path;
        w.has_text = widget->text != nullptr;
        if (w.has_text) w.text = widget->text;
        w.has_file = widget->definition_file != nullptr;
        if (w.has_file) w.file = widget->definition_file;
        assert(widget->size == sizeof(TongsUiWidget));
        self.widgets.push_back(std::move(w));
        return self.stop_after < 0 || static_cast<int>(self.widgets.size()) < self.stop_after;
    }
    std::vector<std::uint64_t> Ids() const {
        std::vector<std::uint64_t> out;
        for (const auto& w : widgets) out.push_back(w.raw.id);
        return out;
    }
    std::vector<int> Parents() const {
        std::vector<int> out;
        for (const auto& w : widgets) out.push_back(w.raw.parent);
        return out;
    }
};

static UiTree AddressedSampleTree() {
    UiTree t = SampleTree();
    for (int i = 0; i < static_cast<int>(t.widgets.size()); ++i) t.widgets[i].address = Address(i);
    return t;
}

static void TestUiServiceFind() {
    const UiTree t = AddressedSampleTree();
    UiSelector sel;
    // The matches of the pipe's `ui find`, in its order, each after its ancestors.
    assert(ParseUiSelector("EventWindow//event_option_button", sel).empty());
    SeenVisits seen;
    std::uint32_t count = VisitUiFind(t, sel, 0, 0, &SeenVisits::Visit, &seen);
    assert(count == 3);
    assert((seen.Ids() == std::vector<std::uint64_t>{Address(8), Address(9), Address(10), Address(11), Address(12),
                                                      Address(13), Address(14), Address(15)}));
    assert((seen.Parents() == std::vector<int>{-1, 0, 1, 2, 1, 4, 1, 6}));
    std::vector<std::string> matched;
    for (const auto& w : seen.widgets) {
        assert(w.has_path);
        if (w.raw.flags & TONGS_UI_MATCH) matched.push_back(w.path);
    }
    assert(matched == Paths(t, "EventWindow//event_option_button"));
    assert(seen.widgets[1].path == "EventWindow/event_option_listbox" && seen.widgets[1].raw.type == TONGS_UI_LISTBOX);
    // Rectangles, text and flags as the pipe's answer has them.
    const SeenWidget& refuse = seen.widgets[5];
    assert(refuse.raw.x == 1376 && refuse.raw.y == 960 && refuse.raw.width == 500 && refuse.raw.height == 56);
    assert(refuse.has_text && refuse.text == "Refuse them" && refuse.name == "event_option_button");
    assert(refuse.raw.flags == (TONGS_UI_MATCH | TONGS_UI_VISIBLE | TONGS_UI_ENABLED) &&
           refuse.raw.type == TONGS_UI_BUTTON);
    assert(!(seen.widgets[7].raw.flags & TONGS_UI_ENABLED));  // the disabled third option
    assert(seen.widgets[0].raw.flags == (TONGS_UI_VISIBLE | TONGS_UI_ENABLED) && !seen.widgets[0].has_text);
    UiRequest r;
    bool found = false;
    assert(ParseUiRequest("find EventWindow//event_option_button", r).empty());
    assert(Contains(UiAnswer(t, r, found), "\"path\":\"EventWindow/event_option_listbox/event_option_entry[2]/"
                                           "event_option_button\",\"type\":\"button\",\"visible\":true,"
                                           "\"rect\":{\"x\":1376,\"y\":960,\"w\":500,\"h\":56}"));

    // A match inside a match is visited once, as a match; the hovered widget is marked.
    assert(ParseUiSelector("//[type=window]", sel).empty());
    seen = {};
    count = VisitUiFind(t, sel, 0, Address(12), &SeenVisits::Visit, &seen);
    assert(count == 5 && seen.widgets.size() == 6);  // speed_controls, EventWindow, the list box and its three entries
    assert(!(seen.widgets[2].raw.flags & TONGS_UI_MATCH) && seen.widgets[2].raw.id == Address(9));
    assert((seen.widgets[4].raw.flags & TONGS_UI_HOVERED) && seen.widgets[4].raw.id == Address(12));
    for (std::size_t i = 0; i < seen.widgets.size(); ++i) assert(i == 4 || !(seen.widgets[i].raw.flags & TONGS_UI_HOVERED));
    // A visit that returns 0 stops the call; count still has every match.
    seen = {};
    seen.stop_after = 2;
    assert(VisitUiFind(t, sel, 0, 0, &SeenVisits::Visit, &seen) == 5 && seen.widgets.size() == 2);

    // Hidden widgets only with TONGS_UI_HIDDEN, as with `all`; then their paths count them too.
    assert(ParseUiSelector("//timeline_pause", sel).empty());
    seen = {};
    assert(VisitUiFind(t, sel, 0, 0, &SeenVisits::Visit, &seen) == 0 && seen.widgets.empty());
    assert(VisitUiFind(t, sel, TONGS_UI_HIDDEN, 0, &SeenVisits::Visit, &seen) == 1 && seen.widgets.size() == 2);
    assert(seen.widgets[1].path == "timeline_controls/timeline_pause" &&
           seen.widgets[1].raw.flags == (TONGS_UI_MATCH | TONGS_UI_ENABLED));
    assert(ParseUiSelector("EventWindow", sel).empty());
    seen = {};
    assert(VisitUiFind(t, sel, TONGS_UI_HIDDEN, 0, &SeenVisits::Visit, &seen) == 2);
    assert(seen.widgets[0].path == "EventWindow[1]" && seen.widgets[1].path == "EventWindow[2]");
    // A widget without a name: "" and a positional step.
    assert(ParseUiSelector("EventWindow/[2]", sel).empty());
    seen = {};
    assert(VisitUiFind(t, sel, 0, 0, &SeenVisits::Visit, &seen) == 1 && seen.widgets[1].name.empty() &&
           seen.widgets[1].path == "EventWindow/[2]" && seen.widgets[1].raw.type == TONGS_UI_OTHER);

    // Stale widgets only with TONGS_UI_STALE, flagged, with their ancestors.
    const UiTree stale = StaleTree();
    assert(ParseUiSelector("//text", sel).empty());
    seen = {};
    assert(VisitUiFind(stale, sel, 0, 0, &SeenVisits::Visit, &seen) == 0);
    assert(VisitUiFind(stale, sel, TONGS_UI_STALE, 0, &SeenVisits::Visit, &seen) == 1 && seen.widgets.size() == 2);
    assert(seen.widgets[0].path == "[3]" && (seen.widgets[0].raw.flags & TONGS_UI_IS_STALE) &&
           seen.widgets[1].path == "[3]/text" && (seen.widgets[1].raw.flags & TONGS_UI_IS_STALE));
}

static void TestUiServiceHovered() {
    const UiTree t = AddressedSampleTree();
    // The hovered widget, then its ancestors; each parent is the next visit.
    SeenVisits seen;
    VisitUiHovered(t, Address(13), 0, &SeenVisits::Visit, &seen);
    assert((seen.Ids() == std::vector<std::uint64_t>{Address(13), Address(12), Address(9), Address(8)}));
    assert((seen.Parents() == std::vector<int>{1, 2, 3, -1}));
    assert(seen.widgets[0].raw.flags == (TONGS_UI_HOVERED | TONGS_UI_VISIBLE | TONGS_UI_ENABLED));
    for (const auto& w : seen.widgets) assert(!w.has_path);
    seen = {};
    VisitUiHovered(t, Address(13), TONGS_UI_PATHS, &SeenVisits::Visit, &seen);
    assert(seen.widgets.size() == 4 &&
           seen.widgets[0].path == "EventWindow/event_option_listbox/event_option_entry[2]/event_option_button" &&
           seen.widgets[3].path == "EventWindow");
    seen = {};
    seen.stop_after = 1;
    VisitUiHovered(t, Address(13), 0, &SeenVisits::Visit, &seen);
    assert(seen.widgets.size() == 1);
    // Nothing hovered, a widget not in the tree, or a hidden one without TONGS_UI_HIDDEN: no visit.
    seen = {};
    VisitUiHovered(t, 0, 0, &SeenVisits::Visit, &seen);
    VisitUiHovered(t, 0xDEAD0, 0, &SeenVisits::Visit, &seen);
    VisitUiHovered(t, Address(7), 0, &SeenVisits::Visit, &seen);
    assert(seen.widgets.empty());
    VisitUiHovered(t, Address(7), TONGS_UI_HIDDEN, &SeenVisits::Visit, &seen);
    assert(seen.widgets.size() == 2 && seen.widgets[1].raw.id == Address(5));
    // A stale one only with TONGS_UI_STALE.
    const UiTree stale = StaleTree();
    seen = {};
    VisitUiHovered(stale, Address(20), 0, &SeenVisits::Visit, &seen);
    assert(seen.widgets.empty());
    VisitUiHovered(stale, Address(20), TONGS_UI_STALE | TONGS_UI_PATHS, &SeenVisits::Visit, &seen);
    assert(seen.widgets.size() == 2 && seen.widgets[0].path == "[3]/text" &&
           (seen.widgets[0].raw.flags & (TONGS_UI_IS_STALE | TONGS_UI_HOVERED)) == (TONGS_UI_IS_STALE | TONGS_UI_HOVERED));
}

// The service's front: arguments, the main thread, calls from inside a visit, a tree that cannot be read.
static void TestUiServiceCalls() {
    const UiTree sample = AddressedSampleTree();
    std::uint32_t thread = 7, main = 7;
    int reads = 0;
    std::string error;
    std::vector<std::string> logged;
    UiServiceHost h;
    h.current_thread = [&] { return thread; };
    h.main_thread = [&] { return main; };
    h.read_tree = [&](UiTree& tree) {
        ++reads;
        tree = sample;
        return error;
    };
    h.read_hovered = [] { return Address(3); };
    h.log_error = [&](const std::string& message) { logged.push_back(message); };
    UiService service(h);

    SeenVisits seen;
    std::uint32_t count = 99;
    assert(service.Find("speed_controls/button_*", 0, &SeenVisits::Visit, &seen, &count) == ANVIL_OK && count == 2);
    assert(seen.widgets.size() == 3 && (seen.widgets[1].raw.flags & TONGS_UI_HOVERED) && reads == 1);
    assert(service.Find("speed_controls/button_*", TONGS_UI_PATHS, &SeenVisits::Visit, &seen, nullptr) == ANVIL_OK);
    // Bad arguments are refused before anything is read.
    for (const char* bad : {"", "/a", "a[x=1]"}) {
        assert(service.Find(bad, 0, &SeenVisits::Visit, &seen, &count) == ANVIL_ERROR_INVALID_ARGUMENT && count == 0);
    }
    assert(service.Find(nullptr, 0, &SeenVisits::Visit, &seen, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
    assert(service.Find("//x", 0, nullptr, nullptr, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
    assert(service.Find("//x", 0x8, &SeenVisits::Visit, &seen, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
    assert(service.Hovered(0x100, &SeenVisits::Visit, &seen) == ANVIL_ERROR_INVALID_ARGUMENT);
    assert(service.Hovered(0, nullptr, nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
    assert(reads == 2);

    // Off the main thread, or before the first frame: refused, nothing read.
    thread = 8;
    seen = {};
    assert(service.Find("//*", 0, &SeenVisits::Visit, &seen, &count) == ANVIL_ERROR_WRONG_PHASE && count == 0);
    assert(service.Hovered(0, &SeenVisits::Visit, &seen) == ANVIL_ERROR_WRONG_PHASE);
    thread = main = 0;
    assert(service.Find("//*", 0, &SeenVisits::Visit, &seen, &count) == ANVIL_ERROR_WRONG_PHASE);
    assert(reads == 2 && seen.widgets.empty());
    thread = main = 7;

    // A call from inside a visit is refused; the outer call goes on.
    struct Nested {
        UiService* service;
        AnvilResult inner = ANVIL_OK;
        int visits = 0;
        static int Visit(void* context, const TongsUiWidget*) {
            auto& self = *static_cast<Nested*>(context);
            ++self.visits;
            SeenVisits ignored;
            self.inner = self.service->Hovered(0, &SeenVisits::Visit, &ignored);
            return 1;
        }
    } nested{&service};
    assert(service.Find("speed_controls", 0, &Nested::Visit, &nested, &count) == ANVIL_OK && count == 1);
    assert(nested.visits == 1 && nested.inner == ANVIL_ERROR_WRONG_PHASE);
    seen = {};
    assert(service.Hovered(0, &SeenVisits::Visit, &seen) == ANVIL_OK && seen.widgets.size() == 2);  // usable again

    // A tree that cannot be read: an error, logged once until it changes or goes away.
    error = "no GUI manager";
    seen = {};
    assert(service.Find("//*", 0, &SeenVisits::Visit, &seen, &count) == ANVIL_ERROR_INTERNAL && count == 0);
    assert(service.Hovered(0, &SeenVisits::Visit, &seen) == ANVIL_ERROR_INTERNAL && seen.widgets.empty());
    assert(logged == std::vector<std::string>{"tongs.ui: no GUI manager"});
    error.clear();
    assert(service.Find("//*", 0, &SeenVisits::Visit, &seen, &count) == ANVIL_OK && count > 0);
    error = "no GUI manager";
    assert(service.Find("//*", 0, &SeenVisits::Visit, &seen, &count) == ANVIL_ERROR_INTERNAL && logged.size() == 2);
}

// A GUI in the test's own memory for UiStructure: widgets with render nodes,
// the child containers of windows, containers, list boxes and scroll bars, and
// the registry. Vtables are small numbers (image base 0), so they are the RVAs.
struct FakeWidgets {
    static constexpr unsigned kWindow = 0x100, kContainer = 0x110, kListbox = 0x120, kScrollbar = 0x130,
                              kButton = 0x140, kUnknown = 0x150;
    static constexpr std::size_t kBlock = 0x800, kPart = 0x40;  // a widget pointer is kPart into its block (window_part)

    std::deque<std::vector<unsigned char>> blocks;
    std::vector<std::uintptr_t> registry;
    std::vector<std::uintptr_t> manager = std::vector<std::uintptr_t>(4, 0);
    int reads = 0;
    UiStructureLayout layout;

    FakeWidgets() {
        UiStructureLayout& l = layout;
        l.registry_begin = 0x10;
        l.registry_end = 0x18;
        l.dead = 0xC4;
        l.window_part = kPart;
        l.window_pairs = 0x300;
        l.pair_names = 0x18;
        l.name_size = 0x20;
        l.window_pair_count = 1;
        l.window_maps = 0x500;  // empty lists (full object)
        l.window_map_count = 1;
        l.map_stride = 0x40;
        l.map_list_head = 0x20;
        l.list_node_next = 0x10;
        l.window_subwindows = 0x600;  // an empty vector (full object)
        l.subwindow_size = 0x28;
        l.subwindow_pointer = 0x20;
        l.container_pairs = 0x300;
        l.container_pair_count = 1;
        l.pair_stride = 0x30;
        l.list_items = 0x400;
        l.listbox_item_window = 0x40;
        l.listbox_scrollbar = 0x408;
        l.scrollbar_parts[0] = 0x300;
        l.scrollbar_parts[1] = 0x308;
        l.scrollbar_parts[2] = 0x310;
        l.scrollbar_parts[3] = 0x318;
        l.node_owner = 0x118;
        l.node_parent = 0x110;
        using C = UiKind::Children;
        l.kinds = {{kWindow, UiType::Window, 0x200, 0, false, C::Window},
                   {kContainer, UiType::Window, 0x200, 0, false, C::Container},
                   {kListbox, UiType::Listbox, 0x200, 0, false, C::Listbox},
                   {kScrollbar, UiType::Other, 0, 0, false, C::Scrollbar},
                   {kButton, UiType::Button, 0x200}};
    }

    std::uintptr_t Block() {
        blocks.emplace_back(kBlock);
        return reinterpret_cast<std::uintptr_t>(blocks.back().data());
    }
    template <typename T> static void Put(std::uintptr_t at, T value) { std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value); }

    // A widget of class `vtable` with a render node under `parent_node` (owned by
    // `owner_of_node`: the widget itself, or 0 as a list box's may be), in the registry.
    std::uintptr_t Widget(unsigned vtable, std::uintptr_t parent_node, bool node = true, bool owned_node = true) {
        const std::uintptr_t w = Block() + kPart;
        Put<std::uintptr_t>(w, vtable);
        if (node) {
            const std::uintptr_t n = Block();
            Put<std::uintptr_t>(n + layout.node_parent, parent_node);
            Put<std::uintptr_t>(n + layout.node_owner, owned_node ? w : 0);
            Put<std::uintptr_t>(w + 0x200, n);
        }
        registry.push_back(w);
        return w;
    }
    static std::uintptr_t NodeOf(std::uintptr_t widget) { return *reinterpret_cast<std::uintptr_t*>(widget + 0x200); }

    // A vector pair at `at`: the children and their names (std::string, short, in place).
    void Pair(std::uintptr_t at, const std::vector<std::uintptr_t>& children, const std::vector<std::string>& names) {
        const std::uintptr_t pointers = Block(), strings = Block();
        for (std::size_t i = 0; i < children.size(); ++i) {
            Put(pointers + i * 8, children[i]);
            const std::uintptr_t s = strings + i * layout.name_size;
            std::memcpy(reinterpret_cast<void*>(s), names[i].c_str(), names[i].size() + 1);
            Put<std::uint64_t>(s + 0x10, names[i].size());
            Put<std::uint64_t>(s + 0x18, 15);
        }
        Put(at, pointers);
        Put(at + 8, pointers + children.size() * 8);
        Put(at + layout.pair_names, strings);
        Put(at + layout.pair_names + 8, strings + names.size() * layout.name_size);
    }
    // A list box's items: an insertion list whose items hold the full objects of their windows.
    void Items(std::uintptr_t listbox, const std::vector<std::uintptr_t>& windows) {
        std::uintptr_t previous = 0;
        for (const std::uintptr_t window : windows) {
            const std::uintptr_t node = Block(), item = Block();
            Put(node, item);
            Put(item + layout.listbox_item_window, window - kPart);
            Put(previous == 0 ? listbox + layout.list_items : previous + layout.list_node_next, node);
            previous = node;
        }
    }

    ReadMemory Reader() {
        return [this](std::uintptr_t address, void* out, std::size_t size) {
            ++reads;
            const auto inside = [&](const unsigned char* begin, std::size_t length) {
                const auto b = reinterpret_cast<std::uintptr_t>(begin);
                return address >= b && address + size <= b + length;
            };
            bool ok = inside(reinterpret_cast<const unsigned char*>(manager.data()), manager.size() * 8) ||
                      (!registry.empty() &&
                       inside(reinterpret_cast<const unsigned char*>(registry.data()), registry.size() * 8));
            for (const auto& b : blocks) ok = ok || inside(b.data(), b.size());
            if (!ok) return false;
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            return true;
        };
    }
    std::uintptr_t Manager() {
        manager[2] = reinterpret_cast<std::uintptr_t>(registry.data());
        manager[3] = manager[2] + registry.size() * 8;
        return reinterpret_cast<std::uintptr_t>(manager.data());
    }
};

// The widget's ancestors in a whole-registry placement, from the widget up.
static std::vector<std::uintptr_t> PlacedChain(const UiStructure& all, std::uintptr_t widget) {
    std::vector<std::uintptr_t> chain;
    int at = -1;
    for (int i = 0; i < static_cast<int>(all.entries().size()); ++i) {
        if (all.entries()[i].pointer == widget) at = i;
    }
    for (; at != -1; at = all.entries()[at].parent) chain.push_back(all.entries()[at].pointer);
    return chain;
}

static void TestUiStructureClimb() {
    FakeWidgets g;
    // A top-level window holding a container, a list box and a button. The list
    // box's node has no owner; its rows are windows its items hold by their
    // full objects, and its scroll bar (no node) holds two buttons. The
    // container holds a button by name and has a button only the game's code
    // holds (its node under the container's). A second top-level window, and a
    // dead button the window still lists.
    const std::uintptr_t top = g.Widget(FakeWidgets::kWindow, 0);
    const std::uintptr_t container = g.Widget(FakeWidgets::kContainer, FakeWidgets::NodeOf(top));
    const std::uintptr_t list = g.Widget(FakeWidgets::kListbox, FakeWidgets::NodeOf(top), true, false);
    const std::uintptr_t button = g.Widget(FakeWidgets::kButton, FakeWidgets::NodeOf(top));
    const std::uintptr_t named = g.Widget(FakeWidgets::kButton, FakeWidgets::NodeOf(container));
    const std::uintptr_t coded = g.Widget(FakeWidgets::kButton, FakeWidgets::NodeOf(container));
    const std::uintptr_t row1 = g.Widget(FakeWidgets::kWindow, FakeWidgets::NodeOf(list));
    const std::uintptr_t row2 = g.Widget(FakeWidgets::kWindow, FakeWidgets::NodeOf(list));
    const std::uintptr_t row_button = g.Widget(FakeWidgets::kButton, FakeWidgets::NodeOf(row2));
    const std::uintptr_t bar = g.Widget(FakeWidgets::kScrollbar, 0, false);
    const std::uintptr_t slider = g.Widget(FakeWidgets::kButton, FakeWidgets::NodeOf(list));
    const std::uintptr_t track = g.Widget(FakeWidgets::kButton, FakeWidgets::NodeOf(list));
    const std::uintptr_t other = g.Widget(FakeWidgets::kWindow, 0);
    const std::uintptr_t unknown = g.Widget(FakeWidgets::kUnknown, FakeWidgets::NodeOf(other));
    const std::uintptr_t dead = g.Widget(FakeWidgets::kButton, FakeWidgets::NodeOf(top));
    FakeWidgets::Put<std::uint8_t>(dead + g.layout.dead, 1);
    g.Pair(top + g.layout.window_pairs, {container, list, button, dead}, {"box", "rows", "ok", "gone"});
    g.Pair(container + g.layout.container_pairs, {named}, {"named"});
    g.Pair(row2 + g.layout.window_pairs, {row_button}, {"pick"});
    g.Pair(other + g.layout.window_pairs, {unknown}, {"minimap"});
    g.Items(list, {row1, row2});
    FakeWidgets::Put(list + g.layout.listbox_scrollbar, bar);
    FakeWidgets::Put(bar + g.layout.scrollbar_parts[0], slider);
    FakeWidgets::Put(bar + g.layout.scrollbar_parts[1], track);

    UiStructure all(g.layout, 0, g.Reader());
    const std::uintptr_t manager = g.Manager();
    assert(all.ReadAll(manager).empty());
    assert(all.entries().size() == g.registry.size() - 1);  // the dead button is not placed
    using V = std::vector<std::uintptr_t>;
    assert((PlacedChain(all, slider) == V{slider, bar, list, top}));
    assert((PlacedChain(all, row_button) == V{row_button, row2, list, top}));
    assert((PlacedChain(all, coded) == V{coded, container, top}));
    assert((PlacedChain(all, unknown) == V{unknown, other}));
    assert(all.roots().size() == 2);

    // The climb from any widget gives the placement's chain, and the holder's name.
    for (const UiEntry& e : all.entries()) {
        UiStructure one(g.layout, 0, g.Reader());
        assert(one.Climb(manager, e.pointer).empty());
        // A class without a known node cannot climb: the caller reads the whole tree.
        assert(one.complete() == (e.pointer != unknown));
        if (!one.complete()) continue;
        V chain;
        for (std::size_t i = 0; i < one.entries().size(); ++i) {
            chain.push_back(one.entries()[i].pointer);
            assert(one.entries()[i].parent == (i + 1 < one.entries().size() ? static_cast<int>(i) + 1 : -1));
            const UiEntry& placed = all.entries()[std::find_if(all.entries().begin(), all.entries().end(), [&](const UiEntry& x) {
                                        return x.pointer == chain.back();
                                    }) - all.entries().begin()];
            assert(one.entries()[i].held_name == placed.held_name);
        }
        assert(chain == PlacedChain(all, e.pointer));
    }
    // Far fewer reads than the whole placement.
    g.reads = 0;
    assert(all.ReadAll(manager).empty());
    const int whole = g.reads;
    g.reads = 0;
    UiStructure one(g.layout, 0, g.Reader());
    assert(one.Climb(manager, row_button).empty() && g.reads < whole);
    // Not a live widget of the registry: the dead button, a pointer outside it, nothing.
    for (const std::uintptr_t not_live : {dead, std::uintptr_t{0x1234}, std::uintptr_t{0}}) {
        assert(one.Climb(manager, not_live).empty() && one.entries().empty());
    }
    // A full object finds its window.
    assert(one.Climb(manager, row1 - FakeWidgets::kPart).empty() && one.entries().size() == 3 &&
           one.entries()[0].pointer == row1);
    // A registry that is not readable.
    g.manager[3] = g.manager[2] - 8;
    assert(!one.Climb(reinterpret_cast<std::uintptr_t>(g.manager.data()), row1).empty());
}

static void TestLiveDefinitionCache() {
    FakeGui gui;
    const std::uintptr_t topbar = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t icon = gui.Define(FakeGui::kIconVtable);
    const std::uintptr_t inner = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t deep = gui.Define(FakeGui::kIconVtable);
    const std::uintptr_t alerts = gui.Define(FakeGui::kWindowVtable);
    gui.Insert(FakeGui::MapOf(topbar), "icon", icon);
    gui.Insert(FakeGui::MapOf(topbar), "inner", inner);
    gui.Insert(FakeGui::MapOf(inner), "deep", deep);
    gui.Insert(gui.registry, "topbar", topbar);
    gui.Insert(gui.registry, "alerts_parent", alerts);
    const std::uintptr_t registry = reinterpret_cast<std::uintptr_t>(&gui.registry);

    LiveDefinitionCache cache;
    const auto live = [&](std::uintptr_t definition) {
        bool out = false;
        assert(cache.IsLive(registry, gui.Layout(), gui.Reader(), definition, out).empty());
        return out;
    };
    // The first question collects; later ones read only the way up.
    cache.Begin();
    assert(live(deep) && cache.collections() == 1);
    gui.reads.clear();
    cache.Begin();
    assert(live(deep) && live(icon) && live(alerts) && cache.collections() == 1);
    assert(gui.reads.size() == 3 + 2 + 1);  // deep: its node, inner's, topbar's; icon: two; alerts: one
    assert(!live(0) && cache.collections() == 1);
    // A definition it does not know collects once per Begin(); then it is stale until the next collection.
    const std::uintptr_t unknown = gui.Define(FakeGui::kIconVtable);
    assert(!live(unknown) && cache.collections() == 2);
    assert(!live(unknown) && cache.collections() == 2);
    cache.Begin();
    assert(!live(unknown) && cache.collections() == 2);

    // A reload replaces the registry's value: the old definitions' way up breaks.
    const std::uintptr_t topbar2 = gui.Define(FakeGui::kWindowVtable);
    const std::uintptr_t icon2 = gui.Define(FakeGui::kIconVtable);
    gui.Insert(FakeGui::MapOf(topbar2), "icon", icon2);
    gui.Insert(gui.registry, "topbar", topbar2);
    cache.Begin();
    assert(!live(deep) && cache.collections() == 3);  // broke, collected again, still not there
    assert(!live(icon) && !live(topbar) && cache.collections() == 3);
    assert(live(icon2) && live(alerts) && cache.collections() == 3);
    // An unreadable registry is an error, not an answer.
    const ReadMemory none = [](std::uintptr_t, void*, std::size_t) { return false; };
    LiveDefinitionCache broken;
    bool out = true;
    broken.Begin();
    assert(!broken.IsLive(registry, gui.Layout(), none, icon2, out).empty() && !out);
}

// hovered without TONGS_UI_PATHS takes the chain; with it, or when the chain is incomplete, the whole tree.
static void TestUiServiceHoveredChain() {
    const UiTree sample = AddressedSampleTree();
    // The chain of the second option button as ReadHoveredChain gives it: the
    // button, its entry, the list box and the event window, each the child of the next.
    UiTree chain;
    for (const int i : {13, 12, 9, 8}) chain.widgets.push_back(sample.widgets[i]);
    for (auto& w : chain.widgets) w.children.clear();
    for (int i = 0; i + 1 < 4; ++i) chain.widgets[i + 1].children.push_back(i);
    chain.roots = {3};
    int trees = 0, chains = 0;
    bool complete = true;
    UiServiceHost h;
    h.current_thread = [] { return 7u; };
    h.main_thread = [] { return 7u; };
    h.read_tree = [&](UiTree& tree) {
        ++trees;
        tree = sample;
        return std::string();
    };
    h.read_hovered = [] { return Address(13); };
    h.read_hovered_chain = [&](UiTree& tree, std::uintptr_t& hovered, bool& whole) {
        ++chains;
        tree = chain;
        hovered = Address(13);
        whole = complete;
        return std::string();
    };
    h.log_error = [](const std::string&) {};
    UiService service(h);
    SeenVisits cheap, full;
    assert(service.Hovered(0, &SeenVisits::Visit, &cheap) == ANVIL_OK && chains == 1 && trees == 0);
    assert(service.Hovered(TONGS_UI_PATHS, &SeenVisits::Visit, &full) == ANVIL_OK && chains == 1 && trees == 1);
    assert(cheap.Ids() == full.Ids() && cheap.Parents() == full.Parents() && cheap.widgets.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        assert(cheap.widgets[i].raw.flags == full.widgets[i].raw.flags && !cheap.widgets[i].has_path &&
               full.widgets[i].has_path && cheap.widgets[i].name == full.widgets[i].name);
    }
    complete = false;
    cheap = {};
    assert(service.Hovered(0, &SeenVisits::Visit, &cheap) == ANVIL_OK && chains == 2 && trees == 2);
    assert(cheap.Ids() == full.Ids() && !cheap.widgets[0].has_path);
}

// definition_file and definition_line: the file and line of a live definition; never for a stale widget, even one
// whose own definition is live.
static void TestUiServiceDefinitionFile() {
    UiTree t = StaleTree();
    for (const int i : {0, 3, 19, 20, 21, 22}) {
        t.widgets[i].has_definition_file = true;
        t.widgets[i].definition_file = i < 18 ? "interface/speed_controls.gui" : "interface/topbar.gui";
        t.widgets[i].definition_line = 80u + static_cast<std::uint32_t>(i);
    }
    UiSelector sel;
    assert(ParseUiSelector("speed_controls/button_speedup", sel).empty());
    SeenVisits seen;
    assert(VisitUiFind(t, sel, 0, 0, &SeenVisits::Visit, &seen) == 1 && seen.widgets.size() == 2);
    assert(seen.widgets[0].has_file && seen.widgets[0].file == "interface/speed_controls.gui");
    assert(seen.widgets[1].has_file && seen.widgets[1].file == "interface/speed_controls.gui");
    assert(seen.widgets[0].raw.definition_line == 80u && seen.widgets[1].raw.definition_line == 83u);
    assert(ParseUiSelector("speed_controls/DateText", sel).empty());
    seen = {};
    assert(VisitUiFind(t, sel, 0, 0, &SeenVisits::Visit, &seen) == 1 && !seen.widgets[1].has_file);  // not live
    assert(seen.widgets[1].raw.definition_line == 0u);
    // The leaked hotkey_assign_parent and everything in it: stale, no file; the live one keeps it.
    assert(ParseUiSelector("//icon", sel).empty());
    seen = {};
    assert(VisitUiFind(t, sel, TONGS_UI_STALE, 0, &SeenVisits::Visit, &seen) == 2 && seen.widgets.size() == 4);
    assert(seen.widgets[0].path == "[3]" && !seen.widgets[0].has_file && seen.widgets[1].path == "[3]/icon" &&
           (seen.widgets[1].raw.flags & TONGS_UI_IS_STALE) && !seen.widgets[1].has_file);
    assert(seen.widgets[0].raw.definition_line == 0u && seen.widgets[1].raw.definition_line == 0u);
    assert(seen.widgets[3].path == "hotkey_assign_parent/icon" && seen.widgets[3].has_file &&
           seen.widgets[3].file == "interface/topbar.gui");
    seen = {};
    VisitUiHovered(t, Address(20), TONGS_UI_STALE, &SeenVisits::Visit, &seen);
    assert(seen.widgets.size() == 2 && !seen.widgets[0].has_file && !seen.widgets[1].has_file);
    assert(seen.widgets[0].raw.definition_line == 0u && seen.widgets[1].raw.definition_line == 0u);
    // The pipe's answers do not carry it.
    UiRequest r;
    bool found = false;
    assert(ParseUiRequest("find //icon", r).empty() && !Contains(UiAnswer(t, r, found), "topbar.gui"));
}

static void TestWorldParsing() {
    WorldRequest r;
    assert(!ParseWorldRequest("province", "295", r).failed() && r.kind == WorldRequest::Kind::Province && r.id == 295);
    assert(!ParseWorldRequest("province", "4941 timeout=60", r).failed() && r.id == 4941 && r.timeout_ms == 60000);
    for (const char* bad : {"0295", "-1", "+1", "0x10", "1e3", "12a", "9999999999"}) {
        assert(ParseWorldRequest("province", bad, r).error == "unknown_province");
    }
    assert(!ParseWorldRequest("province", "0", r).failed() && r.id == 0);  // the bound is the reader's
    assert(ParseWorldRequest("province", "close 295", r).error == "bad_command");
    assert(ParseWorldRequest("province", "", r).error == "bad_command");
    assert(ParseWorldRequest("province", "295 timeout=61", r).error == "bad_command");
    assert(ParseWorldRequest("province", "295 timeout=0", r).error == "bad_command");
    assert(!ParseWorldRequest("country", "rUs", r).failed() && r.kind == WorldRequest::Kind::Country && r.tag == "RUS");
    assert(!ParseWorldRequest("country", "D00", r).failed() && r.tag == "D00");
    for (const char* bad : {"RU", "RUSS", "R-S", "---"}) assert(ParseWorldRequest("country", bad, r).error == "unknown_country");
    assert(ParseWorldRequest("country", "", r).error == "bad_command");
    assert(!ParseWorldRequest("player", "", r).failed() && r.kind == WorldRequest::Kind::Player);
    assert(!ParseWorldRequest("selection", "timeout=5", r).failed() && r.kind == WorldRequest::Kind::Selection &&
           r.timeout_ms == 5000);
    assert(ParseWorldRequest("player", "RUS", r).error == "bad_command");

    // The game's hash: the upper-cased characters summed, & 0x3F.
    assert(TagBucket("RUS", 0x3F) == (('R' + 'U' + 'S') & 0x3F) && TagBucket("rus", 0x3F) == TagBucket("RUS", 0x3F));
    assert(Thousandths(4000) == "4" && Thousandths(4500) == "4.5" && Thousandths(1234) == "1.234" &&
           Thousandths(0) == "0" && Thousandths(-250) == "-0.25" && Thousandths(15000) == "15");
}

static void TestWorldAnswers() {
    ProvinceRaw p;
    p.id = 1754;
    p.name = "Murom";
    p.owner = p.controller = "RUS";
    p.cores = {"RUS"};
    p.base_tax = 4000;
    p.base_production = 7000;
    p.base_manpower = 4000;
    p.land = true;
    std::string out = ProvinceMembers(p);
    assert(Contains(out, ",\"id\":1754,\"name\":\"Murom\",\"owner\":\"RUS\",\"controller\":\"RUS\",\"cores\":[\"RUS\"]"));
    assert(Contains(out, "\"base_tax\":4,\"base_production\":7,\"base_manpower\":4,\"development\":15,\"land\":true,\"wasteland\":false"));
    p = {};
    out = ProvinceMembers(p);
    assert(Contains(out, "\"owner\":null,\"controller\":null,\"cores\":[]") && Contains(out, "\"land\":false"));

    CountryRaw c;
    c.tag = "SWE";
    c.capital = 1;
    c.capital_valid = true;
    out = CountryMembers(c);
    assert(out == ",\"tag\":\"SWE\",\"exists\":false,\"province_count\":0,\"provinces\":[],\"capital\":1,\"observer\":false");
    c.tag = "TUR";
    c.province_count = 2;
    c.provinces = {384, 2313};
    c.capital_valid = false;
    assert(Contains(CountryMembers(c), "\"exists\":true,\"province_count\":2,\"provinces\":[384,2313],\"capital\":null"));

    PlayerRaw pl;
    pl.tag = pl.player_tag = "O00";
    pl.observer = true;
    assert(PlayerMembers(pl) == ",\"tag\":\"O00\",\"player_tag\":\"O00\",\"observed_tag\":null,\"observer\":true");

    // The view is found in the tree by its widget pointer.
    UiTree t;
    UiWidget view;
    view.name = "provinceview";
    view.type = UiType::Window;
    view.shown = true;
    view.address = 0x5000;
    t.widgets.push_back(view);
    t.roots = {0};
    SelectionRaw s;
    assert(SelectionMembers(s, t) == ",\"last_selected\":null,\"view\":{\"province\":null,\"visible\":false,\"path\":null}");
    s.last_selected = 295;
    s.view_province = 295;
    s.view_widget = 0x5000;
    assert(SelectionMembers(s, t) ==
           ",\"last_selected\":295,\"view\":{\"province\":295,\"visible\":true,\"path\":\"provinceview\"}");
    t.widgets[0].shown = false;  // closed: the selection keeps both provinces (#107)
    assert(SelectionMembers(s, t) == ",\"last_selected\":295,\"view\":{\"province\":295,\"visible\":false,\"path\":null}");
    // A province without an owner: the game shows the colonisation panel instead of the view.
    UiWidget panel = view;
    panel.name = "colonisationpanel";
    panel.address = 0x6000;
    t.widgets.push_back(panel);
    t.roots = {0, 1};
    s.colonisation_widget = 0x6000;
    assert(ViewRoot(s, t) == 1 &&
           SelectionMembers(s, t) == ",\"last_selected\":295,\"view\":{\"province\":295,\"visible\":true,\"path\":\"colonisationpanel\"}");
    t.widgets[0].shown = true;  // both up: the view's window first
    assert(ViewRoot(s, t) == 0);
}

static void TestWorldCommands() {
    Handlers h;
    h.now_ms = SteadyMs;
    std::string out = HandleRequest("province 295", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"unavailable\"") && Contains(out, "\"command\":\"province\""));
    out = HandleRequest("province x", h);  // a bad request is told before anything else
    assert(Contains(out, "\"error\":\"bad_command\""));
    Dispatcher d(SteadyMs);
    h.dispatcher = &d;
    StateSnapshot game;
    game.in_game = true;
    game.date_raw = 58'860'240;
    game.load = 1;
    game.pid = 77;
    h.read_state = [&game] { return game; };
    assert(Contains(HandleRequest("player", h), "\"detail\":\"world reads are not set up\""));
    std::thread::id reader;
    h.read_province = [&reader](std::int32_t id, ProvinceRaw& p) -> WorldFailure {
        reader = std::this_thread::get_id();
        if (id >= 4942) return {"unknown_province", "no province " + std::to_string(id) + " (valid IDs 1..4941)"};
        if (id == 13) return {"read_failed", "the entry of province 13 holds ID 0"};
        p.id = id;
        p.name = "Moskva";
        p.owner = "RUS";
        return {};
    };
    h.read_country = [](std::string_view tag, CountryRaw& c) -> WorldFailure {
        if (tag != "RUS") return {"unknown_country", "the game knows no country " + std::string(tag)};
        c.tag = "RUS";
        c.province_count = 1;
        c.provinces = {295};
        return {};
    };
    h.read_player = [](PlayerRaw& p) {
        p.tag = p.player_tag = "RUS";
        return WorldFailure{};
    };
    h.read_selection = [](SelectionRaw& s) {
        s.last_selected = 295;
        return WorldFailure{};
    };
    assert(Contains(HandleRequest("selection", h), "\"error\":\"unavailable\""));  // needs the widget tree
    h.read_ui = [](UiTree&) { return std::string(); };
    out = HandleRequest("province 295 timeout=1", h);  // no frame comes
    assert(Contains(out, "\"ok\":false,\"error\":\"main thread busy\",\"command\":\"province\",\"queued_ms\":") &&
           Contains(out, "\"date\":\"1719.3.17\""));
    {
        FakeMainThread main(d);
        out = HandleRequest("province 295", h);
        assert(Contains(out, "{\"ok\":true,\"command\":\"province\",\"id\":295,\"name\":\"Moskva\",\"owner\":\"RUS\""));
        assert(Contains(out, "\"in_game\":true,\"date\":\"1719.3.17\"") && Contains(out, "\"session\":1,\"pid\":77") &&
               Contains(out, "\"queued_ms\":") && Contains(out, "\"run_ms\":") && Contains(out, "\"frame\":"));
        assert(reader != std::this_thread::get_id());  // read on the "main thread"
        out = HandleRequest("province 4942", h);
        assert(Contains(out, "{\"ok\":false,\"error\":\"unknown_province\",\"detail\":\"no province 4942 (valid IDs 1..4941)\"") &&
               Contains(out, "\"date\":\"1719.3.17\"") && Contains(out, "\"frame\":"));
        assert(Contains(HandleRequest("province 13", h), "\"error\":\"read_failed\""));
        assert(Contains(HandleRequest("province 1", h), "\"ok\":true"));  // a failed guard costs only its answer
        out = HandleRequest("country rus", h);
        assert(Contains(out, "{\"ok\":true,\"command\":\"country\",\"tag\":\"RUS\",\"exists\":true,\"province_count\":1"));
        assert(Contains(HandleRequest("country SWE", h), "\"error\":\"unknown_country\""));
        assert(Contains(HandleRequest("player", h), "{\"ok\":true,\"command\":\"player\",\"tag\":\"RUS\""));
        out = HandleRequest("selection", h);
        assert(Contains(out, "{\"ok\":true,\"command\":\"selection\",\"last_selected\":295,\"view\":{\"province\":null"));
        h.read_ui = [](UiTree&) { return std::string("access violation while reading the widget tree"); };
        assert(Contains(HandleRequest("selection", h), "\"error\":\"read_failed\",\"detail\":\"access violation"));
        // Reads run while Anvil keeps Tongs suspended, and are not counted.
        h.suspended = [] { return true; };
        assert(Contains(HandleRequest("player", h), "\"ok\":true"));
        h.suspended = {};
        game.in_game = false;
        game.reason = "no game loaded (root object is null)";
        out = HandleRequest("country RUS", h);
        assert(Contains(out, "{\"ok\":false,\"error\":\"not_in_game\",\"detail\":\"no game loaded (root object is null)\""));
    }
    assert(Contains(HandleRequest("help", h), "\"province\",\"country\",\"player\",\"selection\""));
}

static void TestNavigationRules() {
    WorldRequest r;
    assert(!ParseWorldRequest("province", "open 295", r).failed() && r.kind == WorldRequest::Kind::Open && r.id == 295);
    assert(!ParseWorldRequest("province", "goto 1252 timeout=5", r).failed() && r.kind == WorldRequest::Kind::Goto &&
           r.id == 1252 && r.timeout_ms == 5000);
    assert(ParseWorldRequest("province", "open", r).error == "bad_command");
    assert(ParseWorldRequest("province", "open x", r).error == "unknown_province");
    assert(ParseWorldRequest("province", "close 295", r).error == "bad_command");

    NavigationFacts f;
    f.id = 295;
    f.land = true;
    assert(!NavigationRefusal(true, f).failed() && !NavigationRefusal(false, f).failed());
    f.land = false;  // a sea: goto takes it, open does not
    assert(NavigationRefusal(true, f).error == "not_selectable" && !NavigationRefusal(false, f).failed());
    f.land = true;
    f.wasteland = true;
    assert(NavigationRefusal(true, f).error == "not_selectable");
    f.wasteland = false;
    f.terra_incognita = true;
    assert(NavigationRefusal(true, f).error == "terra_incognita" && !NavigationRefusal(false, f).failed());
    f.terra_incognita = false;
    f.selection_blocked = true;
    assert(NavigationRefusal(true, f).error == "selection_blocked");
    f.selection_blocked = false;
    f.camera_locked = true;  // goto refuses; open still opens
    assert(NavigationRefusal(false, f).error == "camera_locked" && !NavigationRefusal(true, f).failed());

    CameraRaw c;
    c.x = 1000.4f;
    c.z = 2000.f;
    assert(CameraReached(c, 1000.f, 2000.5f) && !CameraReached(c, 1000.f, 2000.5f, 0.1f));
    assert(!CameraReached(c, 1010.f, 2000.f));
    // Around the wrap of a 5632-wide map: 5631.5 is next to 0.2.
    c.x = 0.2f;
    assert(!CameraReached(c, 5631.5f, 2000.f));
    c.wrap = true;
    c.width = 5632.f;
    assert(CameraReached(c, 5631.5f, 2000.f) && CameraReached(c, -0.5f, 2000.f));
    assert(CameraMembers(1000.f, 2000.5f, true, false) == ",\"camera\":{\"settled\":true,\"target\":{\"x\":1000,\"z\":2000.5}}");
    assert(CameraMembers(1000.f, 2000.5f, false, true) ==
           ",\"camera\":{\"settled\":false,\"target\":{\"x\":1000,\"z\":2000.5},\"locked\":true}");
}

static void TestNavigationCommands() {
    Handlers h;
    h.now_ms = SteadyMs;
    h.sleep_ms = SleepMs;
    StateSnapshot game;
    game.in_game = true;
    game.date_raw = 58'860'240;
    game.load = 1;
    game.session = 0x10;
    h.read_state = [&game] { return game; };
    std::string out = HandleRequest("province open 295", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"input_disabled\"") && Contains(out, "\"command\":\"province open\""));
    h.send_input = [](const std::vector<InputEvent>&) { return std::string(); };
    h.suspended = [] { return true; };
    assert(Contains(HandleRequest("province goto 295", h), "\"error\":\"suspended\""));
    h.suspended = {};
    Dispatcher d(SteadyMs);
    h.dispatcher = &d;
    assert(Contains(HandleRequest("province goto 295", h), "\"error\":\"unavailable\""));

    // A fake game: provinces 1..4941, a camera that reaches its target one read later.
    NavigationFacts facts;
    int calls = 0;
    bool last_open = false;
    SelectionRaw selection;
    CameraRaw camera;
    int camera_reads = 0;
    bool fault = false;
    h.read_navigation = [&facts](std::int32_t id, NavigationFacts& out) -> WorldFailure {
        if (id < 1 || id >= 4942) return {"unknown_province", "no province " + std::to_string(id) + " (valid IDs 1..4941)"};
        out = facts;
        out.id = id;
        return {};
    };
    h.call_navigation = [&](bool open, const NavigationFacts& f, unsigned long& code) {
        ++calls;
        last_open = open;
        if (fault) {
            code = 0xC0000005;
            return false;
        }
        if (open) selection = {f.id, f.id, 0x5000};
        camera.target_x = 100.f;
        camera.target_z = 200.f;
        camera_reads = 0;
        return true;
    };
    h.read_camera = [&](CameraRaw& out) {
        if (++camera_reads >= 2) {
            camera.x = camera.target_x;
            camera.z = camera.target_z;
        }
        out = camera;
        return WorldFailure{};
    };
    h.read_selection = [&selection](SelectionRaw& out) {
        out = selection;
        return WorldFailure{};
    };
    UiTree view_tree;
    UiWidget view;
    view.name = "provinceview";
    view.type = UiType::Window;
    view.shown = true;
    view.address = 0x5000;
    view.rect = {10, 10, 100, 100};
    view_tree.widgets.push_back(view);
    view_tree.roots = {0};
    h.read_ui = [&view_tree](UiTree& tree) {
        tree = view_tree;
        return std::string();
    };
    std::vector<std::string> logged;
    h.log = [&logged](int, const std::string& message) { logged.push_back(message); };
    FakeMainThread main(d);

    facts.land = true;
    out = HandleRequest("province goto 1252", h);
    assert(Contains(out, "{\"ok\":true,\"command\":\"province goto\",\"province\":1252,\"camera\":{\"settled\":true,\"target\":{\"x\":100,\"z\":200}}"));
    assert(Contains(out, "\"client_calls_total\":1,\"waited_ms\":") && Contains(out, "\"in_game\":true"));
    assert(calls == 1 && !last_open && logged.empty());  // no log line per call

    out = HandleRequest("province open 295", h);
    assert(Contains(out, "{\"ok\":true,\"command\":\"province open\",\"province\":295,\"view\":\"provinceview\",\"covered_by\":null,\"camera\":{\"settled\":true"));
    assert(Contains(out, "\"client_calls_total\":2") && calls == 2 && last_open);

    // The game moves the target on (the pointer at the screen edge): the call's target is waited for.
    h.read_camera = [&](CameraRaw& out) {
        out = camera;
        out.target_x += static_cast<float>(10 * camera_reads++);  // the first read is the call's
        out.x = out.target_x;
        return WorldFailure{};
    };
    out = HandleRequest("province goto 1252 timeout=1", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"camera\":{\"settled\":false,\"target\":{\"x\":100,\"z\":200}}"));
    h.read_camera = [&](CameraRaw& out) {
        if (++camera_reads >= 2) {
            camera.x = camera.target_x;
            camera.z = camera.target_z;
        }
        out = camera;
        return WorldFailure{};
    };

    // Refusals call nothing and count nothing; they go to the log.
    assert(Contains(HandleRequest("province open 4942", h), "\"error\":\"unknown_province\""));
    facts.land = false;
    out = HandleRequest("province open 1252", h);
    assert(Contains(out, "{\"ok\":false,\"error\":\"not_selectable\"") && Contains(out, "\"province\":1252"));
    facts.land = true;
    facts.camera_locked = true;
    assert(Contains(HandleRequest("province goto 295", h), "\"error\":\"camera_locked\""));
    camera.locked = true;  // open still opens, and says so
    out = HandleRequest("province open 295", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"settled\":false") && Contains(out, "\"locked\":true"));
    camera.locked = false;
    facts.camera_locked = false;
    assert(calls == 4 && logged.size() == 3);

    // The discover case: a game call, refused without game_calls.
    facts.discover = true;
    out = HandleRequest("province open 151", h);
    assert(Contains(out, "\"error\":\"needs_game_calls\"") && calls == 4);
    assert(Contains(HandleRequest("province goto 151", h), "\"ok\":true"));  // the camera alone posts nothing
    h.game_calls.enabled = true;
    out = HandleRequest("province open 151", h);
    assert(Contains(out, "\"ok\":true") && Contains(out, "\"game_call\":true,\"game_calls_total\":1") &&
           !Contains(out, "client_calls_total"));
    facts.discover = false;

    // A view that does not show the province: not_shown, still counted.
    h.call_navigation = [&](bool, const NavigationFacts&, unsigned long&) {
        ++calls;
        selection = {295, 0, 0x5000};
        return true;
    };
    out = HandleRequest("province open 300", h);
    assert(Contains(out, "\"error\":\"not_shown\"") && Contains(out, "\"selected\":295,\"view_province\":null,\"visible\":true"));
    assert(Contains(out, "\"client_calls_total\":"));

    // A fault turns navigation off.
    h.call_navigation = [&](bool, const NavigationFacts&, unsigned long& code) {
        code = 0xC0000005;
        return false;
    };
    out = HandleRequest("province goto 295", h);
    assert(Contains(out, "\"error\":\"faulted\"") && Contains(out, "0xC0000005"));
    assert(Contains(HandleRequest("province goto 295", h), "\"error\":\"navigation_tripped\""));
}

int main() {
    TestWorldParsing();
    TestWorldAnswers();
    TestWorldCommands();
    TestNavigationRules();
    TestSubscribeParsing();
    TestDateCrosses();
    TestSubscriptionQueue();
    TestNotificationHub();
    TestStateChanges();
    TestEventChanges();
    TestChoiceLog();
    TestLineTail();
    TestNotificationSources();
    TestSubscribeCommands();
    TestEvents();
    TestUiClickTarget();
    TestLiveDefinitions();
    TestLivePointerDefinitions();
    TestWidgetNamePlan();
    TestStaleWidgets();
    TestUiServiceFind();
    TestUiServiceHovered();
    TestUiServiceCalls();
    TestUiServiceHoveredChain();
    TestUiServiceDefinitionFile();
    TestUiStructureClimb();
    TestLiveDefinitionCache();
    TestChildrenBounds();
    TestFitRects();
    TestUiClickCommand();
    TestUiClickChoice();
    TestEventChooseScrolls();
    TestEventCommands();
    TestInputsCount();
    TestRunUntilEventsFirst();
    TestDate();
    TestLoadWatch();
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
    TestSuspended();
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
    TestNavigationCommands();  // trips navigation for the rest of the process
    TestConsoleCommand();  // last: it trips game calls for the rest of the process
    std::puts("ok");
    return 0;
}
