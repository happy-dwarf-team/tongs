#pragma once

#include "ui_tree.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// The open events as read on the main thread (game_events.cpp), and what
// `event list` / `event choose` make of them (docs/design.md, "Events").
// Nothing here touches game memory, so the tests build events by hand.

// An option button as the event controller lists it (+0x208).
struct EventButtonRaw {
    std::uintptr_t widget = 0;  // the button, as the widget registry holds it
    std::string scope;          // the widget's string at +0x38: "<numeric event id>_<option index>"
};

// One event controller from the session's pop-up list (+0x78).
struct OpenEventRaw {
    std::uintptr_t controller = 0;
    bool waiting = false;        // +0x124: no option chosen yet
    std::uintptr_t window = 0;   // the event window's widget pointer; 0 before the window is filled
    std::string id;              // script id (event+0x20), game text
    std::int32_t numeric_id = 0;  // event+0x14, assigned at load; only matched against the buttons
    std::vector<std::string> option_keys;  // option names by index (event+0x790, option+0x88), game text
    std::vector<EventButtonRaw> buttons;   // in the controller's order, which is the order on screen
};

struct EventsRaw {
    std::vector<OpenEventRaw> events;  // in the order of the pop-up list: oldest first
    std::vector<std::uintptr_t> listed_windows;  // widget pointers in the session's event window list (+0x498)
    bool has_delay = false;
    float option_delay = 0;            // the define that keeps option buttons disabled after opening
};

struct EventOption {
    int number = 0;        // on screen, from 1
    int index = -1;        // option_index: the option in the event file; -1 when the button does not say
    std::string key;       // option_key (option name), UTF-8; may be empty or repeated
    int widget = -1;       // the button in the tree; -1 when it is not there
    std::string text;      // UTF-8
    bool enabled = false;
    bool visible = false;
    std::string path;
};

struct EventView {
    int number = 0;        // in `event list`, from 1, topmost (newest) first
    std::uintptr_t controller = 0;
    std::string id;        // UTF-8
    std::string title;
    std::string description;
    int window = -1;
    std::uintptr_t window_address = 0;  // the window's widget pointer, as OpenEventRaw::window
    std::string path;
    std::vector<EventOption> options;
};

// The events waiting for a choice whose window is filled, topmost first, with
// their options matched to widgets of the tree.
std::vector<EventView> BuildEvents(const UiTree& tree, const EventsRaw& raw);

// "<numeric id>_<index>" -> index when the id matches; -1 otherwise.
int ParseOptionScope(std::string_view scope, std::int32_t numeric_id);

// `event list [timeout=S]` and `event choose N | text^="…" [event=N | event=title^="…"] [timeout=S]`.
struct EventRequest {
    enum class Verb { List, Choose };
    enum class Match { None, Number, Equals, Contains, Starts };
    Verb verb = Verb::List;
    Match option = Match::None;  // Number or a text match
    int option_number = 0;
    std::string option_text;     // lower case
    Match event = Match::None;   // None: the only open event
    int event_number = 0;
    std::string event_title;     // lower case
    int timeout_ms = 0;          // 0: the dispatcher's default
};

// Parses the words after `event`. On failure returns the JSON members of the
// error (starting with a comma), otherwise an empty string.
std::string ParseEventRequest(std::string_view argument, EventRequest& out);

// Which option `event choose` clicks, or why it refuses.
struct EventChoice {
    std::string error;    // "", "not_found", "ambiguous", "disabled", "not_visible"
    std::string members;  // JSON members of a refusal (starting with a comma)
    int event = -1;       // indices into the events and their options
    int option = -1;
};

EventChoice ChooseEvent(const std::vector<EventView>& events, const EventRequest& request);

// The first enabled, visible option of an event; -1 when there is none.
int FirstEnabledOption(const EventView& event);

// JSON: an event with its options, as `event list` shows it.
std::string EventJson(const EventView& event);
// JSON members of a choice (starting with a comma): event and option.
std::string ChoiceMembers(const EventView& event, const EventOption& option);
// JSON object of a choice made during run_until: date, id, title, option.
std::string ChoiceJson(const EventView& event, const EventOption& option, std::string_view date);

}  // namespace tongs
