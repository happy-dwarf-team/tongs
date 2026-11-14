#include "game_world.hpp"

#include "game_memory.hpp"

namespace tongs {
namespace {

WorldFailure Failed(std::string detail) { return {"read_failed", std::move(detail)}; }

bool ReadRoot(std::uintptr_t base, const WorldLayout& l, std::uintptr_t& root) {
    return Read(base + l.root_rva, root) && root != 0;
}

}  // namespace

WorldFailure ReadSelection(std::uintptr_t base, const WorldLayout& l, SelectionRaw& out) {
    out = {};
    std::uintptr_t root = 0, session = 0, holder = 0;
    if (!ReadRoot(base, l, root)) return {"not_in_game", "no game loaded (root object is null)"};
    if (!Read(root + l.session, session) || session == 0 || !Read(session + l.holder, holder)) {
        return Failed("the session is not readable");
    }
    if (holder == 0) return {};
    std::uintptr_t selected = 0, view = 0, shown = 0, window = 0, panel = 0, colonisation = 0;
    if (!Read(holder + l.selected, selected) || !Read(holder + l.view, view)) return Failed("the selection is not readable");
    if (selected != 0 && !Read(selected + l.province_id, out.last_selected)) {
        return Failed("the selected province is not readable");
    }
    if (view != 0) {
        if (!Read(view + l.view_province, shown) || !Read(view + l.view_window, window)) {
            return Failed("the province view is not readable");
        }
        if (shown != 0 && !Read(shown + l.province_id, out.view_province)) {
            return Failed("the province the view shows is not readable");
        }
        if (window != 0) out.view_widget = window + l.window_part;
        if (Read(view + l.view_colonisation, panel) && panel != 0 && Read(panel + l.colonisation_window, colonisation) &&
            colonisation != 0) {
            out.colonisation_widget = colonisation + l.window_part;
        }
    }
    return {};
}

}  // namespace tongs
