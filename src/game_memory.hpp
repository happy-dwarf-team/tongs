#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tongs {

// Reads of game memory for the main-thread readers (game_ui.cpp, game_events.cpp).
// Each read is guarded on its own, so an object that changes under us costs
// that object, not the whole answer.
bool ReadRaw(std::uintptr_t address, void* out, std::size_t size);

template <typename T>
bool Read(std::uintptr_t address, T& out) {
    return ReadRaw(address, &out, sizeof(T));
}

// MSVC std::string: 16-byte buffer or pointer, size at +0x10, capacity at +0x18.
// Strings longer than 64 KiB are taken as garbage.
bool ReadString(std::uintptr_t address, std::string& out);

}  // namespace tongs
