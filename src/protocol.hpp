#pragma once

#include "input.hpp"

#include <cstdint>
#include <functional>
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

// Protocol: UTF-8 lines. A request is one command line; the answer is one
// JSON object on one line. Commands: ping, state, window, threads, help,
// wait_until, cancel, and with input enabled key, text, move, click, scroll,
// release, pause, resume, speed, run_until. Input commands answer with the state read after
// the action; they take turns across clients.
struct Handlers {
    std::function<StateSnapshot()> read_state;
    std::function<WindowSnapshot()> read_window;
    std::function<ThreadsSnapshot()> read_threads;
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
    std::uint32_t process_id = 0;
    // How long pause/resume/speed wait for the game to react. Right after a save
    // loads, the main loop handled a key press only 2-7 s later.
    int wait_timeout_ms = 10000;
    int poll_interval_ms = 10;
    int key_settle_ms = 100;     // key, text and mouse commands wait this long before reading the state
};

std::string HandleRequest(std::string_view line, const Handlers& handlers);

std::string JsonEscape(std::string_view text);

}  // namespace tongs
