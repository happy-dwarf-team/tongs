// Tongs — Anvil plugin entry point.
#include "config.hpp"
#include "game_console.hpp"
#include "game_cursor.hpp"
#include "game_events.hpp"
#include "game_files.hpp"
#include "game_frame.hpp"
#include "game_input.hpp"
#include "game_state.hpp"
#include "game_threads.hpp"
#include "game_ui.hpp"
#include "game_window.hpp"
#include "load_watch.hpp"
#include "notify_sources.hpp"
#include "pipe_server.hpp"
#include "screenshot.hpp"

#include <anvil/anvil_plugin.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const AnvilHost* g_host = nullptr;
AnvilPlugin* g_self = nullptr;

void Log(int level, const std::string& message) {
    g_host->log(g_self, static_cast<AnvilLogLevel>(level), message.c_str());
}

tongs::Config LoadConfig(const std::filesystem::path& data_dir) {
    const std::filesystem::path file = data_dir / "config.toml";
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::ofstream(file, std::ios::binary) << tongs::kDefaultConfig;
        return tongs::ParseConfig(tongs::kDefaultConfig);
    }
    std::ostringstream text;
    text << in.rdbuf();
    return tongs::ParseConfig(text.str());
}

// Creation time of this process as Unix milliseconds; 0 when Windows does not tell.
std::uint64_t ProcessStartMs() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const std::uint64_t ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116'444'736'000'000'000ULL;  // 1970-01-01 in 100 ns since 1601
    return ticks > kUnixEpochTicks ? (ticks - kUnixEpochTicks) / 10'000 : 0;
}

}  // namespace

extern "C" ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost* host, AnvilPlugin* self) {
    if (host == nullptr || host->api_version != ANVIL_API_VERSION || host->size < sizeof(AnvilHost)) {
        return ANVIL_ERROR_UNSUPPORTED;
    }
    g_host = host;
    g_self = self;
    // plugin.toml limits the plugin to the one build whose layout game_state.hpp describes.
    const std::filesystem::path data_dir(host->data_dir(self));
    const tongs::Config config = LoadConfig(data_dir);
    for (const auto& note : config.notes) Log(ANVIL_LOG_WARNING, note);
    if (!config.enabled) {
        Log(ANVIL_LOG_INFO, "pipe disabled (set enabled = true in anvil/data/tongs/config.toml)");
        return ANVIL_OK;
    }
    const std::uintptr_t image_base = host->game->image_base;
    tongs::Handlers handlers;
    handlers.process_id = GetCurrentProcessId();
    handlers.process_start_ms = ProcessStartMs();
    handlers.log = [](int level, const std::string& message) { Log(level, message); };
    handlers.game_calls = config.game_calls;
    // Every state read also tells LoadWatch whether a game was loaded, so a reloaded save starts game_calls_total again.
    static tongs::LoadWatch load_watch;
    handlers.read_state = [image_base, pid = handlers.process_id, start_ms = handlers.process_start_ms] {
        tongs::StateSnapshot s = tongs::ReadState(image_base, tongs::GameLayout{});
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
    // Main-thread jobs: they live as long as the process, like the hook that runs them.
    static tongs::Dispatcher dispatcher(handlers.now_ms);
    static tongs::FrameStats frame_stats;
    if (const std::string error = tongs::InstallFrameHook(host, self, tongs::FrameLayout{}, &dispatcher, &frame_stats);
        error.empty()) {
        handlers.dispatcher = &dispatcher;
        handlers.frame_stats = &frame_stats;
        handlers.current_thread_id = [] { return static_cast<std::uint32_t>(GetCurrentThreadId()); };
        handlers.read_ui = [image_base](tongs::UiTree& tree) { return tongs::ReadUiTree(image_base, tongs::UiLayout{}, tree); };
        handlers.read_hovered = [image_base] { return tongs::ReadHoveredWidget(image_base, tongs::UiLayout{}); };
        handlers.read_events = [image_base](tongs::EventsRaw& events) {
            return tongs::ReadEvents(image_base, tongs::EventLayout{}, events);
        };
        handlers.count_event_windows = [image_base] { return tongs::CountEventWindows(image_base, tongs::EventLayout{}); };
        if (config.game_calls.enabled) {
            handlers.read_console = [image_base](tongs::ConsoleRaw& raw) {
                return tongs::ReadGameConsole(image_base, tongs::ConsoleLayout{}, raw);
            };
            handlers.call_console = [image_base](const std::vector<std::string>& tokens, tongs::ConsoleCallResult& result) {
                tongs::CallGameConsole(image_base, tongs::ConsoleLayout{}, tokens, result);
            };
        }
    } else {
        Log(ANVIL_LOG_WARNING, error + "; main-thread commands disabled");
    }
    if (config.input) {
        handlers.send_input = [image_base](const std::vector<tongs::InputEvent>& events) {
            return tongs::PushInput(image_base, tongs::InputLayout{}, events);
        };
        if (const std::string error = tongs::InstallCursorHook(host, self, tongs::CursorLayout{}); error.empty()) {
            handlers.set_cursor = [](int x, int y) { tongs::SetVirtualCursor(x, y); };
            handlers.release_cursor = [] { tongs::ReleaseVirtualCursor(); };
        } else {
            Log(ANVIL_LOG_WARNING, error + "; mouse commands send SDL events only");
        }
    }
    Log(ANVIL_LOG_INFO, config.input ? "input commands enabled" : "input commands disabled (input = false)");
    if (config.game_calls.enabled) {
        std::string allowed;
        for (const auto& name : config.game_calls.allow) allowed += (allowed.empty() ? "" : ", ") + name;
        Log(ANVIL_LOG_WARNING, std::string("game calls enabled: console commands can change game state") +
                                   (allowed.empty() ? "" : "; console_allow: " + allowed) +
                                   (config.game_calls.ironman ? "; allowed in ironman" : "") +
                                   (config.game_calls.multiplayer ? "; allowed in multiplayer" : ""));
        // The moment without a game while a save loads lasts a second or more; reads every
        // 20 ms see it even when no client asks. Only with game calls, which are what it counts.
        std::thread([read_state = handlers.read_state] {
            for (;;) {
                read_state();
                Sleep(20);
            }
        }).detach();
    } else {
        Log(ANVIL_LOG_INFO, "game calls disabled (game_calls = false)");
    }
    // Notifications: the hub of every subscription and the threads that feed it,
    // idle while nobody subscribes.
    static tongs::NotificationHub hub(handlers.now_ms);
    handlers.notifications = &hub;
    handlers.locate_error_log = [image_base] { return tongs::LocateErrorLog(image_base); };
    handlers.read_log_chunk = [](const std::string& path, std::uint64_t offset, std::size_t max,
                                 tongs::LineTail::Chunk& chunk) { return tongs::ReadLogChunk(path, offset, max, chunk); };
    static tongs::NotificationSources sources(handlers, hub);
    sources.Start();
    tongs::StartPipeServer(handlers, Log);
    return ANVIL_OK;  // a pipe that failed to start only turns the plugin into a no-op
}
