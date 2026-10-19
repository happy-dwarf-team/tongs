#include "game_events.hpp"

#include "game_memory.hpp"

namespace tongs {
namespace {

constexpr std::size_t kMaxNodes = 1000;
constexpr std::size_t kMaxOptions = 256;  // the base game has at most 33

// The session, when a game is loaded.
std::uintptr_t Session(std::uintptr_t base, const EventLayout& l) {
    std::uintptr_t root = 0, session = 0, vtable = 0;
    if (!Read(base + l.root_rva, root) || root == 0 || !Read(root + l.session, session) || session == 0 ||
        !Read(session, vtable) || vtable != base + l.session_vtable_rva) {
        return 0;
    }
    return session;
}

// The values of a list; with `live`, nodes marked as being removed are skipped.
std::vector<std::uintptr_t> ListValues(std::uintptr_t head, const EventLayout& l, bool live) {
    std::vector<std::uintptr_t> out;
    std::uintptr_t node = 0;
    if (!Read(head, node)) return out;
    for (std::size_t i = 0; node != 0 && i < kMaxNodes; ++i) {
        std::uintptr_t value = 0;
        std::uint8_t removed = 0;
        if (Read(node, value) && value != 0 && (!live || (Read(node + l.node_removed, removed) && removed == 0))) {
            out.push_back(value);
        }
        if (!Read(node + l.node_next, node)) break;
    }
    return out;
}

void ReadController(std::uintptr_t controller, const EventLayout& l, OpenEventRaw& e) {
    e.controller = controller;
    std::uint8_t waiting = 0;
    e.waiting = Read(controller + l.controller_waiting, waiting) && waiting != 0;
    std::uintptr_t window = 0;
    if (Read(controller + l.controller_window, window) && window != 0) e.window = window + l.window_part;
    std::uintptr_t event = 0;
    if (!Read(controller + l.controller_event, event) || event == 0) return;
    ReadString(event + l.event_id, e.id);
    Read(event + l.event_numeric_id, e.numeric_id);
    std::uintptr_t begin = 0, end = 0;
    if (Read(event + l.event_options, begin) && Read(event + l.event_options + 8, end) && end >= begin &&
        (end - begin) % 8 == 0 && (end - begin) / 8 <= kMaxOptions) {
        for (std::uintptr_t at = begin; at < end; at += 8) {
            std::uintptr_t option = 0;
            std::string name;
            if (Read(at, option) && option != 0) ReadString(option + l.option_name, name);
            e.option_keys.push_back(std::move(name));
        }
    }
    for (const std::uintptr_t button : ListValues(controller + l.controller_buttons, l, false)) {
        EventButtonRaw b;
        b.widget = button;
        ReadString(button + l.button_scope, b.scope);
        e.buttons.push_back(std::move(b));
    }
}

}  // namespace

std::string ReadEvents(std::uintptr_t image_base, const EventLayout& layout, EventsRaw& out) {
    out = {};
    const std::uintptr_t session = Session(image_base, layout);
    if (session == 0) return "no game loaded";
    for (const std::uintptr_t popup : ListValues(session + layout.popups, layout, false)) {
        std::uintptr_t vtable = 0;
        if (!Read(popup, vtable) || vtable != image_base + layout.controller_vtable_rva) continue;
        OpenEventRaw e;
        ReadController(popup, layout, e);
        out.events.push_back(std::move(e));
    }
    for (const std::uintptr_t entry : ListValues(session + layout.windows, layout, true)) {
        std::uintptr_t window = 0;
        if (Read(entry + layout.entry_window, window) && window != 0) out.listed_windows.push_back(window + layout.window_part);
    }
    out.has_delay = Read(image_base + layout.option_delay_rva, out.option_delay);
    return {};
}

int CountEventWindows(std::uintptr_t image_base, const EventLayout& layout) {
    const std::uintptr_t session = Session(image_base, layout);
    std::int32_t count = 0;
    if (session == 0 || !Read(session + layout.window_count, count)) return -1;
    return count;
}

}  // namespace tongs
