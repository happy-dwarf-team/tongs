#pragma once

#include "events.hpp"
#include "game_symbols.hpp"

#include <cstdint>
#include <string>

namespace tongs {

// Where EU4 1.37.5 keeps its open events (docs/design.md, "Events"), from
// plugin/symbols.toml. RVAs are relative to eu4.exe. The session's lists are
// {head, tail, count}; the game's code names the tail, and the head is the
// pointer before it (not in symbols.toml). Their nodes are {value*, prev,
// next, "being removed" byte}.
struct EventLayout {
    static constexpr unsigned kHeadBeforeTail = 8;  // not in symbols.toml

    unsigned root_rva = U32(sym::root_and_date.root);
    unsigned session = U32(sym::session_of_root.session);  // root: the session
    unsigned session_vtable_rva = 0;                        // SessionVtableRva(), set when loading
    unsigned popups = U32(sym::popup_list_append.tail) - kHeadBeforeTail;  // session: every open pop-up, event controllers among them
    unsigned windows = U32(sym::event_window_list.tail) - kHeadBeforeTail;  // session: {event, scope, window} of every filled event window
    unsigned window_count = U32(sym::event_window_count.count);  // session: int32 entries in that list
    unsigned node_next = U32(sym::popup_list_append.node_next);
    unsigned node_removed = U32(sym::popup_list_append.node_removed);
    unsigned entry_window = U32(sym::event_window_list.window);  // the full window object in a {event, scope, window} entry
    unsigned controller_vtable_rva = U32(sym::event_controller.vtable);
    unsigned controller_window = U32(sym::event_controller.window);  // full windowType object; 0 before the window is filled
    unsigned controller_event = U32(sym::event_controller.event);
    unsigned controller_waiting = U32(sym::option_click.waiting);   // byte: no option chosen yet
    unsigned controller_buttons = U32(sym::event_button_append.buttons);  // list of option buttons
    unsigned window_part = U32(sym::eu3dialog_vtable.window_part);  // full window object -> widget pointer
    unsigned event_id = U32(sym::event_id_parser.script_id);        // std::string: the script id
    unsigned event_numeric_id = U32(sym::event_numeric_id.numeric_id);  // int32, assigned at load
    unsigned event_options = U32(sym::event_options.options);       // std::vector<option*>
    unsigned option_name = U32(sym::option_name_parser.name);       // std::string
    unsigned button_scope = U32(sym::gui_object_assign_string.text);  // std::string on the button: "<numeric id>_<option index>"
    unsigned option_delay_rva = U32(sym::option_delay.delay);       // float define: options stay disabled this long after opening

    // The members only Anvil knows: call when the plugin is loading.
    static EventLayout Load() {
        EventLayout l;
        l.session_vtable_rva = SessionVtableRva();
        return l;
    }
};

// Main thread only: the game creates and deletes event windows in its frame.
// Every read is guarded; returns an error text, or empty on success.
std::string ReadEvents(std::uintptr_t image_base, const EventLayout& layout, EventsRaw& out);

// Any thread: the number of entries in the event window list of the session,
// read as one word; -1 when there is no session.
int CountEventWindows(std::uintptr_t image_base, const EventLayout& layout);

}  // namespace tongs
