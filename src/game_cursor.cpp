#include "game_cursor.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace tongs {
namespace {

// The first instructions of 0x14E5E50: mov [rsp+10h],rbx; push rbp; push rsi;
// push rdi; sub rsp,40h.
constexpr unsigned char kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40};

struct CursorPoint {
    std::int32_t x;
    std::int32_t y;
};

using GetGuiCursorFn = CursorPoint* (*)(CursorPoint* out, std::uint8_t a, std::uint8_t b);
GetGuiCursorFn g_original = nullptr;

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

CursorPoint* Detour(CursorPoint* out, std::uint8_t a, std::uint8_t b) {
    if (g_active.load(std::memory_order_acquire)) {
        if (RealCursor() == g_real_at_set.load(std::memory_order_relaxed)) {
            const std::uint64_t v = g_virtual.load(std::memory_order_relaxed);
            out->x = High(v);
            out->y = Low(v);
            return out;
        }
        g_active.store(false, std::memory_order_release);  // the player moved the mouse: hand it back
    }
    return g_original(out, a, b);
}

}  // namespace

std::string InstallCursorHook(const AnvilHost* host, AnvilPlugin* self, const CursorLayout& layout) {
    void* original = nullptr;
    const AnvilResult result =
        host->hook_function(self, layout.rva, kPrologue, sizeof kPrologue, reinterpret_cast<void*>(&Detour), &original);
    if (result != ANVIL_OK) return "cannot hook the GUI cursor function (Anvil result " + std::to_string(result) + ")";
    g_original = reinterpret_cast<GetGuiCursorFn>(original);
    return {};
}

void SetVirtualCursor(int x, int y) {
    g_virtual.store(Pack(x, y), std::memory_order_relaxed);
    g_real_at_set.store(RealCursor(), std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
}

void ReleaseVirtualCursor() { g_active.store(false, std::memory_order_release); }

}  // namespace tongs
