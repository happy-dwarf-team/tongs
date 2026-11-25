#pragma once

#include "game_symbols.hpp"
#include "ui_definitions.hpp"
#include "ui_structure.hpp"
#include "ui_tree.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tongs {

// Where EU4 1.37.5 keeps its widgets (docs/design.md, "Widget tree"), from
// plugin/symbols.toml. RVAs are relative to eu4.exe; offsets are from the
// widget pointer the registry holds, except "full" ones, which start
// window_part before it (the whole window object). The base holds what places
// widgets in the tree (ui_structure.hpp); this adds what describes them.
struct UiLayout : UiStructureLayout {
    unsigned manager_rva = Value(sym::gui_manager_store, "manager");  // GUI manager pointer
    unsigned shown = U32(sym::render_pick.shown);                 // byte: visible (children of a hidden window may keep it)
    unsigned slot_position = 0xD8;  // not in symbols.toml: vtable slots writing two int32, position in client pixels
    unsigned slot_size = 0xE0;      // not in symbols.toml: and size
    unsigned slot_name = U32(sym::listbox_find_child.name_slot);  // returns const std::string*
    unsigned button_state = U32(sym::button_state.state);
    unsigned hovered = U32(sym::hover_callback.hovered);          // mgr: the topmost widget under the GUI pointer, written once per frame
    unsigned text_target = U32(sym::edit_focus.target);           // mgr: the edit box with the keyboard focus (edit box+0x1D0), 0 = none
    // How a click under the pointer goes (docs/design.md, "Clicks through widgets").
    unsigned graphics = U32(sym::gui_graphics.graphics);          // mgr: the graphics object
    unsigned gui_scale = U32(sym::gui_scale.scale);               // graphics: the GUI scale (float)
    unsigned under_pointer = U32(sym::mouse_collisions.list);     // graphics: {begin, end} of the widgets under the pointer
    unsigned under_pointer_shift = U32(sym::hover_order.entry_shift);  // an entry is 1 << this bytes
    unsigned under_pointer_owner = U32(sym::hover_order.owner);        // the widget in an entry
    unsigned pointer_over = U32(sym::hover_last_frame.last_frame);     // byte: under the pointer on the last frame
    unsigned takes_all_input = U32(sym::takes_all_input.flag);         // byte: consumes every mouse event
    unsigned click_left = U32(sym::button_clickable.left);             // button bytes: reacts to a left
    unsigned click_right = U32(sym::button_clickable.right);           // or a right click

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
        Value(sym::window_type_definition, "map_vtable"),
        {{Value(sym::window_type_definition, "vtable"), U32(sym::window_type_definition.map)},
         {Value(sym::eu3dialog_definition, "vtable"), U32(sym::window_type_definition.map)},
         {Value(sym::listbox_definition, "vtable"), U32(sym::listbox_definition_map.map)},
         {Value(sym::container_definition, "vtable"), U32(sym::container_definition_map.map)},
         {Value(sym::scrollbar_definition, "vtable"), U32(sym::scrollbar_definition_map.map)}},
        // Button definitions an extended scroll bar owns (its destructor deletes
        // them) and a container's background (never freed).
        {{Value(sym::extended_scrollbar_definition, "vtable"), U32(sym::extended_scrollbar_definition.background),
          Value(sym::button_definition, "vtable")},
         {Value(sym::extended_scrollbar_definition, "vtable"), U32(sym::extended_scrollbar_definition.track),
          Value(sym::button_definition, "vtable")},
         {Value(sym::extended_scrollbar_definition, "vtable"), U32(sym::extended_scrollbar_definition.slider),
          Value(sym::button_definition, "vtable")},
         {Value(sym::extended_scrollbar_definition, "vtable"), U32(sym::extended_scrollbar_definition.increase),
          Value(sym::button_definition, "vtable")},
         {Value(sym::extended_scrollbar_definition, "vtable"), U32(sym::extended_scrollbar_definition.decrease),
          Value(sym::button_definition, "vtable")},
         {Value(sym::container_definition, "vtable"), U32(sym::container_background.background),
          Value(sym::button_definition, "vtable")}}};
    unsigned definition = U32(sym::window_name_slot.definition);  // the definition the widget was built from
    static_assert(sym::window_name_slot.definition == sym::container_name_slot.definition &&
                  sym::window_name_slot.definition == sym::widget_name_slot.definition);
    unsigned window_override = U32(sym::window_name_slot.override);        // std::string, empty when not set
    unsigned container_override = U32(sym::container_name_slot.override);  // std::string, empty when not set
    // What slot 0xA0 returns, read without the call (the cheap hovered):
    // every definition class's slot 0x40 returns the std::string at
    // definition_name; windows and containers first return their own name.
    unsigned definition_name = U32(sym::type_name_slot.name);
    // The game path of the .gui file a definition was read from (std::string).
    unsigned definition_file = U32(sym::guibounds_definition_file.file);
    static_assert(sym::guibounds_definition_file.definition == sym::window_name_slot.definition);
    // The line of the `{` that opens the definition's block in that file (int).
    unsigned definition_line = U32(sym::guibounds_definition_line.line);
    static_assert(sym::guibounds_definition_line.definition == sym::window_name_slot.definition);
    static_assert(sym::widget_name_slot.name_slot == 0x40);
    std::uint32_t widget_name_slot_rva = 0;     // RvaOf(sym::widget_name_slot), set when loading
    std::uint32_t window_name_slot_rva = 0;     // RvaOf(sym::window_name_slot)
    std::uint32_t container_name_slot_rva = 0;  // RvaOf(sym::container_name_slot)

    // Sizes that slot 0xE0 gets wrong (docs/design.md, "Widget sizes"). A
    // list box's slot 0xE0 sums all its rows; its layout keeps the extent of
    // the shown rows. A sprite node's size divides the sprite's by its frames,
    // which a masked shield's sprite has for its overlay strip: its own size
    // is the shield's.
    unsigned listbox_extent =
        Displaced(sym::listbox_shown_extent.extent, S8(sym::listbox_find_child.list_object));  // two int32
    unsigned listbox_vertical =
        Displaced(sym::listbox_shown_extent.vertical, S8(sym::listbox_find_child.list_object));  // byte
    unsigned node_size_slot = U32(sym::button_size.size_slot);
    static_assert(sym::button_size.node == sym::button_render_node.node);
    std::uint32_t sprite_node_size_rva = 0;  // RvaOf(sym::sprite_node_size), set when loading
    unsigned sprite_node_scale = U32(sym::sprite_node_size.scale);    // float
    unsigned sprite_node_sprite = U32(sym::sprite_node_size.sprite);  // the sprite
    unsigned masked_shield_vtable = Value(sym::masked_shield_vtable, "vtable");  // RVA
    unsigned masked_shield_size = U32(sym::masked_shield_size.width);       // two int32
    static_assert(sym::masked_shield_size.height == sym::masked_shield_size.width + 4);

    // What a button's, icon's or check box's render node draws (docs/design.md, "Sprites and
    // frames"): its object type, whose name is the sprite's, read through slot 0x278 only when
    // the slot is node_object_type; its frame through slot 0xB0 only when that is
    // node_super_state; and, when the node is a shield, the country whose flag it shows.
    unsigned node_type_slot = U32(sym::icon_sprite_name.type_slot);
    unsigned node_type = U32(sym::node_object_type.type);         // node: the object type (the sprite)
    unsigned object_type_name = U32(sym::icon_sprite_name.name);  // object type: its name (a string)
    unsigned node_frame_slot = U32(sym::icon_frame.frame_slot);
    unsigned node_frame = U32(sym::node_super_state.frame);       // node: int32, from 1 as a .gui's `frame`
    static_assert(sym::icon_sprite_name.node == sym::icon_render_node.node && sym::icon_frame.node == sym::icon_render_node.node);
    std::uint32_t node_type_getter_rva = 0;   // RvaOf(sym::node_object_type), set when loading
    std::uint32_t node_frame_getter_rva = 0;  // RvaOf(sym::node_super_state), set when loading
    unsigned shield_node_vtable = Value(sym::shield_object_vtable, "vtable");  // RVA
    unsigned shield_tag_index = U32(sym::shield_tag_index.tag_index);           // shield node: int32 country index
    unsigned countries_db_rva = Value(sym::country_own_tag, "countries_db");    // pointer to the country database
    unsigned countries = U32(sym::country_own_tag.countries);                   // db: Country*[], by country index
    // country: its own tag, 8 bytes (three characters and a NUL, the index, the kind, the valid byte); the
    // kind byte sits 6 into a tag, as in the player's tags (game_world.hpp).
    unsigned country_tag = U32(sym::country_own_tag.kind) - (U32(sym::player_country.player_kind) - U32(sym::player_country.player));
    static_assert(sym::country_own_tag.countries == sym::player_country.countries);

    // What the game's code draws in place of the sprite's own picture (docs/design.md, "Sprites and
    // frames"): the texture file an instant sprite (a modifier icon) was given, the rebel type a
    // shield shows, and the slices of a pie chart. Each is read only from a node of that class.
    unsigned instant_sprite_vtable = Value(sym::instant_sprite_vtable, "vtable");  // RVA
    unsigned instant_sprite_file = U32(sym::instant_sprite_file.file);              // node: a string, "" = its sprite's
    unsigned shield_custom = U32(sym::shield_custom_value.custom);  // shield node: int32, -(r+1) shows rebel type r
    unsigned country_count = U32(sym::shield_custom_value.count);   // country db: int32, the number of countries
    static_assert(sym::shield_custom_value.tag_index == sym::shield_tag_index.tag_index);
    unsigned rebel_types_rva = Value(sym::rebel_types, "instance");  // pointer to the rebel type database
    unsigned rebel_list = U32(sym::rebel_types.list);                // db: RebelType*[], {begin, end}
    unsigned rebel_list_end = U32(sym::rebel_types.list_end);
    unsigned rebel_name = U32(sym::rebel_type_tag.tag);     // rebel type: its name (a string), its flag's file name
    unsigned rebel_index = U32(sym::rebel_type_tag.index);  // rebel type: int32, its place in the list
    unsigned pie_chart_vtable = Value(sym::pie_chart_vtable, "vtable");  // RVA
    unsigned pie_slices = U32(sym::pie_slices.begin);  // node: the slices, {begin, end}
    unsigned pie_slices_end = U32(sym::pie_slices.end);
    unsigned slice_stride = U32(sym::pie_slice.stride);
    // slice: the percent, an int32 at +0 (not in symbols.toml: the render compares it without a
    // displacement), and the colour's floats
    unsigned slice_red = U32(sym::pie_slice.red);
    unsigned slice_green = U32(sym::pie_slice.green);
    unsigned slice_blue = U32(sym::pie_slice.blue);
    unsigned slice_alpha = U32(sym::pie_slice.alpha);

    UiLayout() {
        registry_begin = U32(sym::widget_registry.begin);
        registry_end = U32(sym::widget_registry.end);
        dead = U32(sym::render_pick.dead);
        window_part = U32(sym::eu3dialog_vtable.window_part);
        // windowType children: ten vector pairs {pointers, lower-cased names} from
        // the widget pointer, then (full object) seven named maps with an insertion
        // list of entries {value, std::string name}, and sub-windows
        // {std::string name; window*}.
        window_pairs = U32(sym::window_find_child.pairs);
        pair_names = U32(sym::window_find_child.names) - U32(sym::window_find_child.pairs);
        static_assert(sym::window_find_child.names - sym::window_find_child.pairs ==
                      sym::container_find_child.names - sym::container_find_child.pairs);
        name_size = 0x20;       // not in symbols.toml: a std::string in the names vector
        map_entry_name = 0x08;  // not in symbols.toml
        window_pair_count = 10;    // not in symbols.toml
        window_maps = 0x798;       // not in symbols.toml (full object)
        window_map_count = 7;      // not in symbols.toml
        map_stride = 0x40;         // not in symbols.toml
        map_list_head = 0x20;      // not in symbols.toml
        list_node_next = U32(sym::listbox_find_child.next);
        window_subwindows = 0x958;  // not in symbols.toml (full object)
        subwindow_size = 0x28;      // not in symbols.toml
        subwindow_pointer = 0x20;   // not in symbols.toml
        subwindow_name = 0;         // not in symbols.toml
        container_pairs = U32(sym::container_find_child.pairs);
        container_pair_count = 15;  // not in symbols.toml
        pair_stride = 0x30;         // not in symbols.toml
        // List boxes: find child takes the list object `list_object` (-8)
        // before the address listbox_items computes, and the object's slot
        // gives its head at +head. The item holds the full object of its window.
        list_items =
            Displaced(sym::listbox_items.list, S8(sym::listbox_find_child.list_object)) + U32(sym::type_name_slot.head);
        listbox_item_window = U32(sym::listbox_find_child.item_window);
        listbox_scrollbar = U32(sym::listbox_scrollbar.scrollbar);
        smooth_item_window = U32(sym::smooth_listbox_find_child.item_window);
        smooth_scrollbar = U32(sym::smooth_listbox_scrollbar.scrollbar);
        // Overlapping elements boxes: the same item list through the object at
        // +list_object, whose slot list_slot is type_name_slot (item_list_head_slot).
        overlapping_items = U32(sym::overlapping_box_find_child.list_object) + U32(sym::type_name_slot.head);
        overlapping_item_window = U32(sym::overlapping_box_find_child.item_window);
        grid_items = U32(sym::grid_box_show.items);
        static_assert(sym::grid_box_show.items_end == sym::grid_box_show.items + 8);
        grid_item_window = U32(sym::grid_item_window.window);
        grid_scrollbar = U32(sym::grid_box_show.scrollbar);
        scrollbar_parts[0] = U32(sym::scrollbar_slider_up.slider);
        scrollbar_parts[1] = U32(sym::scrollbar_down_track.track);
        scrollbar_parts[2] = U32(sym::scrollbar_down_track.down);
        scrollbar_parts[3] = U32(sym::scrollbar_slider_up.up);
        // Render nodes: the owning widget and the parent node. Widgets the game's
        // own code holds (not a window) are placed under the owner of the parent node.
        node_owner = U32(sym::render_pick.node_owner);
        node_parent = U32(sym::node_parents_draw_children.parent);
        // Whether a node draws its children: a window that the game never showed is
        // still drawn when its node does (docs/design.md, "Windows the game never shows").
        node_children_slot = U32(sym::node_parents_draw_children.children_slot);
        node_children = U32(sym::node_draws_children.flag);
        // The vtables, from each class's constructor, and where each class keeps
        // its render node (read by its slot 0xD8) and its text.
        kinds = {
            {Value(sym::window_vtable, "vtable"), UiType::Window, U32(sym::window_render_node.node) - window_part, 0, false,
             UiKind::Children::Window, 0, UiClickInput::Passes},
            {Value(sym::eu3dialog_vtable, "vtable"), UiType::Window, U32(sym::window_render_node.node) - window_part, 0,
             false, UiKind::Children::Window, 0, UiClickInput::Passes},
            {Value(sym::container_vtable, "vtable"), UiType::Window, U32(sym::container_render_node.node), 0, false,
             UiKind::Children::Container, 0, UiClickInput::Passes},
            {Value(sym::gui_button_vtable, "vtable"), UiType::Button, U32(sym::button_render_node.node),
             U32(sym::button_set_text.text), true, UiKind::Children::None, 0, UiClickInput::Button},
            {Value(sym::button_vtable, "vtable"), UiType::Button, U32(sym::button_render_node.node),
             U32(sym::button_set_text.text), true, UiKind::Children::None, 0, UiClickInput::Button},
            {Value(sym::slider_vtable, "vtable"), UiType::Button, U32(sym::slider_render_node.node)},
            {Value(sym::track_vtable, "vtable"), UiType::Button, U32(sym::track_render_node.node), 0, false,
             UiKind::Children::None, 0, UiClickInput::Button},
            {Value(sym::icon_type_vtable, "vtable"), UiType::Icon, U32(sym::icon_render_node.node), 0, false,
             UiKind::Children::None, 0, UiClickInput::Button},
            {Value(sym::icon_vtable, "vtable"), UiType::Icon, U32(sym::icon_render_node.node), 0, false,
             UiKind::Children::None, 0, UiClickInput::Button},
            {Value(sym::instant_text_vtable, "vtable"), UiType::Text, U32(sym::instant_text_render_node.node),
             U32(sym::instant_text_set_text.text), false, UiKind::Children::None, 0, UiClickInput::Passes},
            {Value(sym::text_box_destructor, "vtable"), UiType::Text, U32(sym::text_box_render_node.node),
             U32(sym::text_box_destructor.text), false, UiKind::Children::None, 0, UiClickInput::Passes},
            // The edit box's text at +0xF8 is not in symbols.toml.
            {Value(sym::edit_box_vtable, "vtable"), UiType::Editbox, U32(sym::edit_box_render_node.node), 0xF8},
            // The check box's ticked byte at +0x100 is not in symbols.toml.
            {Value(sym::checkbox_vtable, "vtable"), UiType::Checkbox, U32(sym::checkbox_render_node.node), 0, false,
             UiKind::Children::None, 0x100},
            {Value(sym::listbox_vtable, "vtable"), UiType::Listbox, U32(sym::listbox_render_node.node), 0, false,
             UiKind::Children::Listbox, 0, UiClickInput::Passes},
            {Value(sym::smooth_listbox_vtable, "vtable"), UiType::Listbox, U32(sym::smooth_listbox_render_node.node), 0,
             false, UiKind::Children::SmoothListbox, 0, UiClickInput::Passes},
            {Value(sym::overlapping_box_vtable, "vtable"), UiType::Listbox, U32(sym::overlapping_box_render_node.node), 0,
             false, UiKind::Children::Overlapping, 0, UiClickInput::Passes},
            {Value(sym::grid_box_vtable, "vtable"), UiType::Listbox, U32(sym::grid_box_render_node.node), 0, false,
             UiKind::Children::Grid, 0, UiClickInput::Passes},
            {Value(sym::scrollbar_vtable, "vtable"), UiType::Other, 0, 0, false, UiKind::Children::Scrollbar, 0,
             UiClickInput::Passes},
        };
    }

    // The members only Anvil knows: call when the plugin is loading.
    static UiLayout Load() {
        UiLayout l;
        const std::uint32_t map_vtable = Value(sym::window_type_definition, "map_vtable");
        Require(Value(sym::listbox_definition_map, "map_vtable") == map_vtable &&
                    Value(sym::container_definition, "map_vtable") == map_vtable &&
                    Value(sym::scrollbar_definition_map, "map_vtable") == map_vtable,
                "the definition maps' vtable");
        l.sprite_node_size_rva = RvaOf(sym::sprite_node_size);
        l.node_type_getter_rva = RvaOf(sym::node_object_type);
        l.node_frame_getter_rva = RvaOf(sym::node_super_state);
        l.node_children_getter_rva = RvaOf(sym::node_draws_children);
        Require(Value(sym::player_country, "countries_db") == l.countries_db_rva, "the country database");
        Require(Value(sym::shield_custom_value, "countries_db") == l.countries_db_rva, "the country database");
        l.widget_name_slot_rva = RvaOf(sym::widget_name_slot);
        l.window_name_slot_rva = RvaOf(sym::window_name_slot);
        l.container_name_slot_rva = RvaOf(sym::container_name_slot);
        return l;
    }
};

