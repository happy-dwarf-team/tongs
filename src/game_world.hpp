#pragma once

#include "game_symbols.hpp"
#include "world.hpp"

#include <cstdint>

namespace tongs {

// Where EU4 1.37.5 keeps the player's tags and the province selection, and
// the province and country fields navigation reads for its own checks
// (docs/design.md, "World reads (0.20.0)"), from plugin/symbols.toml. The values of
// `province`, `country` and `player` come from anvil-game.world instead
// (world_reads.hpp). RVAs are relative to eu4.exe; offsets are from the object
// named in each comment.
struct WorldLayout {
    std::uint32_t root_rva = Value(sym::root_and_date, "root");
    std::uint32_t province_id = U32(sym::province_base_tax.province_id);      // province: int32
    std::uint32_t owner = U32(sym::province_base_tax.owner);                  // province: tag

    // Tags: three characters, the country index (int16) above country_index_shift,
    // the kind byte and the valid byte, as the player's tags show them.
    std::uint32_t country_index_shift = U32(sym::province_base_tax.country_index_shift);
    std::uint32_t tag_kind = U32(sym::player_country.player_kind - sym::player_country.player);
    std::uint32_t tag_valid = U32(sym::player_country.observed_valid - sym::player_country.observed);
    std::uint32_t observer_kind = U32(sym::player_country.observer_kind);

    std::uint32_t countries_db_rva = Value(sym::player_country, "countries_db");  // pointer to the country database
    std::uint32_t countries = U32(sym::player_country.countries);                 // db: Country*[], by country index

    std::uint32_t player = U32(sym::player_country.player);      // root: tag
    std::uint32_t observed = U32(sym::player_country.observed);  // root: tag

    std::uint32_t session = U32(sym::session_of_root.session);    // root: the session
    std::uint32_t holder = Value(sym::selection_holder, "holder");      // session: the selection object
    std::uint32_t selected = 0x00;  // selection: the province, the `mov [rbx],rsi` of selected_province
    std::uint32_t view = U32(sym::selection_view.view);            // selection: the province view
    std::uint32_t view_province = U32(sym::province_view_province.shown);  // view: the province it shows
    std::uint32_t view_window = U32(sym::view_window.window);              // view: its window's full object
    std::uint32_t view_colonisation = U32(sym::province_view_colonisation.panel);     // view: the colonisation panel's holder
    std::uint32_t colonisation_window = U32(sym::province_view_colonisation.window);  // holder: the panel's full object
    std::uint32_t window_part = U32(sym::eu3dialog_vtable.window_part);    // full window object -> widget pointer

    static_assert(sym::player_country.countries == sym::province_base_tax.countries);
    static_assert(sym::selected_province.province_id == sym::province_base_tax.province_id &&
                  sym::view_window.view == sym::selection_view.view &&
                  sym::province_view_colonisation.shown == sym::render_pick.shown + sym::eu3dialog_vtable.window_part);

    // Checks the values that differ between the game builds: call when the plugin is loading.
    static WorldLayout Load() {
        WorldLayout l;
        Require(Value(sym::province_base_tax, "countries_db") == l.countries_db_rva, "the country database");
        return l;
    }
};

// Main thread only, in game (the caller checked the state on the same frame).
// Every read is guarded: a fault answers read_failed and costs only this answer.
WorldFailure ReadSelection(std::uintptr_t image_base, const WorldLayout& layout, SelectionRaw& out);

}  // namespace tongs
