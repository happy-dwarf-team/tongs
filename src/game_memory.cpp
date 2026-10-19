#include "game_memory.hpp"

#include <windows.h>

#include <cstring>

namespace tongs {
namespace {

constexpr std::size_t kMaxText = 65536;

}  // namespace

bool ReadRaw(std::uintptr_t address, void* out, std::size_t size) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool ReadString(std::uintptr_t address, std::string& out) {
    unsigned char raw[0x20];
    if (!ReadRaw(address, raw, sizeof raw)) return false;
    std::uint64_t size = 0, capacity = 0;
    std::memcpy(&size, raw + 0x10, 8);
    std::memcpy(&capacity, raw + 0x18, 8);
    if (size > capacity || size > kMaxText) return false;
    if (capacity < 16) {
        out.assign(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(size));
        return true;
    }
    std::uintptr_t data = 0;
    std::memcpy(&data, raw, 8);
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || ReadRaw(data, out.data(), out.size());
}

}  // namespace tongs
