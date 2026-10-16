// Tongs — Anvil plugin entry point.
#include "config.hpp"
#include "game_input.hpp"
#include "game_state.hpp"
#include "pipe_server.hpp"

#include <anvil/anvil_plugin.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

}  // namespace

extern "C" ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost* host, AnvilPlugin* self) {
    if (host == nullptr || host->api_version != ANVIL_API_VERSION || host->size < sizeof(AnvilHost)) {
        return ANVIL_ERROR_UNSUPPORTED;
    }
    g_host = host;
    g_self = self;
    // plugin.toml limits the plugin to the one build whose layout game_state.hpp describes.
    const tongs::Config config = LoadConfig(std::filesystem::path(host->data_dir(self)));
    for (const auto& note : config.notes) Log(ANVIL_LOG_WARNING, note);
    if (!config.enabled) {
        Log(ANVIL_LOG_INFO, "pipe disabled (set enabled = true in anvil/data/tongs/config.toml)");
        return ANVIL_OK;
    }
    const std::uintptr_t image_base = host->game->image_base;
    tongs::Handlers handlers;
    handlers.process_id = GetCurrentProcessId();
    handlers.read_state = [image_base] { return tongs::ReadState(image_base, tongs::GameLayout{}); };
    handlers.sleep_ms = [](int ms) { Sleep(static_cast<DWORD>(ms)); };
    handlers.now_ms = [] { return static_cast<std::uint64_t>(GetTickCount64()); };
    if (config.input) {
        handlers.press_key = [image_base](const tongs::KeyPress& key) {
            return tongs::PressKey(image_base, tongs::InputLayout{}, key);
        };
    }
    Log(ANVIL_LOG_INFO, config.input ? "input commands enabled" : "input commands disabled (input = false)");
    tongs::StartPipeServer(handlers, Log);
    return ANVIL_OK;  // a pipe that failed to start only turns the plugin into a no-op
}
