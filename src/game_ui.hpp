#pragma once

#include "ui_tree.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tongs {

// How the widget tree reader treats one class of widget, told apart by the
// vtable at the widget pointer (the GUI classes have no RTTI).
struct UiKind {
    enum class Children { None, Window, Container, Listbox, SmoothListbox, Scrollbar };
    unsigned vtable = 0;  // RVA
    UiType type = UiType::Other;
    unsigned node = 0;    // the widget's render node pointer; 0 = none
    unsigned text = 0;    // std::string shown by the widget; 0 = none
    bool state = false;   // int32 state at UiLayout::button_state, 3 = disabled
    Children children = Children::None;
};

// Where EU4 1.37.5 keeps its widgets (docs/design.md, "Widget tree"). RVAs are
// relative to eu4.exe; offsets are from the widget pointer the registry holds,
// except "full" ones, which start 0x30 before it (the whole window object).
struct UiLayout {
    unsigned manager_rva = 0x23494F0;   // GUI manager pointer
    unsigned registry_begin = 0x110;    // mgr: std::vector<widget*> of every live widget
    unsigned registry_end = 0x118;
    unsigned dead = 0xC4;               // byte: waiting for the registry sweep
    unsigned shown = 0xC5;              // byte: visible (children of a hidden window may keep it)
    unsigned slot_position = 0xD8;      // vtable slots writing two int32: position in client pixels
    unsigned slot_size = 0xE0;          // and size
    unsigned slot_name = 0xA0;          // returns const std::string*
    unsigned button_state = 0x124;

    unsigned window_part = 0x30;        // full window object -> widget pointer
    // windowType children (full object): ten vector pairs {pointers, names}, seven
    // named maps with an insertion list, and sub-windows {std::string name; window*}.
    unsigned window_pairs = 0x5B8;
    unsigned window_pair_count = 10;
    unsigned window_maps = 0x798;
    unsigned window_map_count = 7;
    unsigned map_stride = 0x40;
    unsigned map_list_head = 0x20;      // first list node {entry*, prev, next}; the entry holds the value at +0
    unsigned list_node_next = 0x10;
    unsigned window_subwindows = 0x958;
    unsigned subwindow_size = 0x28;
    unsigned subwindow_pointer = 0x20;
    // containerWindowType children (full object): fifteen vector pairs.
    unsigned container_pairs = 0x680;
    unsigned container_pair_count = 15;
    unsigned pair_stride = 0x30;
    // List boxes: an insertion list of items at +0x100; the item holds the full
    // object of its window. The scroll bar is a separate pointer.
    unsigned list_items = 0x100;
    unsigned listbox_item_window = 0x40;
    unsigned listbox_scrollbar = 0x1E0;
    unsigned smooth_item_window = 0x60;
    unsigned smooth_scrollbar = 0x228;
    // Scroll bars hold their four buttons: slider, track, down, up.
    unsigned scrollbar_parts[4] = {0x158, 0x2F0, 0x2F8, 0x3E0};
    // Render nodes: the owning widget and the parent node. Widgets the game's
    // own code holds (not a window) are placed under the owner of the parent node.
    unsigned node_owner = 0x118;
    unsigned node_parent = 0x110;

    std::vector<UiKind> kinds = {
        {0x1D929D0, UiType::Window, 0xA00, 0, false, UiKind::Children::Window},     // windowType
        {0x1D57510, UiType::Window, 0xA00, 0, false, UiKind::Children::Window},     // eu3dialogtype
        {0x1DA4090, UiType::Window, 0xA50, 0, false, UiKind::Children::Container},  // containerWindowType
        {0x1D58218, UiType::Button, 0x290, 0x200, true},                            // guiButtonType
        {0x1D9A340, UiType::Button, 0x290, 0x200, true},                            // buttonType
        {0x1D954B0, UiType::Button, 0x168},                                         // scroll bar slider
        {0x1DA2940, UiType::Button, 0x180},                                         // scroll bar track
        {0x1D585C8, UiType::Icon, 0x168},                                           // iconType
        {0x1D96D00, UiType::Icon, 0x168},
        {0x1D8FD10, UiType::Text, 0x118, 0x170},                                    // instantTextBoxType
        {0x1D99230, UiType::Text, 0xD0, 0x178},                                     // textBoxType
        {0x1D91358, UiType::Editbox, 0x1E8},                                        // editBoxType
        {0x1D8FFC8, UiType::Checkbox, 0x110},                                       // checkboxType
        {0x1D90FD8, UiType::Listbox, 0x180, 0, false, UiKind::Children::Listbox},   // listboxType
        {0x1D96578, UiType::Listbox, 0x218, 0, false, UiKind::Children::SmoothListbox},  // smoothListboxType
        {0x1D97EF0, UiType::Listbox},                                               // gridBoxType
        {0x1D93748, UiType::Other, 0, 0, false, UiKind::Children::Scrollbar},       // scrollbarType
    };
};

// Reads the widget tree. Main thread only: the game deletes widgets in its GUI
// update. Every read and call is guarded; returns an error text, or empty on success.
std::string ReadUiTree(std::uintptr_t image_base, const UiLayout& layout, UiTree& out);

}  // namespace tongs
