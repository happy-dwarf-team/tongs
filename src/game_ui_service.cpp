#include "game_ui_service.hpp"

#include "game_window.hpp"
#include "ui_service.hpp"

#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>

#include <windows.h>

#include <memory>

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

// The game window's client area: the window found once, then only asked for
// its size while it lives (cheaper than a search each frame).
UiRect ClientArea() {
    static HWND window = nullptr;
    if (window == nullptr || !IsWindow(window)) {
        const WindowSnapshot found = ReadGameWindow();
        window = found.found ? reinterpret_cast<HWND>(found.handle) : nullptr;
    }
    RECT client{};
    if (window == nullptr || !GetClientRect(window, &client)) return {};
    return {0, 0, client.right - client.left, client.bottom - client.top};
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
    h.read_tree = [image_base, layout](UiTree& tree) { return ReadUiTree(image_base, layout, ClientArea(), tree); };
    h.read_hovered = [image_base, layout] { return ReadHoveredWidget(image_base, layout); };
    // Main thread only, as every call: the cache needs no lock.
    h.read_hovered_chain = [image_base, layout, definitions = std::make_shared<LiveDefinitionCache>()](
                               UiTree& tree, std::uintptr_t& hovered, bool& complete) {
        return ReadHoveredChain(image_base, layout, ClientArea(), *definitions, tree, hovered, complete);
    };
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
