#include "game_navigation_service.hpp"

#include "game_navigation.hpp"
#include "game_state.hpp"
#include "navigation_service.hpp"

#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>

#include <windows.h>

#include <utility>

namespace tongs {
namespace {

// The service lives as long as the process, as Anvil's lifetime rule asks.
NavigationService* g_service = nullptr;
// Set while loading, read on the main thread afterwards.
std::function<void()> g_count;

AnvilResult ProvinceView(TongsProvinceView* out) { return g_service->ProvinceView(out); }

AnvilResult OpenProvince(std::int32_t id, std::uint32_t flags, std::uint32_t* outcome) {
    return g_service->OpenProvince(id, flags, outcome);
}

constexpr TongsNavigationService kTable = {sizeof(TongsNavigationService), &ProvinceView, &OpenProvince};

}  // namespace

std::string RegisterNavigationService(std::uintptr_t image_base, std::function<bool()> suspended) {
    const anvil::Host host;
    // plugin.toml requires anvil-game, which knows the main thread.
    const auto* game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
    if (game == nullptr) return "anvil-game is not loaded";
    const NavigationLayout navigation = NavigationLayout::Load();
    const GameLayout state = GameLayout::Load();
    NavigationServiceHost h;
    h.current_thread = [] { return static_cast<std::uint32_t>(GetCurrentThreadId()); };
    h.main_thread = [game] { return game->main_thread_id(); };
    h.in_game = [image_base, state] { return ReadState(image_base, state).in_game; };
    h.suspended = std::move(suspended);
    h.read_view = [image_base, navigation](ProvinceViewRaw& out) { return ReadProvinceView(image_base, navigation, out); };
    h.read_facts = [image_base, navigation](std::int32_t id, NavigationFacts& out) {
        return ReadNavigationFacts(image_base, navigation, id, out);
    };
    h.call = [image_base, navigation](const NavigationFacts& facts, bool close_views, bool camera,
                                      unsigned long& fault_code) {
        return CallOpen(image_base, navigation, facts, close_views, camera, fault_code);
    };
    h.count = [] {
        if (g_count) g_count();
    };
    h.log_error = [](const std::string& message) { anvil::Host{}.warn("{}", message); };
    g_service = new NavigationService(std::move(h));
    try {
        host.register_service(TONGS_NAVIGATION_SERVICE, TONGS_NAVIGATION_SERVICE_VERSION, kTable);
    } catch (const anvil::Error& e) {
        return e.what();
    }
    return {};
}

void CountNavigationServiceCalls(std::function<void()> count) { g_count = std::move(count); }

}  // namespace tongs
