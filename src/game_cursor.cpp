#include "game_cursor.hpp"

#include "game_symbols.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace tongs {
namespace {

struct CursorPoint {
    std::int32_t x;
    std::int32_t y;
};

// The hook and the rest of its chain; Anvil writes `next` before it enables the hook.
anvil::HookOf<sym::gui_cursor> g_hook;

// Packed state shared between the pipe threads and the game's main thread:
// virtual client point, and the real screen point seen when it was set.
std::atomic<bool> g_active{false};
std::atomic<std::uint64_t> g_virtual{0};
std::atomic<std::uint64_t> g_real_at_set{0};

std::uint64_t Pack(std::int32_t x, std::int32_t y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
}

std::int32_t High(std::uint64_t v) { return static_cast<std::int32_t>(v >> 32); }
std::int32_t Low(std::uint64_t v) { return static_cast<std::int32_t>(v & 0xFFFFFFFFu); }

std::uint64_t RealCursor() {
    POINT p{};
    GetCursorPos(&p);
    return Pack(p.x, p.y);
}

void* Detour(void* out, std::uint8_t a, std::uint8_t b) noexcept {
    if (g_active.load(std::memory_order_acquire)) {
        if (RealCursor() == g_real_at_set.load(std::memory_order_relaxed)) {
            const std::uint64_t v = g_virtual.load(std::memory_order_relaxed);
            auto* point = static_cast<CursorPoint*>(out);
            point->x = High(v);
            point->y = Low(v);
            return out;
        }
        g_active.store(false, std::memory_order_release);  // the player moved the mouse: hand it back
    }
    return g_hook.next(out, a, b);
}

}  // namespace

std::string InstallCursorHook() {
    try {
        anvil::Host{}.hook(g_hook, sym::gui_cursor, Detour);
    } catch (const anvil::Error& e) {
        return std::string("cannot hook the GUI cursor function (") + e.what() + ")";
    }
    return {};
}

void SetVirtualCursor(int x, int y) {
    g_virtual.store(Pack(x, y), std::memory_order_relaxed);
    g_real_at_set.store(RealCursor(), std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
}

void ReleaseVirtualCursor() { g_active.store(false, std::memory_order_release); }

bool VirtualCursorHeld() { return g_active.load(std::memory_order_acquire); }

}  // namespace tongs