// Reads the widget tree. Main thread only: the game deletes widgets in its GUI
// update and a reload frees definitions. Every read and call is guarded, and
// slot 0xA0 is called only on widgets whose definition the type registry
// holds; returns an error text, or empty on success.
// `client`: the game window's client area (w, h), or 0 x 0 when unknown;
// rects are fitted to it (FitRects in ui_tree.hpp).
std::string ReadUiTree(std::uintptr_t image_base, const UiLayout& layout, UiRect client, UiTree& out);

// Main thread: the widget the game hovers (mgr+0x130), 0 when none or unreadable.
std::uintptr_t ReadHoveredWidget(std::uintptr_t image_base, const UiLayout& layout);
// Whether an edit box has the keyboard focus: the GUI manager's text input target, which
// the edit box's focus call sets and its focus removal clears. nullopt when the manager cannot be read.
std::optional<bool> ReadTextFieldFocused(std::uintptr_t image_base, const UiLayout& layout);

// Main thread: the widgets the game found under the pointer on the last frame,
// topmost first (the order it hands them a click); empty when unreadable.
std::vector<std::uintptr_t> ReadUnderPointer(std::uintptr_t image_base, const UiLayout& layout);

// Main thread: the hovered widget and its ancestors only, without reading the
// whole tree (tongs.ui's hovered without TONGS_UI_PATHS, docs/design.md "The
// tongs.ui service"). `out` is a tree whose one root is the top-level window
// and whose widgets are the chain, plus the children of windows whose rect
// FitRects takes from their children. Names come from direct reads of what
// slot 0xA0 would return, never from the call; a widget is stale when
// `definitions` cannot show its definition live. Sets `hovered` to the
// hovered widget's address, 0 when nothing (live) is hovered. `complete` is
// false when the climb could not see the whole way up (ui_structure.hpp):
// read the whole tree instead.
std::string ReadHoveredChain(std::uintptr_t image_base, const UiLayout& layout, UiRect client,
                             LiveDefinitionCache& definitions, UiTree& out, std::uintptr_t& hovered, bool& complete);

}  // namespace tongs
