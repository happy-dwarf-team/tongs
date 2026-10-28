// Tongs — Anvil plugin entry point (plugin API 2).
#include "game_console.hpp"
#include "game_cursor.hpp"
#include "game_events.hpp"
#include "game_files.hpp"
#include "game_frame.hpp"
#include "game_input.hpp"
#include "game_state.hpp"
#include "game_symbols.hpp"
#include "game_threads.hpp"
#include "game_ui.hpp"
#include "game_ui_service.hpp"
#include "game_window.hpp"
#include "load_watch.hpp"
#include "notify_sources.hpp"
#include "pipe_server.hpp"
#include "screenshot.hpp"
#include "settings.hpp"

#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <tongs/tongs_ui.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

void Log(int level, const std::string& message) { anvil::Host{}.log(static_cast<AnvilLogLevel>(level), message); }

// Creation time of this process as Unix milliseconds; 0 when Windows does not tell.
std::uint64_t ProcessStartMs() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const std::uint64_t ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116'444'736'000'000'000ULL;  // 1970-01-01 in 100 ns since 1601
    return ticks > kUnixEpochTicks ? (ticks - kUnixEpochTicks) / 10'000 : 0;
}

// anvil.suspended / anvil.resumed: Anvil took Tongs' hooks out of an ironman
// or multiplayer game the player did not allow it in (docs/design.md, "Gameplay").
std::atomic<bool> g_suspended{false};

