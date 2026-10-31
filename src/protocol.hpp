#pragma once

#include "console.hpp"
#include "dispatcher.hpp"
#include "events.hpp"
#include "frame_stats.hpp"
#include "input.hpp"
#include "navigation.hpp"
#include "notifications.hpp"
#include "save_meta.hpp"
#include "ui_tree.hpp"
#include "world.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// One snapshot of what the plugin can read from the game (game_state.cpp).
struct StateSnapshot {
    bool in_game = false;        // a game is loaded (root object and session present)
    const char* reason = nullptr;  // why in_game is false, or why a read failed
    std::int32_t date_raw = 0;
    std::int32_t speed = 0;      // game value 0..4; the UI shows speed + 1
    bool paused = false;         // the game's own "is paused" answer (0x7E01B0)
    bool paused_flag = false;    // the player pause byte (+0x48) alone
    std::uintptr_t session = 0;  // the session object (root+0x1E00); not reported, tells loaded saves apart
    // The session the read saw (LoadWatch): 1 for the first game of the process, +1 at every
    // load; 0 when not observed. Reported as `session`.
    std::uint64_t load = 0;
    // The game process, so a client can tell answers of a restarted game apart:
    // its id and creation time (Unix ms). Filled by the plugin; 0 when unknown, then not reported.
    std::uint32_t pid = 0;
    std::uint64_t process_start_ms = 0;
};

// The game window (game_window.cpp). Mouse commands use client coordinates.
struct WindowSnapshot {
    bool found = false;
    const char* reason = nullptr;  // why found is false
    std::uintptr_t handle = 0;
    int width = 0;                 // client area
    int height = 0;
    int screen_x = 0;              // client origin on the screen
    int screen_y = 0;
    bool focused = false;          // the foreground window
    bool minimized = false;
    std::uint32_t thread_id = 0;   // the thread that owns the window: the game's main thread
    unsigned dpi = 0;              // as the window reports it (96 = 100 %)
};

// The game's worker pools and CPU masks (game_threads.cpp).
struct ThreadsSnapshot {
    bool ok = false;
    const char* reason = nullptr;   // why ok is false
    std::int32_t threads_setting = 0;  // the game's "threads" option; -1 = automatic
    struct Pool {
        std::uint32_t rva = 0;
        std::int64_t workers = -1;  // worker threads in the pool's vector; -1 = not readable
        std::int64_t capacity = -1;
    } pools[2];
    std::uint64_t process_mask = 0;  // process affinity
    std::uint64_t system_mask = 0;
    int process_threads = 0;         // all threads of the process
    std::uint32_t main_thread_id = 0;  // the window's thread; 0 = no window yet
    std::uint64_t main_mask = 0;     // its affinity (0 = not readable)
    int main_ideal_cpu = -1;
    int main_priority = 0;           // GetThreadPriority
};

// A PNG of the game window (screenshot.cpp).
struct ScreenshotResult {
    bool ok = false;
    std::string error;
    std::string path;      // UTF-8, absolute
    int width = 0;
    int height = 0;
    std::uint64_t bytes = 0;
    std::string method;    // "print_window" or "screen"
    bool blank = false;    // every pixel black: the capture probably failed
};

