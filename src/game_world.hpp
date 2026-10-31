#pragma once

#include "game_symbols.hpp"
#include "world.hpp"

#include <cstdint>
#include <string_view>

namespace tongs {

// Where EU4 1.37.5 keeps its provinces, countries, the player and the province
// selection (docs/design.md, "World reads (0.20.0)"), from plugin/symbols.toml.
// RVAs are relative to eu4.exe; offsets are from the object named in each comment.
struct WorldLayout {
    std::uint32_t root_rva = U32(sym::province_count.root);
    std::uint32_t provinces = U32(sym::province_count.provinces);          // root: vector of Province*, entry 0 included
    std::uint32_t provinces_end = U32(sym::province_count.provinces_end);
    std::uint32_t pointer_shift = U32(sym::province_count.pointer_shift);  // 8-byte entries
    std::uint32_t province_array = U32(sym::province_is_sea.province_array);  // root: the array, indexed by ID
    std::uint32_t province_size = U32(sym::province_is_sea.province_size);
    std::uint32_t province_id = U32(sym::province_base_tax.province_id);     // int32
    std::uint32_t owner = U32(sym::province_base_tax.owner);                 // tag
    std::uint32_t controller = U32(sym::province_controller.controller);     // tag
    std::uint32_t cores = U32(sym::province_cores.cores);                    // std::vector of tags: begin
    std::uint32_t cores_end = U32(sym::province_cores.cores_end);            // and end, from the vector
    std::uint32_t base_tax = U32(sym::province_base_tax.base_tax);           // int32, x 1000
    std::uint32_t base_production = U32(sym::province_base_production.base_production);
    std::uint32_t base_manpower = U32(sym::province_base_manpower.base_manpower);
    std::uint32_t static_part = U32(sym::province_is_sea.static_part);       // pointer to the map part
    std::uint32_t name = U32(sym::province_name.name);                       // map part: std::string
    std::uint32_t land_flags = U32(sym::province_is_sea.land_flags);         // map part: byte, land_bit set on land
    std::uint32_t land_bit = U32(sym::province_is_sea.land_bit);
    std::uint32_t wasteland_flags = U32(sym::province_is_wasteland.wasteland_flags);
    std::uint32_t wasteland_bit = U32(sym::province_is_wasteland.wasteland_bit);

    // Tags: three characters, the country index (int16) above country_index_shift,
    // the kind byte and the valid byte, as the player's tags show them.
    std::uint32_t country_index_shift = U32(sym::province_base_tax.country_index_shift);
    std::uint32_t tag_kind = U32(sym::player_country.player_kind - sym::player_country.player);
    std::uint32_t tag_valid = U32(sym::player_country.observed_valid - sym::player_country.observed);
    std::uint32_t observer_kind = U32(sym::player_country.observer_kind);

    std::uint32_t countries_db_rva = U32(sym::province_base_tax.countries_db);  // pointer to the country database
    std::uint32_t countries = U32(sym::province_base_tax.countries);            // db: Country*[], by country index
    std::uint32_t buckets = U32(sym::tag_buckets.buckets);                      // db + n*stride: tags of bucket n
    std::uint32_t bucket_count = U32(sym::tag_buckets.bucket_count);            // db + n*stride: int32
    std::uint32_t bucket_mask = U32(sym::tag_buckets.bucket_mask);
    std::uint32_t bucket_stride = 0x18;  // not in symbols.toml: lea rcx,[rax+rax*2]; lea r15,[r10+rcx*8] in tag_buckets' bytes
    std::uint32_t compared = U32(sym::tag_buckets.compared);                    // characters compared
    std::uint32_t country_tag = U32(sym::country_exists.kind - (sym::player_country.player_kind - sym::player_country.player));
    std::uint32_t capital = U32(sym::province_base_tax.capital);                // country: int32 province ID
    std::uint32_t owned = U32(sym::country_provinces.owned);                    // country: int32*
    std::uint32_t owned_count = U32(sym::country_provinces.owned_count);        // country: int32

    std::uint32_t player = U32(sym::player_country.player);      // root: tag
    std::uint32_t observed = U32(sym::player_country.observed);  // root: tag

    std::uint32_t session = U32(sym::session_of_root.session);    // root: the session
    std::uint32_t holder = U32(sym::selection_holder.holder);      // session: the selection object
    std::uint32_t selected = 0x00;  // selection: the province, the `mov [rbx],rsi` of selected_province
    std::uint32_t view = U32(sym::selection_view.view);            // selection: the province view
    std::uint32_t view_province = U32(sym::province_view_province.shown);  // view: the province it shows
    std::uint32_t view_window = U32(sym::view_window.window);              // view: its window's full object
    std::uint32_t view_colonisation = U32(sym::province_view_colonisation.panel);     // view: the colonisation panel's holder
    std::uint32_t colonisation_window = U32(sym::province_view_colonisation.window);  // holder: the panel's full object
    std::uint32_t window_part = U32(sym::eu3dialog_vtable.window_part);    // full window object -> widget pointer

    static_assert(sym::province_count.root == sym::root_and_date.root && sym::province_is_sea.root == sym::root_and_date.root &&
                  sym::province_is_wasteland.root == sym::root_and_date.root);
    static_assert(sym::province_is_wasteland.province_size == sym::province_is_sea.province_size &&
                  sym::province_is_wasteland.province_array == sym::province_is_sea.province_array &&
                  sym::province_is_wasteland.static_part == sym::province_is_sea.static_part &&
                  sym::province_name.static_part == sym::province_is_sea.static_part);
    static_assert(sym::player_country.countries_db == sym::province_base_tax.countries_db &&
                  sym::country_exists.countries_db == sym::province_base_tax.countries_db &&
                  sym::player_country.countries == sym::province_base_tax.countries &&
                  sym::country_exists.countries == sym::province_base_tax.countries);
    static_assert(sym::country_exists.owned_count == sym::country_provinces.owned_count &&
                  sym::country_exists.observer_kind == sym::player_country.observer_kind);
    static_assert(sym::selected_province.province_id == sym::province_base_tax.province_id &&
                  sym::view_window.view == sym::selection_view.view &&
                  sym::province_view_colonisation.shown == sym::render_pick.shown + sym::eu3dialog_vtable.window_part);
    static_assert(sym::tag_buckets.bucket_mask + 1 == kTagBuckets);
};

// Main thread only, in game (the caller checked the state on the same frame).
// Every read is guarded: a fault or a failed guard (an entry's own ID, a
// country's own tag) answers read_failed and costs only this answer.
WorldFailure ReadProvince(std::uintptr_t image_base, const WorldLayout& layout, std::int32_t id, ProvinceRaw& out);
WorldFailure ReadCountry(std::uintptr_t image_base, const WorldLayout& layout, std::string_view tag, CountryRaw& out);
WorldFailure ReadPlayer(std::uintptr_t image_base, const WorldLayout& layout, PlayerRaw& out);
WorldFailure ReadSelection(std::uintptr_t image_base, const WorldLayout& layout, SelectionRaw& out);

}  // namespace tongs
