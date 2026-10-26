#include "game_ui_service.hpp"

#include "game_window.hpp"
#include "ui_service.hpp"

#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>

#include <windows.h>

namespace tongs {
namespace {

// The service lives as long as the process, as Anvil's lifetime rule asks.
UiService* g_service = nullptr;

AnvilResult Find(const char* selector, std::uint32_t flags, TongsUiVisit visit, void* context, std::uint32_t* count) {
    return g_service->Find(selector, flags, visit, context, count);
}

AnvilResult Hovered(std::uint32_t flags, TongsUiVisit visit, void* context) {
    return g_service->Hovered(flags, visit, context);
}

constexpr TongsUiService kTable = {sizeof(TongsUiService), &Find, &Hovered};

}  // namespace

std::string RegisterUiService(std::uintptr_t image_base, const UiLayout& layout) {
    const anvil::Host host;
    // plugin.toml requires anvil-game, which knows the main thread.
    const auto* game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
    if (game == nullptr) return "anvil-game is not loaded";
    UiServiceHost h;
    h.current_thread = [] { return static_cast<std::uint32_t>(GetCurrentThreadId()); };
    h.main_thread = [game] { return game->main_thread_id(); };
    h.read_tree = [image_base, layout](UiTree& tree) {
        const WindowSnapshot window = ReadGameWindow();
        const UiRect client{0, 0, window.found ? window.width : 0, window.found ? window.height : 0};
        return ReadUiTree(image_base, layout, client, tree);
    };
    h.read_hovered = [image_base, layout] { return ReadHoveredWidget(image_base, layout); };
    h.log_error = [](const std::string& message) { anvil::Host{}.warn("{}", message); };
    g_service = new UiService(std::move(h));
    try {
        host.register_service(TONGS_UI_SERVICE, TONGS_UI_SERVICE_VERSION, kTable);
    } catch (const anvil::Error& e) {
        return e.what();
    }
    return {};
}

}  // namespace tongs
