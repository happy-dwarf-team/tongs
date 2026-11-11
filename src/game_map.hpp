// The object under a point of the map (`cursor`, docs/design.md "Cursor (0.32.0)").
#pragma once

#include "cursor.hpp"

#include <cstdint>
#include <string>

namespace tongs {

struct MapLayout {
    std::uint32_t root_rva = 0;       // root_and_date
    std::uint32_t session = 0;        // root: the session (session_of_root)
    std::uint32_t graphical_map = 0;  // session: the map holder (map_of_session)
    std::uint32_t map = 0;            // holder: the graphical map
    std::uint32_t map_vtable_rva = 0;
    std::uint32_t province_rva = 0;   // province_from_point
    std::uint32_t pick_rva = 0;       // selectable_at_point
    std::uint32_t unit_id = 0;        // unit_id.offset
    std::uint32_t unit_country = 0;   // unit_country.country
    static MapLayout Load();
};

// Empty on success; "no_map" when the session has no graphical map yet; otherwise
// why the read failed (a guard or a fault).
std::string PickAtPoint(std::uintptr_t image_base, const MapLayout& layout, int x, int y, MapPickRaw& out);

}  // namespace tongs
