#pragma once

// The game's addresses and offsets, from plugin/symbols.toml: `anvil gen`
// writes symbols.gen.hpp, where every value of a symbol is a constant
// (sym::root_and_date.date) and every rip_relative value an RVA. Anvil checks
// the bytes that hold them against eu4.exe before it loads tongs.dll, so the
// layouts below take their numbers from there and nowhere else. Offsets that no
// declared instruction holds are written in the layouts, each marked
// "not in symbols.toml".
#include <anvil/anvil.hpp>

#include <cstdint>

#include "symbols.gen.hpp"

namespace tongs {

// A value of symbols.toml as the 32-bit RVA or offset the layouts keep.
consteval std::uint32_t U32(std::uint64_t value) {
    if (value > 0xFFFFFFFFu) throw "a symbols.toml value does not fit 32 bits";
    return static_cast<std::uint32_t>(value);
}

// A value that the game's code holds as a signed 8-bit displacement, e.g. the
// -8 of `mov rax, [rcx-8]`.
consteval std::int32_t S8(std::uint64_t value) {
    if (value > 0xFFu) throw "a symbols.toml value is not one byte";
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(value));
}

// value + delta, as a 32-bit offset.
consteval std::uint32_t Displaced(std::uint64_t value, std::int32_t delta) {
    return U32(static_cast<std::uint64_t>(static_cast<std::int64_t>(value) + delta));
}

// The RVA of a verified symbol whose address only Anvil knows (functions and
// pointer slots carry no value). Valid once the plugin is loading.
template <class S> std::uint32_t RvaOf(const S& symbol) {
    const anvil::Host host;
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(host.address(symbol)) - host.game().image_base);
}

// The game session's vtable: slot speed_slot of it is the verified pointer slot.
inline std::uint32_t SessionVtableRva() {
    return RvaOf(sym::session_speed_slot) - U32(sym::session_of_root.speed_slot);
}

}  // namespace tongs
