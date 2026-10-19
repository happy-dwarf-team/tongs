#include "game_frame.hpp"

#include "game_symbols.hpp"

#include <anvil/anvil_game.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tongs {
namespace {

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

std::string SubscribeOuterFrames(Dispatcher* dispatcher, FrameStats* stats) {
    const anvil::Host host;
    // plugin.toml requires anvil-game, so Anvil loads it first; without it no frame event would come.
    if (host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION) == nullptr) {
        return "anvil-game is not loaded";
    }
    try {
        host.subscribe(ANVIL_GAME_EVENT_FRAME, [dispatcher, stats](const void* payload) {
            const auto* frame = static_cast<const AnvilGameFrame*>(payload);
            if (frame == nullptr || frame->size < offsetof(AnvilGameFrame, number) + sizeof frame->number) return;
            stats->OnOuter(NowMs());
            dispatcher->RunBatch(frame->number);
        });
    } catch (const anvil::Error& e) {
        return e.what();
    }
    return {};
}

std::string CountNestedFrames(FrameStats* stats) {
    try {
        // The hook lives as long as the process, like the stats it feeds.
        anvil::Host{}.mid_hook(sym::nested_frame, [stats] { stats->OnNested(NowMs()); }).release();
    } catch (const anvil::Error& e) {
        return e.what();
    }
    return {};
}

}  // namespace tongs
