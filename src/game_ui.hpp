#pragma once

#include "game_symbols.hpp"
#include "ui_definitions.hpp"
#include "ui_tree.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tongs {

// How the widget tree reader treats one class of widget, told apart by the
// vtable at the widget pointer (the GUI classes have no RTTI).
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

// Where EU4 1.37.5 keeps its widgets (docs/design.md, "Widget tree"), from
// plugin/symbols.toml. RVAs are relative to eu4.exe; offsets are from the
// widget pointer the registry holds, except "full" ones, which start
// window_part before it (the whole window object).
struct UiLayout {
    unsigned manager_rva = U32(sym::gui_manager_store.manager);  // GUI manager pointer
    unsigned registry_begin = U32(sym::widget_registry.begin);    // mgr: std::vector<widget*> of every live widget
    unsigned registry_end = U32(sym::widget_registry.end);
    unsigned dead = U32(sym::render_pick.dead);                   // byte: waiting for the registry sweep
    unsigned shown = U32(sym::render_pick.shown);                 // byte: visible (children of a hidden window may keep it)
    unsigned slot_position = 0xD8;  // not in symbols.toml: vtable slots writing two int32, position in client pixels
    unsigned slot_size = 0xE0;      // not in symbols.toml: and size
    unsigned slot_name = U32(sym::listbox_find_child.name_slot);  // returns const std::string*
    unsigned button_state = U32(sym::button_state.state);
    unsigned hovered = U32(sym::hover_callback.hovered);          // mgr: the topmost widget under the GUI pointer, written once per frame

    // Stale widgets (ui_definitions.hpp): the type registry, the definition
    // classes with nested maps or pointer fields, each widget's definition, and the names
    // windows and containers keep themselves. Vtables here are RVAs.
    unsigned type_registry = U32(sym::row_type_lookup.registry);  // mgr: the name -> top-level definition tree
    TypeTreeLayout type_tree = {
        U32(sym::type_tree_insert.root),
        U32(sym::type_tree_node.size),
        0,  // the value: the insert stores it at the node itself
        U32(sym::type_tree_node.lower),
        U32(sym::type_tree_node.higher),
        U32(sym::type_tree_node.next),
        U32(sym::window_type_definition.map_vtable),
        {{U32(sym::window_type_definition.vtable), U32(sym::window_type_definition.map)},
         {U32(sym::eu3dialog_definition.vtable), U32(sym::window_type_definition.map)},
         {U32(sym::listbox_definition.vtable), U32(sym::listbox_definition_map.map)},
         {U32(sym::container_definition.vtable), U32(sym::container_definition_map.map)},
         {U32(sym::scrollbar_definition.vtable), U32(sym::scrollbar_definition_map.map)}},
        // Button definitions an extended scroll bar owns (its destructor deletes
        // them) and a container's background (never freed).
        {{U32(sym::extended_scrollbar_definition.vtable), U32(sym::extended_scrollbar_definition.background),
          U32(sym::button_definition.vtable)},
         {U32(sym::extended_scrollbar_definition.vtable), U32(sym::extended_scrollbar_definition.track),
          U32(sym::button_definition.vtable)},
         {U32(sym::extended_scrollbar_definition.vtable), U32(sym::extended_scrollbar_definition.slider),
          U32(sym::button_definition.vtable)},
         {U32(sym::extended_scrollbar_definition.vtable), U32(sym::extended_scrollbar_definition.increase),
          U32(sym::button_definition.vtable)},
         {U32(sym::extended_scrollbar_definition.vtable), U32(sym::extended_scrollbar_definition.decrease),
          U32(sym::button_definition.vtable)},
         {U32(sym::container_definition.vtable), U32(sym::container_background.background),
          U32(sym::button_definition.vtable)}}};
    static_assert(sym::window_type_definition.map_vtable == sym::listbox_definition_map.map_vtable &&
                  sym::window_type_definition.map_vtable == sym::container_definition.map_vtable &&
                  sym::window_type_definition.map_vtable == sym::scrollbar_definition_map.map_vtable);
    unsigned definition = U32(sym::window_name_slot.definition);  // the definition the widget was built from
    static_assert(sym::window_name_slot.definition == sym::container_name_slot.definition);
    unsigned window_override = U32(sym::window_name_slot.override);        // std::string, empty when not set
    unsigned container_override = U32(sym::container_name_slot.override);  // std::string, empty when not set

