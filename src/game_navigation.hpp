#pragma once

#include "game_symbols.hpp"
#include "game_world.hpp"
#include "navigation.hpp"

#include <cstdint>

namespace tongs {

// The camera, the province selection and the game's own guards of its
// province links (docs/design.md, "Navigation (0.21.0)"), from plugin/symbols.toml.
struct NavigationLayout {
    WorldLayout world;
    std::uint32_t camera = U32(sym::camera_to_province.camera);             // session: the camera
    std::uint32_t lock = U32(sym::camera_set_target.lock);                  // camera: byte, debug_lockcamera
    std::uint32_t current_x = U32(sym::camera_snap.current_x);              // camera: float
    std::uint32_t current_z = U32(sym::camera_snap.current_z);
    std::uint32_t target_x = U32(sym::camera_set_target.target_x);
    std::uint32_t target_z = U32(sym::camera_set_target.target_z);
    std::uint32_t wrap = U32(sym::camera_to_province.wrap);                 // camera: byte, the map wraps
    std::uint32_t map_rva = U32(sym::camera_to_province.map);               // pointer to the map
    std::uint32_t map_width = U32(sym::camera_to_province.map_width);       // map: int32
    std::uint32_t selection_session = U32(sym::select_province.session);   // selection: the session
    std::uint32_t tutorial = U32(sym::select_province.tutorial);            // session: the tutorial, or null
    std::uint32_t session_mode = U32(sym::select_province.session_mode);    // session: int32
    std::uint32_t blocked_mode = U32(sym::select_province.blocked_mode);    // the mode in which nothing is selected
    std::uint32_t ti_flags = U32(sym::terra_incognita_check.ti_flags);      // province: byte
    std::uint32_t ti_bit = U32(sym::terra_incognita_check.ti_bit);          // set: the province can be undiscovered
    std::uint32_t discovered_by = U32(sym::terra_incognita_check.discovered_by);  // province: uint32* bitset by country index
    std::uint32_t word_shift = U32(sym::terra_incognita_check.word_shift);  // index >> 5: the word of the bitset
    std::uint32_t layout_count = U32(sym::province_view_layout.layout_count);  // province: int32
    std::uint32_t layout_flags = U32(sym::province_view_layout.layout_flags);  // province: byte
    std::uint32_t layout_bit = U32(sym::province_view_layout.layout_bit);
    std::uint32_t capital_province = U32(sym::province_view_discover.capital_province);  // country: Province*
    std::uint32_t camera_rva = 0;       // RvaOf(sym::camera_to_province_id): (session, id)
    std::uint32_t views_open_rva = 0;   // RvaOf(sym::views_open): bool (session)
    std::uint32_t close_views_rva = 0;  // RvaOf(sym::close_views): (session)
    std::uint32_t select_rva = 0;       // RvaOf(sym::select_province): (selection, province)
    std::uint32_t tutorial_blocks_rva = 0;  // RvaOf(sym::tutorial_blocks): bool (tutorial)

    static_assert(sym::camera_to_province_id.root == sym::root_and_date.root &&
                  sym::camera_to_province_id.provinces == sym::province_count.provinces &&
                  sym::camera_to_province_id.provinces_end == sym::province_count.provinces_end &&
                  sym::camera_to_province_id.province_size == sym::province_is_sea.province_size &&
                  sym::camera_to_province_id.province_array == sym::province_is_sea.province_array);
    static_assert(sym::camera_to_province.current_x == sym::camera_snap.current_x &&
                  sym::camera_snap.target_x == sym::camera_set_target.target_x &&
                  sym::camera_snap.target_z == sym::camera_set_target.target_z);
    static_assert(sym::province_view_discover.ti_flags == sym::terra_incognita_check.ti_flags &&
                  sym::province_view_discover.ti_bit == sym::terra_incognita_check.ti_bit);
    // The owner's valid byte, as the view tests it: the tag at +0x468, its byte 7.
    static_assert(sym::province_view_layout.owner_valid ==
                  sym::province_base_tax.owner + (sym::player_country.observed_valid - sym::player_country.observed));
    static_assert(sym::camera_to_province.province_id == sym::province_base_tax.province_id &&
                  sym::camera_to_province.map_part == sym::province_is_sea.static_part);

    // The members only Anvil knows: call when the plugin is loading.
    static NavigationLayout Load() {
        NavigationLayout l;
        l.camera_rva = RvaOf(sym::camera_to_province_id);
        l.views_open_rva = RvaOf(sym::views_open);
        l.close_views_rva = RvaOf(sym::close_views);
        l.select_rva = RvaOf(sym::select_province);
        l.tutorial_blocks_rva = RvaOf(sym::tutorial_blocks);
        return l;
    }
};

// Main thread only, in game. Reads everything a navigation call checks first;
// unknown_province outside 1 <= id < count, read_failed when a read faults.
WorldFailure ReadNavigationFacts(std::uintptr_t image_base, const NavigationLayout& layout, std::int32_t id,
                                 NavigationFacts& out);

// Main thread only, right after ReadNavigationFacts in the same job. `open`:
// close the open views, move the camera and select the province (the religion
// view's province link, 0xFF6350); otherwise the camera only. False when the
// game's code raised a structured exception (`fault_code`).
bool CallNavigation(std::uintptr_t image_base, const NavigationLayout& layout, bool open, const NavigationFacts& facts,
                    unsigned long& fault_code);

// Main thread only, in game.
WorldFailure ReadCamera(std::uintptr_t image_base, const NavigationLayout& layout, CameraRaw& out);

}  // namespace tongs
