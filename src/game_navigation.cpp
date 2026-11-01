#include "game_navigation.hpp"

#include "game_memory.hpp"

#include <windows.h>

namespace tongs {
namespace {

using CameraFn = void (*)(void* session, int province);
using SessionBoolFn = bool (*)(void* session);
using SessionFn = void (*)(void* session);
using SelectFn = void (*)(void* selection, void* province);
using TutorialFn = bool (*)(void* tutorial);

WorldFailure Failed(std::string detail) { return {"read_failed", std::move(detail)}; }

std::uint8_t ByteOf(std::uint64_t tag, std::uint32_t index) { return static_cast<std::uint8_t>(tag >> (8 * index)); }

// The player's country as the game's links take it: its tag, valid and observer.
struct Player {
    bool valid = false;
    bool observer = false;
    std::int16_t index = 0;
};

bool ReadPlayerTag(std::uintptr_t root, const WorldLayout& l, Player& out) {
    std::uint64_t player = 0, observed = 0;
    if (!Read(root + l.player, player) || !Read(root + l.observed, observed)) return false;
    const bool observing = ByteOf(player, l.tag_kind) == l.observer_kind && ByteOf(observed, l.tag_valid) != 0;
    const std::uint64_t tag = observing ? observed : player;
    out.valid = ByteOf(tag, l.tag_valid) != 0;
    out.observer = ByteOf(tag, l.tag_kind) == l.observer_kind;
    out.index = static_cast<std::int16_t>(tag >> l.country_index_shift);
    return true;
}

// The bit of country `index` in the discovered-by bitset of a province.
bool Discovered(std::uintptr_t province, const NavigationLayout& l, std::int16_t index, bool& out) {
    std::uintptr_t bits = 0;
    std::uint32_t word = 0;
    if (index < 0 || !Read(province + l.discovered_by, bits) || bits == 0) return false;
    const auto i = static_cast<std::uint32_t>(index);
    if (!Read(bits + (i >> l.word_shift) * 4, word)) return false;
    out = ((word >> (i & 31)) & 1) != 0;
    return true;
}

// The province link's terra incognita test (0xFF63B8): true when it would refuse.
bool Undiscovered(std::uintptr_t province, const NavigationLayout& l, const Player& p, bool& out) {
    std::uint8_t flags = 0;
    if (!Read(province + l.ti_flags, flags)) return false;
    out = false;
    if ((flags & l.ti_bit) == 0 || !p.valid || p.observer) return true;
    bool discovered = false;
    if (!Discovered(province, l, p.index, discovered)) return false;
    out = !discovered;
    return true;
}

// No C++ objects with destructors here: the bodies run under __try.
bool TutorialBlocks(TutorialFn fn, std::uintptr_t tutorial, bool& out) {
    __try {
        out = fn(reinterpret_cast<void*>(tutorial));
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// The province links' sequence (0xFF6350): close the shown views, the camera, select.
bool Call(std::uintptr_t base, const NavigationLayout& l, bool close_views, bool camera, bool select,
          const NavigationFacts& f, unsigned long* code) {
    __try {
        auto* session = reinterpret_cast<void*>(f.session);
        if (close_views && reinterpret_cast<SessionBoolFn>(base + l.views_open_rva)(session)) {
            reinterpret_cast<SessionFn>(base + l.close_views_rva)(session);
        }
        if (camera) reinterpret_cast<CameraFn>(base + l.camera_rva)(session, f.id);
        if (select) {
            reinterpret_cast<SelectFn>(base + l.select_rva)(reinterpret_cast<void*>(f.selection),
                                                            reinterpret_cast<void*>(f.province));
        }
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ViewsOpen(std::uintptr_t base, const NavigationLayout& l, std::uintptr_t session, bool& out) {
    __try {
        out = reinterpret_cast<SessionBoolFn>(base + l.views_open_rva)(reinterpret_cast<void*>(session));
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

}  // namespace

WorldFailure ReadNavigationFacts(std::uintptr_t base, const NavigationLayout& nl, std::int32_t id, NavigationFacts& out) {
    out = {};
    out.id = id;
    const WorldLayout& l = nl.world;
    // The province's own fields first, with the world read's bound and guard.
    ProvinceRaw p;
    if (WorldFailure f = ReadProvince(base, l, id, p); f.failed()) return f;
    out.land = p.land;
    out.wasteland = p.wasteland;
    std::uintptr_t root = 0, array = 0;
    if (!Read(base + l.root_rva, root) || root == 0 || !Read(root + l.province_array, array) || array == 0) {
        return Failed("the province array is not readable");
    }
    out.province = array + static_cast<std::uintptr_t>(id) * l.province_size;
    Player player;
    if (!ReadPlayerTag(root, l, player)) return Failed("the player's tags are not readable");
    if (!Undiscovered(out.province, nl, player, out.terra_incognita)) {
        return Failed("the discovery of province " + std::to_string(id) + " is not readable");
    }

    if (!Read(root + l.session, out.session) || out.session == 0 || !Read(out.session + l.holder, out.selection) ||
        out.selection == 0) {
        return Failed("the session's selection is not readable");
    }
    std::int32_t mode = 0;
    std::uintptr_t tutorial = 0, camera = 0;
    std::uint8_t lock = 0;
    if (!Read(out.session + nl.session_mode, mode) || !Read(out.session + nl.tutorial, tutorial) ||
        !Read(out.session + nl.camera, camera) || camera == 0 || !Read(camera + nl.lock, lock)) {
        return Failed("the session is not readable");
    }
    bool blocks = false;
    if (tutorial != 0 && !TutorialBlocks(reinterpret_cast<TutorialFn>(base + nl.tutorial_blocks_rva), tutorial, blocks)) {
        return Failed("the tutorial is not readable");
    }
    out.selection_blocked = mode == static_cast<std::int32_t>(nl.blocked_mode) || blocks;
    out.camera_locked = lock != 0;

    // The discover case of the province view (0x1347A50): a foreign owner whose
    // capital may be undiscovered and is not discovered by the player.
    std::uint64_t owner = 0;
    std::int32_t layout_count = 0;
    std::uint8_t layout_flags = 0;
    if (!Read(out.province + l.owner, owner) || !Read(out.province + nl.layout_count, layout_count) ||
        !Read(out.province + nl.layout_flags, layout_flags)) {
        return Failed("province " + std::to_string(id) + " is not readable");
    }
    const auto owner_index = static_cast<std::int16_t>(owner >> l.country_index_shift);
    if (ByteOf(owner, l.tag_valid) == 0 || owner_index == player.index ||
        (layout_count > 0 && (layout_flags & nl.layout_bit) == 0)) {
        return {};
    }
    std::uintptr_t db = 0, countries = 0, country = 0, capital = 0;
    if (owner_index < 0 || !Read(base + l.countries_db_rva, db) || db == 0 || !Read(db + l.countries, countries) ||
        !Read(countries + static_cast<std::uintptr_t>(owner_index) * 8, country) || country == 0 ||
        !Read(country + nl.capital_province, capital)) {
        return Failed("the owner of province " + std::to_string(id) + " is not readable");
    }
    if (capital == 0) return {};
    if (!Undiscovered(capital, nl, player, out.discover)) return Failed("the owner's capital is not readable");
    return {};
}

bool CallNavigation(std::uintptr_t base, const NavigationLayout& l, bool open, const NavigationFacts& f,
                    unsigned long& fault_code) {
    return Call(base, l, open, true, open, f, &fault_code);
}

bool CallOpen(std::uintptr_t base, const NavigationLayout& l, const NavigationFacts& f, bool close_views, bool camera,
              unsigned long& fault_code) {
    return Call(base, l, close_views, camera, true, f, &fault_code);
}

WorldFailure ReadProvinceView(std::uintptr_t base, const NavigationLayout& nl, ProvinceViewRaw& out) {
    out = {};
    if (WorldFailure f = ReadSelection(base, nl.world, out.selection); f.failed()) return f;
    std::uint8_t shown = 0;
    if (out.selection.view_widget != 0) {
        if (!Read(out.selection.view_widget + nl.widget_shown, shown)) return Failed("the province view is not readable");
        out.view_shown = shown != 0;
    }
    if (out.selection.colonisation_widget != 0) {
        if (!Read(out.selection.colonisation_widget + nl.widget_shown, shown)) {
            return Failed("the colonisation panel is not readable");
        }
        out.colonisation_shown = shown != 0;
    }
    std::uintptr_t root = 0, session = 0;
    if (!Read(base + nl.world.root_rva, root) || root == 0 || !Read(root + nl.world.session, session) || session == 0 ||
        !ViewsOpen(base, nl, session, out.views_open)) {
        return Failed("the session's views are not readable");
    }
    return {};
}

WorldFailure ReadCamera(std::uintptr_t base, const NavigationLayout& nl, CameraRaw& out) {
    out = {};
    const WorldLayout& l = nl.world;
    std::uintptr_t root = 0, session = 0, camera = 0, map = 0;
    std::uint8_t lock = 0, wrap = 0;
    std::int32_t width = 0;
    if (!Read(base + l.root_rva, root) || root == 0 || !Read(root + l.session, session) || session == 0 ||
        !Read(session + nl.camera, camera) || camera == 0 || !Read(camera + nl.lock, lock) ||
        !Read(camera + nl.wrap, wrap) || !Read(camera + nl.current_x, out.x) || !Read(camera + nl.current_z, out.z) ||
        !Read(camera + nl.target_x, out.target_x) || !Read(camera + nl.target_z, out.target_z) ||
        !Read(base + nl.map_rva, map) || map == 0 || !Read(map + nl.map_width, width)) {
        return Failed("the camera is not readable");
    }
    out.locked = lock != 0;
    out.wrap = wrap != 0;
    out.width = static_cast<float>(width);
    return {};
}

}  // namespace tongs
