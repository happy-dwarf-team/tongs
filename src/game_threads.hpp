#pragma once

#include "protocol.hpp"

#include <cstdint>

namespace tongs {

// Addresses for EU4 1.37.5 (Steam, win64); plugin.toml checks the code that
// sets them up. Each pool is a global object whose worker threads sit in a
// vector of 8-byte handles: begin at +0x38, end at +0x40, capacity end at
// +0x48 (lab repository, docs/reverse-engineering.md, "Inicjalizacja puli").
struct ThreadsLayout {
    std::uint32_t pool_rvas[2] = {0x242B8E0, 0x242B950};
    std::uint32_t vector_begin = 0x38;
    std::uint32_t vector_end = 0x40;
    std::uint32_t vector_capacity = 0x48;
    std::uint32_t threads_setting_rva = 0x1FC23AC;  // int32, written at start-up (0x876999)
};

// Reads the pools and the CPU masks; `main_thread_id` is the game window's
// thread (0 when there is no window yet). Any thread may call it.
ThreadsSnapshot ReadThreads(std::uintptr_t image_base, const ThreadsLayout& layout, std::uint32_t main_thread_id);

}  // namespace tongs
