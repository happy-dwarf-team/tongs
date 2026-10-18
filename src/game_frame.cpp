#include "game_frame.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace tongs {
namespace {

// The first instructions of 0x14C25A0: mov [rsp+8],rbx; mov [rsp+10h],rbp;
// mov [rsp+18h],rsi; mov [rsp+20h],rdi; push r14; sub rsp,70h. Nothing in
// them is RIP-relative.
constexpr unsigned char kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                       0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70};

// The callers ignore the return value (they test app+0x83 next); passing rax
// through keeps it whatever it is.
using FrameFn = std::uintptr_t (*)(void* app, std::uint8_t outer);
// Anvil activates the hook before it hands back the original; a frame in that
// instant waits for it.
std::atomic<FrameFn> g_original{nullptr};

FrameLayout g_layout;
Dispatcher* g_dispatcher = nullptr;
FrameStats* g_stats = nullptr;
std::atomic<DWORD> g_main_thread{0};

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uintptr_t Detour(void* app, std::uint8_t outer) {
    FrameFn original = g_original.load(std::memory_order_acquire);
    while (original == nullptr) {
        YieldProcessor();
        original = g_original.load(std::memory_order_acquire);
    }
    const std::uintptr_t result = original(app, outer);
    if (outer == 0) {
        g_stats->OnNested(NowMs());
        return result;
    }
    // The first outer frame comes from the main loop; any other thread is not ours to run jobs on.
    const DWORD thread = GetCurrentThreadId();
    DWORD expected = 0;
    if (!g_main_thread.compare_exchange_strong(expected, thread) && expected != thread) return result;
    g_stats->OnOuter(NowMs());
    const auto* bytes = static_cast<const std::uint8_t*>(app);
    if (bytes[g_layout.quit_flag] != 0 || bytes[g_layout.quit_flag_2] != 0) return result;
    const std::uint64_t frame = *reinterpret_cast<const std::uint64_t*>(bytes + g_layout.frame_counter);
    g_dispatcher->RunBatch(frame);
    return result;
}

}  // namespace

std::string InstallFrameHook(const AnvilHost* host, AnvilPlugin* self, const FrameLayout& layout,
                             Dispatcher* dispatcher, FrameStats* stats) {
    g_layout = layout;
    g_dispatcher = dispatcher;
    g_stats = stats;
    void* original = nullptr;
    const AnvilResult result =
        host->hook_function(self, layout.rva, kPrologue, sizeof kPrologue, reinterpret_cast<void*>(&Detour), &original);
    if (result != ANVIL_OK) return "cannot hook the frame function (Anvil result " + std::to_string(result) + ")";
    g_original.store(reinterpret_cast<FrameFn>(original), std::memory_order_release);
    return {};
}

}  // namespace tongs