// Protocol: UTF-8 lines. A request is one command line; the answer is one
// JSON object on one line. Commands: ping, state, window, threads, screenshot,
// frames, ui, event, province, country, player, selection, help, wait_until, cancel,
// subscribe (see below), and with input enabled key, text, move, click, scroll,
// release, pause, resume, speed, run_until, ui click, event choose, province open/goto,
// and with game calls enabled console. Input commands and game calls answer with
// the state read after the action; they take turns across clients.
struct Handlers {
    std::function<StateSnapshot()> read_state;
    std::function<WindowSnapshot()> read_window;
    std::function<ThreadsSnapshot()> read_threads;
    // Writes a PNG named after the argument (validated: 1-48 of A-Z a-z 0-9 _ -),
    // or a time-stamped name when it is empty.
    std::function<ScreenshotResult(std::string_view name)> take_screenshot;
    // Queues SDL events for the game in order; returns an error text, or an
    // empty string on success. Left empty when input is disabled.
    std::function<std::string(const std::vector<InputEvent>&)> send_input;
    // Where the game's GUI sees the pointer (game_cursor.cpp): set before mouse
    // events, released by `release`. Optional; without it only the SDL events carry
    // the position.
    std::function<void(int x, int y)> set_cursor;
    std::function<void()> release_cursor;
    std::function<void(int ms)> sleep_ms;
    std::function<std::uint64_t()> now_ms;  // monotonic clock
    // Main-thread jobs (game_frame.cpp): both null when anvil-game's outer
    // frames are not available, and main-thread commands answer an error.
    Dispatcher* dispatcher = nullptr;
    FrameStats* frame_stats = nullptr;
    int dispatch_timeout_ms = 10000;  // default limit of a main-thread job; timeout=S changes it
    std::function<std::uint32_t()> current_thread_id;  // optional: reported by `frames` from the main thread
    // Reads the widget tree (game_ui.cpp); called on the main thread through the
    // dispatcher. Returns an error text, or an empty string on success.
    std::function<std::string(UiTree&)> read_ui;
    // Main thread: the widget under the GUI pointer (mgr+0x130, 0 = none).
    std::function<std::uintptr_t()> read_hovered;
    // Main thread: the world reads (game_world.cpp), called after the state on the
    // same frame said in game. Each fills its raw struct or answers a WorldFailure.
    // All four null: the world reads answer `unavailable`.
    std::function<WorldFailure(std::int32_t id, ProvinceRaw&)> read_province;
    std::function<WorldFailure(std::string_view tag, CountryRaw&)> read_country;
    std::function<WorldFailure(PlayerRaw&)> read_player;
    std::function<WorldFailure(SelectionRaw&)> read_selection;
    // Main thread: navigation (game_navigation.cpp), client calls behind `input`.
    // read_navigation reads what the call checks first; call_navigation closes the
    // views, moves the camera and (open) selects, and returns false when the
    // game's code faulted; read_camera reads the camera. All null: unavailable.
    std::function<WorldFailure(std::int32_t id, NavigationFacts&)> read_navigation;
    std::function<bool(bool open, const NavigationFacts&, unsigned long& fault_code)> call_navigation;
    std::function<WorldFailure(CameraRaw&)> read_camera;
    // Loading a save (game_save.cpp), a game call. On the pipe thread: where the
    // game would find a save (an error text when it cannot tell), its first
    // bytes, the running version. On the main thread: the loader (it returns
    // after the whole load), closing the in-game windows, the loading flag.
    // All null: `save load` answers unavailable.
    std::function<std::string(const std::string& name, std::string& path, bool& exists)> locate_save;
    std::function<std::string(const std::string& path, std::string& bytes)> read_save;
    std::function<bool(std::array<int, 4>&)> read_version;
    std::function<LoadCall(const std::string& name)> call_load;
    std::function<bool(unsigned long& fault_code)> close_windows;
    std::function<bool(bool& loading)> read_loading;
    SaveTokens save_tokens;
    // Main thread: the open events (game_events.cpp). Returns an error text, or empty.
    std::function<std::string(EventsRaw&)> read_events;
    // Pipe thread, one word: entries in the session's event window list; -1 = unreadable.
    // run_until events=first reads the events on the main thread only while it is above 0.
    std::function<int()> count_event_windows;
    // True while Anvil keeps Tongs suspended (anvil.suspended): an ironman or
    // multiplayer game that the player did not allow Tongs in (anvil.toml).
    // Input commands and game calls then answer an error; reads go on. Optional.
    std::function<bool()> suspended;
    // Game calls (ADR 0001): the settings, and two main-thread functions
    // (game_console.cpp). read_console returns an error text, or empty;
    // call_console runs one tokenised console line and reports a fault instead
    // of raising it.
    GameCallPolicy game_calls;
    std::function<std::string(ConsoleRaw&)> read_console;
    std::function<void(const std::vector<std::string>& tokens, ConsoleCallResult&)> call_console;
    // Notifications (0.8.0): the hub of every subscription, and where error.log
    // and game.log are (game.log names the event choices). read_log_chunk reads
    // up to `max` bytes from `offset` and the file's size and identity
    // (game_files.cpp); it returns an error text, or empty.
    NotificationHub* notifications = nullptr;
    std::function<ErrorLogLocation()> locate_error_log;
    std::function<ErrorLogLocation()> locate_game_log;  // optional: without it `chosen` names only Tongs' choices
    std::function<std::string(const std::string& path, std::uint64_t offset, std::size_t max, LineTail::Chunk&)>
        read_log_chunk;
    // The Anvil log (level as AnvilLogLevel: 1 info, 2 warning, 3 error); optional.
    std::function<void(int level, const std::string& message)> log;
    std::uint32_t process_id = 0;
    std::uint64_t process_start_ms = 0;  // creation time of the game process (Unix ms); 0 when unknown
    // How long pause/resume/speed wait for the game to react. Right after a save
    // loads, the main loop handled a key press only 2-7 s later.
    int wait_timeout_ms = 10000;
    int poll_interval_ms = 10;
    int key_settle_ms = 100;     // key, text and mouse commands wait this long before reading the state
};

// `client` is the pipe instance (from 1) that sent the line, for the log.
std::string HandleRequest(std::string_view line, const Handlers& handlers, int client = 0);

// Subscriptions (pipe_server.cpp). A connection that sends `subscribe KINDS
// [date=day|month|year]` gets the answer, then only notifications, until it
// sends `unsubscribe` (back to ordinary requests; what is still queued is
// discarded) or closes. Meanwhile SubscribedRequest answers `ping` and
// `unsubscribe` and refuses everything else. Subscribe sets `out` on success.
bool IsSubscribe(std::string_view line);
std::string Subscribe(std::string_view line, const Handlers& handlers, std::shared_ptr<Subscription>& out);
std::string SubscribedRequest(std::string_view line, const Handlers& handlers, std::shared_ptr<Subscription>& subscription);

std::string JsonEscape(std::string_view text);

// "tongs" when `save load` made the session `session` (StateSnapshot::load), else null:
// `loaded_by` of the entered_game notification.
const char* LoadedBy(std::uint64_t session);

// Counts one client call (ADR 0003) that called the game: `client_calls` of
// every wait in progress, an `activity` notification with what "client_call",
// and client_calls_total of the save `before` was read in, which it returns.
// For the commands that make client calls: province open and province goto.
std::uint64_t CountClientCall(const Handlers& handlers, std::string_view command, const StateSnapshot& before);

}  // namespace tongs
