#pragma once

#include "events.hpp"

#include <cstdint>
#include <string>

namespace tongs {

// Where EU4 1.37.5 keeps its open events (docs/design.md, "Events"). RVAs are
// relative to eu4.exe. Lists are doubly linked with nodes {value*, prev, next
// at +0x10, "being removed" byte at +0x18}.
struct EventLayout {
    unsigned root_rva = 0x233FE78;
    unsigned session = 0x1E00;              // root: the session
    unsigned session_vtable_rva = 0x1CB8460;
    unsigned popups = 0x78;                 // session: every open pop-up, event controllers among them
    unsigned windows = 0x498;               // session: {event, scope, window} of every filled event window
    unsigned window_count = 0x4A8;          // session: int32 entries in that list
    unsigned node_next = 0x10;
    unsigned node_removed = 0x18;
    unsigned entry_window = 0x10;           // the full window object in a {event, scope, window} entry
    unsigned controller_vtable_rva = 0x1D590F8;
    unsigned controller_window = 0x38;      // full windowType object; 0 before the window is filled
    unsigned controller_event = 0x48;
    unsigned controller_waiting = 0x124;    // byte: no option chosen yet
    unsigned controller_buttons = 0x208;    // list of option buttons
    unsigned window_part = 0x30;            // full window object -> widget pointer
    unsigned event_id = 0x20;               // std::string: the script id
    unsigned event_numeric_id = 0x14;       // int32, assigned at load
    unsigned event_options = 0x790;         // std::vector<option*>
    unsigned option_name = 0x88;            // std::string
    unsigned button_scope = 0x38;           // std::string on the button: "<numeric id>_<option index>"
    unsigned option_delay_rva = 0x233E854;  // float define: options stay disabled this long after opening
};

// Main thread only: the game creates and deletes event windows in its frame.
// Every read is guarded; returns an error text, or empty on success.
std::string ReadEvents(std::uintptr_t image_base, const EventLayout& layout, EventsRaw& out);

// Any thread: the number of entries in the event window list of the session,
// read as one word; -1 when there is no session.
int CountEventWindows(std::uintptr_t image_base, const EventLayout& layout);

}  // namespace tongs