struct Tongs : anvil::Plugin {
    void load(anvil::Host& host) override {
        // symbols.toml limits the plugin to the one build whose layout it declares.
        const std::filesystem::path data_dir = host.data_dir();
        const tongs::Config config = tongs::LoadSettings(data_dir);
        for (const auto& note : config.notes) host.warn("{}", note);
        const std::uintptr_t image_base = host.game().image_base;
        const tongs::UiLayout ui_layout = tongs::UiLayout::Load();
        // tongs.ui only reads, so it is there whatever the settings say (docs/adr/0002).
        if (const std::string error = tongs::RegisterUiService(image_base, ui_layout); error.empty()) {
            host.info("service {} v{} registered", TONGS_UI_SERVICE, TONGS_UI_SERVICE_VERSION);
        } else {
            host.warn("cannot register {} ({})", TONGS_UI_SERVICE, error);
        }
        if (!config.enabled) {
            host.info("pipe disabled (set enabled = true in anvil/data/tongs/settings.toml)");
            return;
        }
        const tongs::GameLayout game_layout = tongs::GameLayout::Load();
        const tongs::EventLayout event_layout = tongs::EventLayout::Load();
        tongs::Handlers handlers;
        handlers.process_id = GetCurrentProcessId();
        handlers.process_start_ms = ProcessStartMs();
        handlers.log = [](int level, const std::string& message) { Log(level, message); };
        handlers.game_calls = config.game_calls;
        // Every state read also tells LoadWatch whether a game was loaded: the `session` number,
        // and a reloaded save starts game_calls_total and client_calls_total again.
        static tongs::LoadWatch load_watch;
        handlers.read_state = [image_base, game_layout, pid = handlers.process_id, start_ms = handlers.process_start_ms] {
            tongs::StateSnapshot s = tongs::ReadState(image_base, game_layout);
            load_watch.Observe(s);
            s.pid = pid;
            s.process_start_ms = start_ms;
            return s;
        };
        handlers.read_window = [] { return tongs::ReadGameWindow(); };
        handlers.take_screenshot = [folder = data_dir / "screenshots"](std::string_view name) {
            return tongs::TakeScreenshot(folder, name);
        };
        handlers.read_threads = [image_base] {
            return tongs::ReadThreads(image_base, tongs::ThreadsLayout{}, tongs::ReadGameWindow().thread_id);
        };
        handlers.sleep_ms = [](int ms) { Sleep(static_cast<DWORD>(ms)); };
        handlers.now_ms = [] {  // steady_clock is QueryPerformanceCounter: 1 ms, not GetTickCount64's ~16 ms
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count());
        };
        handlers.suspended = [] { return g_suspended.load(std::memory_order_acquire); };
        host.subscribe(ANVIL_EVENT_SUSPENDED, [host] {
            g_suspended.store(true, std::memory_order_release);
            host.warn("suspended by Anvil (ironman or multiplayer): input commands and game calls answer an error "
                      "until the game session ends; reads go on");
        });
        host.subscribe(ANVIL_EVENT_RESUMED, [host] {
            g_suspended.store(false, std::memory_order_release);
            host.info("resumed by Anvil: input commands and game calls are back");
        });
        // Main-thread jobs run after anvil-game's outer frames; they live as long as the process.
        static tongs::Dispatcher dispatcher(handlers.now_ms);
        static tongs::FrameStats frame_stats;
        if (const std::string error = tongs::SubscribeOuterFrames(&dispatcher, &frame_stats); error.empty()) {
            if (const std::string nested = tongs::CountNestedFrames(&frame_stats); !nested.empty()) {
                host.warn("{}; frames reports no nested frames", nested);
            }
            handlers.dispatcher = &dispatcher;
            handlers.frame_stats = &frame_stats;
            handlers.current_thread_id = [] { return static_cast<std::uint32_t>(GetCurrentThreadId()); };
            handlers.read_ui = [image_base, ui_layout](tongs::UiTree& tree) {
                const tongs::WindowSnapshot window = tongs::ReadGameWindow();
                const tongs::UiRect client{0, 0, window.found ? window.width : 0, window.found ? window.height : 0};
                return tongs::ReadUiTree(image_base, ui_layout, client, tree);
            };
            handlers.read_hovered = [image_base, ui_layout] { return tongs::ReadHoveredWidget(image_base, ui_layout); };
            handlers.read_events = [image_base, event_layout](tongs::EventsRaw& events) {
                return tongs::ReadEvents(image_base, event_layout, events);
            };
            handlers.count_event_windows = [image_base, event_layout] {
                return tongs::CountEventWindows(image_base, event_layout);
            };
            if (config.game_calls.enabled) {
                const tongs::ConsoleLayout console_layout = tongs::ConsoleLayout::Load();
                handlers.read_console = [image_base, console_layout](tongs::ConsoleRaw& raw) {
                    return tongs::ReadGameConsole(image_base, console_layout, raw);
                };
                handlers.call_console = [image_base, console_layout](const std::vector<std::string>& tokens,
                                                                     tongs::ConsoleCallResult& result) {
                    tongs::CallGameConsole(image_base, console_layout, tokens, result);
                };
            }
        } else {
            host.warn("cannot subscribe to {} ({}); main-thread commands disabled", ANVIL_GAME_EVENT_FRAME, error);
        }
        if (config.input) {
            handlers.send_input = [image_base](const std::vector<tongs::InputEvent>& events) {
                return tongs::PushInput(image_base, tongs::InputLayout{}, events);
            };
            if (const std::string error = tongs::InstallCursorHook(); error.empty()) {
                handlers.set_cursor = [](int x, int y) { tongs::SetVirtualCursor(x, y); };
                handlers.release_cursor = [] { tongs::ReleaseVirtualCursor(); };
            } else {
                host.warn("{}; mouse commands send SDL events only", error);
            }
        }
        host.info("{}", config.input ? "input commands enabled" : "input commands disabled (input = false)");
        if (config.game_calls.enabled) {
            std::string allowed;
            for (const auto& name : config.game_calls.allow) allowed += (allowed.empty() ? "" : ", ") + name;
            host.warn("game calls enabled: console commands can change game state{}{}{}",
                      allowed.empty() ? "" : "; console_allow: " + allowed,
                      config.game_calls.ironman ? "; allowed in ironman" : "",
                      config.game_calls.multiplayer ? "; allowed in multiplayer" : "");
        } else {
            host.info("game calls disabled (game_calls = false)");
        }
        // The moment without a game while a save loads lasts a second or more; reads every 20 ms
        // see it even when no client asks, so `session` and the totals count every load.
        std::thread([read_state = handlers.read_state] {
            for (;;) {
                read_state();
                Sleep(20);
            }
        }).detach();
        // Notifications: the hub of every subscription and the threads that feed it,
        // idle while nobody subscribes.
        static tongs::NotificationHub hub(handlers.now_ms);
        handlers.notifications = &hub;
        handlers.locate_error_log = [] { return tongs::LocateLog("error.log"); };
        handlers.locate_game_log = [] { return tongs::LocateLog("game.log"); };
        handlers.read_log_chunk = [](const std::string& path, std::uint64_t offset, std::size_t max,
                                     tongs::LineTail::Chunk& chunk) { return tongs::ReadLogChunk(path, offset, max, chunk); };
        static tongs::NotificationSources sources(handlers, hub);
        sources.Start();
        tongs::StartPipeServer(handlers, Log);
        // A pipe that failed to start only turns the plugin into a no-op.
    }
};

}  // namespace

ANVIL_PLUGIN(Tongs)
