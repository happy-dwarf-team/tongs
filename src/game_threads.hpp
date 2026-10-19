#pragma once

#include "game_symbols.hpp"
#include "protocol.hpp"

#include <cstdint>

namespace tongs {

// Addresses for EU4 1.37.5 (Steam, win64), from the code that sets them up
// (symbols.toml). Each pool is a global object whose worker threads sit in a
// std::vector of 8-byte handles: begin, end, capacity end (lab repository,
// docs/reverse-engineering.md, "Inicjalizacja puli").
struct ThreadsLayout {
    std::uint32_t pool_rvas[2] = {U32(sym::worker_pool_1.pool), U32(sym::worker_pool_2.pool)};
    std::uint32_t vector_rvas[2] = {U32(sym::worker_pool_1_workers.workers), U32(sym::worker_pool_2_workers.workers)};
    std::uint32_t threads_setting_rva = U32(sym::threads_option.threads);  // int32, written at start-up
};

// Reads the pools and the CPU masks; `main_thread_id` is the game window's
// thread (0 when there is no window yet). Any thread may call it.
ThreadsSnapshot ReadThreads(std::uintptr_t image_base, const ThreadsLayout& layout, std::uint32_t main_thread_id);

}  // namespace tongs
