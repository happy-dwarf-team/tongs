#pragma once

#include "ui_definitions.hpp"
#include "ui_tree.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tongs {

// Where widgets sit in the tree (docs/design.md, "Widget tree"): the registry
// of live widgets, the child containers of each class, and the render nodes
// that place widgets the game's own code holds. The whole-tree read
// (game_ui.cpp) and the hovered widget's climb (tongs.ui) both place widgets
// with this code. It reads game memory only through ReadMemory, so the tests
// build a GUI in their own memory.

// How the reader treats one class of widget, told apart by the vtable at the
// widget pointer (the GUI classes have no RTTI).
struct UiKind {
    enum class Children { None, Window, Container, Listbox, SmoothListbox, Overlapping, Grid, Scrollbar };
    unsigned vtable = 0;  // RVA
    UiType type = UiType::Other;
    unsigned node = 0;    // the widget's render node pointer; 0 = none
    unsigned text = 0;    // std::string shown by the widget; 0 = none
    bool state = false;   // int32 state at UiLayout::button_state, 3 = disabled
    Children children = Children::None;
    unsigned checked = 0;  // byte: the check box is ticked; 0 = none
};

// The offsets of the registry, the child containers and the render nodes;
// game_ui.hpp's UiLayout fills them from plugin/symbols.toml. Offsets are from
// the widget pointer the registry holds, except "full" ones, which start
// window_part before it (the whole window object).
struct UiStructureLayout {
    unsigned registry_begin = 0;  // mgr: std::vector<widget*> of every live widget
    unsigned registry_end = 0;
    unsigned dead = 0;         // byte: waiting for the registry sweep
    unsigned window_part = 0;  // full window object -> widget pointer
    // windowType children: vector pairs {pointers, lower-cased names} from the
    // widget pointer, then (full object) named maps with an insertion list of
    // entries {value, std::string name}, and sub-windows {std::string name; window*}.
    unsigned window_pairs = 0;
    unsigned pair_names = 0;  // the names vector, from the pair
    unsigned name_size = 0;   // a std::string in the names vector
    unsigned map_entry_name = 0;
    unsigned window_pair_count = 0;
    unsigned window_maps = 0;  // full object
    unsigned window_map_count = 0;
    unsigned map_stride = 0;
    unsigned map_list_head = 0;  // first list node {entry*, prev, next}; the entry holds the value at +0
    unsigned list_node_next = 0;
    unsigned window_subwindows = 0;  // full object
    unsigned subwindow_size = 0;
    unsigned subwindow_pointer = 0;
    unsigned subwindow_name = 0;
    // containerWindowType children: vector pairs from the widget pointer.
    unsigned container_pairs = 0;
    unsigned container_pair_count = 0;
    unsigned pair_stride = 0;
    // List boxes: an insertion list of items whose item holds the full object
    // of its window; the scroll bar is a separate pointer.
    unsigned list_items = 0;
    unsigned listbox_item_window = 0;
    unsigned listbox_scrollbar = 0;
    unsigned smooth_item_window = 0;
    unsigned smooth_scrollbar = 0;
    // Overlapping elements boxes: the same item list, no scroll bar.
    unsigned overlapping_items = 0;
    unsigned overlapping_item_window = 0;
    // Grid boxes: a std::vector of item pointers; each item holds the full
    // object of its window. The scroll bar is a separate pointer.
    unsigned grid_items = 0;
    unsigned grid_item_window = 0;
    unsigned grid_scrollbar = 0;
    // Scroll bars hold their four buttons: slider, track, down, up.
    unsigned scrollbar_parts[4] = {};
    // Render nodes: the owning widget and the parent node.
    unsigned node_owner = 0;
    unsigned node_parent = 0;
    std::vector<UiKind> kinds;
};

// A widget a holder keeps, and where the holder stores its name (a std::string; 0: nowhere).
struct UiHeld {
    std::uintptr_t pointer = 0;
    std::uintptr_t name = 0;
};

// What a widget's containers hold, in their order. Not every pointer is a live widget.
std::vector<UiHeld> HeldBy(std::uintptr_t widget, const UiKind& kind, const UiStructureLayout& l, const ReadMemory& read);

struct UiEntry {
    std::uintptr_t pointer = 0;
    const UiKind* kind = nullptr;  // null: a class Tongs does not know
    int parent = -1;
    std::uintptr_t held_name = 0;  // the std::string a window's or container's child list keeps for it
};

constexpr std::size_t kMaxUiWidgets = 200000;  // the registry reserves 64 000

// The registry's live widgets, placed: each under the first widget whose
// containers hold it, else under the owner of its render node's parent (a
// widget the game's own code holds); what is left are the top-level windows,
// in registry order.
class UiStructure {
public:
    // `image_base` turns vtables into the RVAs of `layout.kinds`.
    UiStructure(const UiStructureLayout& layout, std::uintptr_t image_base, ReadMemory read);

    // Every widget of the registry at `manager`, placed. Returns an error text, or empty.
    std::string ReadAll(std::uintptr_t manager);

    // Only `widget` and its ancestors, as ReadAll would place them, from the
    // widget up: entries()[0] is the widget, each entry's parent the next one.
    // A few reads per level instead of the whole registry; empty (no error)
    // when `widget` is not a live widget of the registry. It can differ from
    // ReadAll where a widget is held twice (ReadAll takes the first holder in
    // registry order, the climb the one under its render node's owner).
    // complete() is false when the climb stopped at a widget whose render node
    // it cannot read (a class Tongs does not know): ReadAll may still place
    // that one under a holder.
    std::string Climb(std::uintptr_t manager, std::uintptr_t widget);
    bool complete() const { return complete_; }

    // After Climb: the live widget of the registry at a pointer (or the full
    // object of a window), found by a scan, and its kind; false when there is none.
    bool LiveWidget(std::uintptr_t p, std::uintptr_t& widget, const UiKind*& kind) const;
    const UiKind* KindOf(std::uintptr_t widget) const;

    const std::vector<UiEntry>& entries() const { return entries_; }
    const std::vector<std::vector<int>>& children() const { return children_; }
    const std::vector<int>& roots() const { return roots_; }
    const UiStructureLayout& layout() const { return l_; }
    const ReadMemory& read() const { return read_; }

private:
    std::string CopyRegistry(std::uintptr_t manager);
    int Find(std::uintptr_t p) const;
    void Adopt(int parent, int child, const UiHeld* held);
    // The owner of the first render node above the widget's own that has a
    // live owner other than the widget; 0 when none.
    std::uintptr_t NodeOwnerAbove(std::uintptr_t widget, const UiKind* kind) const;
    bool Holds(std::uintptr_t holder, const UiKind* kind, std::uintptr_t widget, UiHeld& held) const;
    // The holder of `widget` at or under `owner` (a box or scroll bar `owner`
    // holds); `owner` itself when nothing there holds it.
    std::uintptr_t HolderUnder(std::uintptr_t owner, std::uintptr_t widget, UiHeld& held) const;

    const UiStructureLayout& l_;
    std::uintptr_t base_;
    ReadMemory read_;
    std::unordered_map<unsigned, const UiKind*> kinds_;
    std::vector<std::uintptr_t> registry_;
    std::vector<UiEntry> entries_;
    std::vector<std::vector<int>> children_;
    std::vector<int> roots_;
    std::unordered_map<std::uintptr_t, int> index_;  // ReadAll only
    bool complete_ = true;  // Climb only
};

}  // namespace tongs