    unsigned window_part = U32(sym::eu3dialog_vtable.window_part);  // full window object -> widget pointer
    // windowType children: ten vector pairs {pointers, lower-cased names} from
    // the widget pointer, then (full object) seven named maps with an insertion
    // list of entries {value, std::string name}, and sub-windows
    // {std::string name; window*}.
    unsigned window_pairs = U32(sym::window_find_child.pairs);
    unsigned pair_names = U32(sym::window_find_child.names) - U32(sym::window_find_child.pairs);  // the names vector
    static_assert(sym::window_find_child.names - sym::window_find_child.pairs ==
                  sym::container_find_child.names - sym::container_find_child.pairs);
    unsigned name_size = 0x20;       // not in symbols.toml: a std::string in the names vector
    unsigned map_entry_name = 0x08;  // not in symbols.toml
    unsigned window_pair_count = 10;    // not in symbols.toml
    unsigned window_maps = 0x798;       // not in symbols.toml (full object)
    unsigned window_map_count = 7;      // not in symbols.toml
    unsigned map_stride = 0x40;         // not in symbols.toml
    unsigned map_list_head = 0x20;      // not in symbols.toml: first list node {entry*, prev, next}; the entry holds the value at +0
    unsigned list_node_next = U32(sym::listbox_find_child.next);
    unsigned window_subwindows = 0x958;  // not in symbols.toml (full object)
    unsigned subwindow_size = 0x28;      // not in symbols.toml
    unsigned subwindow_pointer = 0x20;   // not in symbols.toml
    unsigned subwindow_name = 0;         // not in symbols.toml
    // containerWindowType children: fifteen vector pairs from the widget pointer.
    unsigned container_pairs = U32(sym::container_find_child.pairs);
    unsigned container_pair_count = 15;  // not in symbols.toml
    unsigned pair_stride = 0x30;         // not in symbols.toml
    // List boxes: an insertion list of items. Find child takes the list object
    // `list_object` (-8) before the address listbox_items computes, and the
    // object's slot gives its head at +head. The item holds the full object of
    // its window. The scroll bar is a separate pointer.
    unsigned list_items =
        Displaced(sym::listbox_items.list, S8(sym::listbox_find_child.list_object)) + U32(sym::type_name_slot.head);
    unsigned listbox_item_window = U32(sym::listbox_find_child.item_window);
    unsigned listbox_scrollbar = U32(sym::listbox_scrollbar.scrollbar);
    unsigned smooth_item_window = U32(sym::smooth_listbox_find_child.item_window);
    unsigned smooth_scrollbar = U32(sym::smooth_listbox_scrollbar.scrollbar);
    // Overlapping elements boxes: the same item list through the object at
    // +list_object, whose slot list_slot is type_name_slot (item_list_head_slot),
    // and no scroll bar.
    unsigned overlapping_items = U32(sym::overlapping_box_find_child.list_object) + U32(sym::type_name_slot.head);
    unsigned overlapping_item_window = U32(sym::overlapping_box_find_child.item_window);
    // Grid boxes: a std::vector of item pointers; each item holds the full
    // object of its window. The scroll bar is a separate pointer.
    unsigned grid_items = U32(sym::grid_box_show.items);
    static_assert(sym::grid_box_show.items_end == sym::grid_box_show.items + 8);
    unsigned grid_item_window = U32(sym::grid_item_window.window);
    unsigned grid_scrollbar = U32(sym::grid_box_show.scrollbar);
    // Scroll bars hold their four buttons: slider, track, down, up.
    unsigned scrollbar_parts[4] = {U32(sym::scrollbar_slider_up.slider), U32(sym::scrollbar_down_track.track),
                                   U32(sym::scrollbar_down_track.down), U32(sym::scrollbar_slider_up.up)};
    // Render nodes: the owning widget and the parent node. Widgets the game's
    // own code holds (not a window) are placed under the owner of the parent node.
    unsigned node_owner = U32(sym::render_pick.node_owner);
    unsigned node_parent = 0x110;  // not in symbols.toml

