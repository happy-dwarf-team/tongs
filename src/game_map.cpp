#include "game_map.hpp"

#include "game_memory.hpp"
#include "game_symbols.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace tongs {
namespace {

using ProvinceFn = std::int32_t (*)(void* map, const float* point);
using PickFn = void* (*)(void* map, const float* point, void* selection, std::uint8_t a, std::uint8_t b);
using KindFn = std::int32_t (*)(void* object);

struct Calls {
    ProvinceFn province;
    PickFn pick;
    std::int32_t province_out;
    void* picked;
    std::int32_t kind;
};

// No C++ objects with destructors here: the body runs under __try.
bool Call(void* map, const float* point, Calls* c, unsigned long* code) {
    __try {
        c->province_out = c->province(map, point);
        // An empty CSelectionList, as CInGameMapInput::UpdateCursor passes one: head, tail, count.
        alignas(8) unsigned char selection[0x20] = {};
        c->picked = c->pick(map, point, selection, 0, 1);
        c->kind = -1;
        if (c->picked != nullptr) {
            // Slot 1 of a selectable answers its kind, a constant (anvil-game reads the same slot).
            const auto vtable = *static_cast<void***>(c->picked);
            c->kind = reinterpret_cast<KindFn>(vtable[1])(c->picked);
        }
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

MapLayout MapLayout::Load() {
    MapLayout l;
    l.root_rva = Value(sym::root_and_date, "root");
    l.session = U32(sym::session_of_root.session);
    l.graphical_map = Value(sym::map_of_session, "graphical_map");
    l.map = U32(sym::map_of_session.map);
    l.map_vtable_rva = RvaOf(sym::graphical_map_vtable);
    l.province_rva = RvaOf(sym::province_from_point);
    l.pick_rva = RvaOf(sym::selectable_at_point);
    l.unit_id = U32(sym::unit_id.offset);
    l.unit_country = U32(sym::unit_country.country);
    return l;
}

std::string PickAtPoint(std::uintptr_t base, const MapLayout& l, int x, int y, MapPickRaw& out) {
    out = MapPickRaw{};
    std::uintptr_t root = 0, session = 0, holder = 0, map = 0, vtable = 0;
    if (!Read(base + l.root_rva, root) || root == 0 || !Read(root + l.session, session) || session == 0) {
        return "the session is not readable";
    }
    if (!Read(session + l.graphical_map, holder) || holder == 0 || !Read(holder + l.map, map) || map == 0) {
        return "no_map";
    }
    if (!Read(map, vtable) || vtable != base + l.map_vtable_rva) return "the map object is not the game's map";
    const float point[2] = {static_cast<float>(x), static_cast<float>(y)};
    Calls c{reinterpret_cast<ProvinceFn>(base + l.province_rva), reinterpret_cast<PickFn>(base + l.pick_rva), -1, nullptr,
            -1};
    unsigned long code = 0;
    if (!Call(reinterpret_cast<void*>(map), point, &c, &code)) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "the map pick faulted (0x%08lX)", code);
        return buf;
    }
    out.province = c.province_out > 0 ? c.province_out : -1;
    out.pick_kind = c.picked != nullptr ? c.kind : -1;
    if (c.picked != nullptr && (c.kind == 0 || c.kind == 1)) {
        const auto unit = reinterpret_cast<std::uintptr_t>(c.picked);
        std::uint64_t tag = 0;
        if (!Read(unit + l.unit_id, out.unit_id) || !Read(unit + l.unit_country, tag)) return "the unit is not readable";
        for (int i = 0; i < 3; ++i) {
            const char ch = static_cast<char>((tag >> (8 * i)) & 0xFF);
            if (ch == 0) break;
            out.unit_tag[i] = ch;
        }
    }
    return {};
}

}  // namespace tongs