    // The vtables, from each class's constructor, and where each class keeps
    // its render node (read by its slot 0xD8) and its text.
    std::vector<UiKind> kinds = {
        {U32(sym::window_vtable.vtable), UiType::Window, U32(sym::window_render_node.node) - window_part, 0, false,
         UiKind::Children::Window},
        {U32(sym::eu3dialog_vtable.vtable), UiType::Window, U32(sym::window_render_node.node) - window_part, 0, false,
         UiKind::Children::Window},
        {U32(sym::container_vtable.vtable), UiType::Window, U32(sym::container_render_node.node), 0, false,
         UiKind::Children::Container},
        {U32(sym::gui_button_vtable.vtable), UiType::Button, U32(sym::button_render_node.node),
         U32(sym::button_set_text.text), true},
        {U32(sym::button_vtable.vtable), UiType::Button, U32(sym::button_render_node.node), U32(sym::button_set_text.text),
         true},
        {U32(sym::slider_vtable.vtable), UiType::Button, U32(sym::slider_render_node.node)},
        {U32(sym::track_vtable.vtable), UiType::Button, U32(sym::track_render_node.node)},
        {U32(sym::icon_type_vtable.vtable), UiType::Icon, U32(sym::icon_render_node.node)},
        {U32(sym::icon_vtable.vtable), UiType::Icon, U32(sym::icon_render_node.node)},
        {U32(sym::instant_text_vtable.vtable), UiType::Text, U32(sym::instant_text_render_node.node),
         U32(sym::instant_text_set_text.text)},
        {U32(sym::text_box_destructor.vtable), UiType::Text, U32(sym::text_box_render_node.node),
         U32(sym::text_box_destructor.text)},
        // The edit box's text at +0xF8 is not in symbols.toml.
        {U32(sym::edit_box_vtable.vtable), UiType::Editbox, U32(sym::edit_box_render_node.node), 0xF8},
        // The check box's ticked byte at +0x100 is not in symbols.toml.
        {U32(sym::checkbox_vtable.vtable), UiType::Checkbox, U32(sym::checkbox_render_node.node), 0, false,
         UiKind::Children::None, 0x100},
        {U32(sym::listbox_vtable.vtable), UiType::Listbox, U32(sym::listbox_render_node.node), 0, false,
         UiKind::Children::Listbox},
        {U32(sym::smooth_listbox_vtable.vtable), UiType::Listbox, U32(sym::smooth_listbox_render_node.node), 0, false,
         UiKind::Children::SmoothListbox},
        {U32(sym::overlapping_box_vtable.vtable), UiType::Listbox, U32(sym::overlapping_box_render_node.node), 0, false,
         UiKind::Children::Overlapping},
        {U32(sym::grid_box_vtable.vtable), UiType::Listbox, U32(sym::grid_box_render_node.node), 0, false,
         UiKind::Children::Grid},
        {U32(sym::scrollbar_vtable.vtable), UiType::Other, 0, 0, false, UiKind::Children::Scrollbar},
    };
};

// Reads the widget tree. Main thread only: the game deletes widgets in its GUI
// update and a reload frees definitions. Every read and call is guarded, and
// slot 0xA0 is called only on widgets whose definition the type registry
// holds; returns an error text, or empty on success.
std::string ReadUiTree(std::uintptr_t image_base, const UiLayout& layout, UiTree& out);

// Main thread: the widget the game hovers (mgr+0x130), 0 when none or unreadable.
std::uintptr_t ReadHoveredWidget(std::uintptr_t image_base, const UiLayout& layout);

}  // namespace tongs
