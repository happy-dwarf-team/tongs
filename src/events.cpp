#include "events.hpp"

#include "protocol.hpp"

#include <charconv>
#include <unordered_map>

namespace tongs {
namespace {

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string LowerCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = Lower(c);
    return out;
}

bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) return false;
    }
    return true;
}

bool ParseNumber(std::string_view text, int& out) {
    if (text.empty()) return false;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::string Num(int value) { return std::to_string(value); }
const char* Bool(bool value) { return value ? "true" : "false"; }

std::string Quoted(std::string_view text) { return "\"" + JsonEscape(text) + "\""; }

// The first widget named `name` with text inside `root` (breadth first, a few levels).
std::string TextBelow(const UiTree& tree, int root, std::string_view name) {
    std::vector<int> level{root};
    for (int depth = 0; depth < 4 && !level.empty(); ++depth) {
        std::vector<int> next;
        for (const int w : level) {
            for (const int child : tree.widgets[w].children) {
                if (child < 0 || child >= static_cast<int>(tree.widgets.size())) continue;
                const UiWidget& c = tree.widgets[child];
                if (c.has_text && SameName(c.name, name)) return c.text;
                next.push_back(child);
            }
        }
        level = std::move(next);
    }
    return {};
}

// Words separated by spaces; spaces inside double quotes belong to the word.
std::vector<std::string_view> SplitQuoted(std::string_view text, bool& unclosed) {
    std::vector<std::string_view> words;
    unclosed = false;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size()) break;
        const std::size_t start = i;
        bool quoted = false;
        while (i < text.size() && (quoted || (text[i] != ' ' && text[i] != '\t'))) {
            if (text[i] == '"') quoted = !quoted;
            ++i;
        }
        if (quoted) unclosed = true;
        words.push_back(text.substr(start, i - start));
    }
    return words;
}

// "=v", "~=v" or "^=v" (v may be quoted) -> the match and the lower-case value.
bool ParseTextMatch(std::string_view spec, EventRequest::Match& match, std::string& value) {
    using Match = EventRequest::Match;
    if (spec.starts_with("~=")) {
        match = Match::Contains;
        spec.remove_prefix(2);
    } else if (spec.starts_with("^=")) {
        match = Match::Starts;
        spec.remove_prefix(2);
    } else if (spec.starts_with("=")) {
        match = Match::Equals;
        spec.remove_prefix(1);
    } else {
        return false;
    }
    if (spec.size() >= 2 && spec.front() == '"' && spec.back() == '"') spec = spec.substr(1, spec.size() - 2);
    if (spec.empty() || spec.find('"') != std::string_view::npos) return false;
    value = LowerCopy(spec);
    return true;
}

bool TextMatches(EventRequest::Match match, std::string_view value, std::string_view text) {
    const std::string lower = LowerCopy(text);
    switch (match) {
        case EventRequest::Match::Equals: return lower == value;
        case EventRequest::Match::Contains: return lower.find(value) != std::string::npos;
        case EventRequest::Match::Starts: return lower.starts_with(value);
        default: return false;
    }
}

std::string EventSummary(const EventView& e) {
    return "{\"number\":" + Num(e.number) + ",\"id\":" + Quoted(e.id) + ",\"title\":" + Quoted(e.title) + "}";
}

std::string OptionSummary(const EventOption& o) {
    return "{\"number\":" + Num(o.number) + ",\"text\":" + Quoted(o.text) + ",\"enabled\":" + Bool(o.enabled) + "}";
}

std::string Refusal(EventChoice& c, const char* code, const std::string& detail, std::string members = {}) {
    c.error = code;
    c.members = ",\"detail\":" + Quoted(detail) + members;
    return c.error;
}

}  // namespace

int ParseOptionScope(std::string_view scope, std::int32_t numeric_id) {
    const std::size_t cut = scope.find('_');
    if (cut == std::string_view::npos) return -1;
    int id = 0, index = 0;
    if (!ParseNumber(scope.substr(0, cut), id) || !ParseNumber(scope.substr(cut + 1), index)) return -1;
    return id == numeric_id && index >= 0 ? index : -1;
}

std::vector<EventView> BuildEvents(const UiTree& tree, const EventsRaw& raw) {
    std::unordered_map<std::uintptr_t, int> by_address;
    for (int i = 0; i < static_cast<int>(tree.widgets.size()); ++i) {
        if (tree.widgets[i].address != 0) by_address.emplace(tree.widgets[i].address, i);
    }
    const auto find = [&by_address](std::uintptr_t address) {
        const auto it = by_address.find(address);
        return it == by_address.end() ? -1 : it->second;
    };
    const UiView visible(tree, false);
    const UiView all(tree, true);
    const auto path_of = [&](int w) { return visible.Visible(w) ? visible.PathOf(w) : all.PathOf(w); };

    std::vector<EventView> out;
    // The newest event opens on top of the others.
    for (auto it = raw.events.rbegin(); it != raw.events.rend(); ++it) {
        const OpenEventRaw& e = *it;
        if (!e.waiting || e.window == 0) continue;
        EventView v;
        v.number = static_cast<int>(out.size()) + 1;
        v.controller = e.controller;
        v.id = GameTextToUtf8(e.id);
        v.window = find(e.window);
        v.window_address = e.window;
        if (v.window != -1) {
            v.path = path_of(v.window);
            v.title = TextBelow(tree, v.window, "Title");
            v.description = TextBelow(tree, v.window, "Description");
        }
        for (const EventButtonRaw& b : e.buttons) {
            EventOption o;
            o.number = static_cast<int>(v.options.size()) + 1;
            o.index = ParseOptionScope(b.scope, e.numeric_id);
            if (o.index >= 0 && o.index < static_cast<int>(e.option_keys.size())) {
                o.key = GameTextToUtf8(e.option_keys[o.index]);
            }
            o.widget = find(b.widget);
            if (o.widget != -1) {
                const UiWidget& w = tree.widgets[o.widget];
                o.text = w.text;
                o.enabled = w.has_enabled && w.enabled;
                o.visible = visible.Visible(o.widget);
                o.path = path_of(o.widget);
            }
            v.options.push_back(std::move(o));
        }
        out.push_back(std::move(v));
    }
    return out;
}

std::string ParseEventRequest(std::string_view argument, EventRequest& out) {
    out = {};
    using Match = EventRequest::Match;
    const auto fail = [](const std::string& detail) {
        return ",\"error\":\"bad_command\",\"detail\":" + Quoted(detail);
    };
    constexpr char kUsage[] =
        "usage: event list [timeout=S] | event choose N|text^=\"...\" [event=N|event=title^=\"...\"] [timeout=S]";
    bool unclosed = false;
    const std::vector<std::string_view> words = SplitQuoted(argument, unclosed);
    if (unclosed) return fail("unclosed \"");
    if (words.empty()) return fail(kUsage);
    if (words[0] == "list") {
        out.verb = EventRequest::Verb::List;
    } else if (words[0] == "choose") {
        out.verb = EventRequest::Verb::Choose;
    } else {
        return fail(kUsage);
    }
    for (std::size_t i = 1; i < words.size(); ++i) {
        const std::string_view word = words[i];
        int number = 0;
        if (word.starts_with("timeout=")) {
            if (!ParseNumber(word.substr(8), number) || number < 1 || number > 600) return fail("timeout=1..600");
            out.timeout_ms = number * 1000;
        } else if (out.verb == EventRequest::Verb::List) {
            return fail("event list takes only timeout=S");
        } else if (word.starts_with("event=")) {
            if (out.event != Match::None) return fail("event= given twice");
            const std::string_view value = word.substr(6);
            if (ParseNumber(value, number)) {
                if (number < 1) return fail("event=N counts from 1");
                out.event = Match::Number;
                out.event_number = number;
            } else if (!value.starts_with("title") || !ParseTextMatch(value.substr(5), out.event, out.event_title)) {
                return fail("expected event=N or event=title^=\"...\" (also title= and title~=)");
            }
        } else if (out.option != Match::None) {
            return fail("bad option: " + std::string(word.substr(0, 40)));
        } else if (ParseNumber(word, number)) {
            if (number < 1) return fail("options count from 1");
            out.option = Match::Number;
            out.option_number = number;
        } else if (!word.starts_with("text") || !ParseTextMatch(word.substr(4), out.option, out.option_text)) {
            return fail("expected an option number or text^=\"...\" (also text= and text~=), not " +
                        std::string(word.substr(0, 40)));
        }
    }
    if (out.verb == EventRequest::Verb::Choose && out.option == Match::None) return fail("event choose needs an option");
    return {};
}

EventChoice ChooseEvent(const std::vector<EventView>& events, const EventRequest& r) {
    using Match = EventRequest::Match;
    EventChoice c;
    const auto event_list = [](const std::vector<const EventView*>& list) {
        std::string out = ",\"events\":[";
        for (std::size_t i = 0; i < list.size(); ++i) out += (i ? "," : "") + EventSummary(*list[i]);
        return out + "]";
    };
    std::vector<const EventView*> all;
    for (const EventView& e : events) all.push_back(&e);
    if (events.empty()) {
        Refusal(c, "not_found", "no event is open");
        return c;
    }
    if (r.event == Match::Number) {
        if (r.event_number > static_cast<int>(events.size())) {
            Refusal(c, "not_found", "no event " + Num(r.event_number) + "; " + Num(static_cast<int>(events.size())) + " open",
                    event_list(all));
            return c;
        }
        c.event = r.event_number - 1;
    } else if (r.event != Match::None) {
        std::vector<const EventView*> hits;
        for (const EventView& e : events) {
            if (TextMatches(r.event, r.event_title, e.title)) hits.push_back(&e);
        }
        if (hits.empty()) {
            Refusal(c, "not_found", "no open event has that title", event_list(all));
            return c;
        }
        if (hits.size() > 1) {
            Refusal(c, "ambiguous", Num(static_cast<int>(hits.size())) + " open events match the title", event_list(hits));
            return c;
        }
        c.event = hits[0]->number - 1;
    } else if (events.size() > 1) {
        Refusal(c, "ambiguous", Num(static_cast<int>(events.size())) + " events are open; name one with event=N or event=title^=\"...\"",
                event_list(all));
        return c;
    } else {
        c.event = 0;
    }

    const EventView& e = events[c.event];
    const std::string about = ",\"event\":" + EventSummary(e);
    const auto option_list = [](const std::vector<const EventOption*>& list) {
        std::string out = ",\"options\":[";
        for (std::size_t i = 0; i < list.size(); ++i) out += (i ? "," : "") + OptionSummary(*list[i]);
        return out + "]";
    };
    std::vector<const EventOption*> options;
    for (const EventOption& o : e.options) options.push_back(&o);
    if (r.option == Match::Number) {
        if (r.option_number > static_cast<int>(e.options.size())) {
            Refusal(c, "not_found", "the event has " + Num(static_cast<int>(e.options.size())) + " options on screen",
                    about + option_list(options));
            c.event = -1;
            return c;
        }
        c.option = r.option_number - 1;
    } else {
        std::vector<const EventOption*> hits;
        for (const EventOption& o : e.options) {
            if (TextMatches(r.option, r.option_text, o.text)) hits.push_back(&o);
        }
        if (hits.size() != 1) {
            Refusal(c, hits.empty() ? "not_found" : "ambiguous",
                    hits.empty() ? "no option has that text" : Num(static_cast<int>(hits.size())) + " options match the text",
                    about + option_list(hits.empty() ? options : hits));
            c.event = -1;
            return c;
        }
        c.option = hits[0]->number - 1;
    }
    const EventOption& o = e.options[c.option];
    const std::string option = about + ",\"option\":" + OptionSummary(o);
    if (o.widget == -1) {
        Refusal(c, "not_found", "the option's button is not in the widget tree", option);
    } else if (!o.visible) {
        Refusal(c, "not_visible", "the option's button is hidden", option);
    } else if (!o.enabled) {
        Refusal(c, "disabled", "the option's button is disabled (the game keeps them disabled for a moment after opening)",
                option);
    }
    if (c.error == "not_visible") {
        c.hidden_event = c.event;
        c.hidden_option = c.option;
    }
    if (!c.error.empty()) c.event = c.option = -1;
    return c;
}

int OptionScrollButton(const UiTree& tree, const EventView& event, int option) {
    const EventOption& o = event.options[option];
    if (o.widget == -1) return -1;
    const UiView all(tree, true);
    int list = all.Parent(o.widget);
    while (list != -1 && tree.widgets[list].type != UiType::Listbox) list = all.Parent(list);
    if (list == -1) return -1;
    bool shown_before = false;
    for (const EventOption& other : event.options) shown_before = shown_before || (other.visible && other.number < o.number);
    const std::string_view name = shown_before ? "downButton" : "upButton";
    for (const int child : tree.widgets[list].children) {
        for (const int button : tree.widgets[child].children) {
            if (tree.widgets[button].name == name) return button;
        }
    }
    return -1;
}

int FirstEnabledOption(const EventView& event) {
    for (int i = 0; i < static_cast<int>(event.options.size()); ++i) {
        const EventOption& o = event.options[i];
        if (o.widget != -1 && o.visible && o.enabled) return i;
    }
    return -1;
}

std::string EventJson(const EventView& e) {
    std::string out = "{\"number\":" + Num(e.number) + ",\"id\":" + Quoted(e.id) + ",\"title\":" + Quoted(e.title) +
                      ",\"description\":" + Quoted(e.description) + ",\"path\":" + Quoted(e.path) + ",\"options\":[";
    for (std::size_t i = 0; i < e.options.size(); ++i) {
        const EventOption& o = e.options[i];
        if (i) out += ',';
        out += "{\"number\":" + Num(o.number) + ",\"text\":" + Quoted(o.text) + ",\"enabled\":" + Bool(o.enabled) +
               ",\"option_index\":" + Num(o.index) + ",\"option_key\":" + Quoted(o.key) + ",\"path\":" + Quoted(o.path);
        if (!o.visible) out += ",\"visible\":false";
        out += '}';
    }
    return out + "]}";
}

std::string ChoiceMembers(const EventView& e, const EventOption& o) {
    return ",\"event\":" + EventSummary(e) + ",\"option\":{\"number\":" + Num(o.number) + ",\"text\":" + Quoted(o.text) +
           ",\"option_index\":" + Num(o.index) + ",\"option_key\":" + Quoted(o.key) + ",\"path\":" + Quoted(o.path) + "}";
}

std::string ChoiceJson(const EventView& e, const EventOption& o, std::string_view date) {
    return "{\"date\":" + Quoted(date) + ",\"id\":" + Quoted(e.id) + ",\"title\":" + Quoted(e.title) +
           ",\"option\":" + Num(o.number) + ",\"text\":" + Quoted(o.text) + ",\"option_index\":" + Num(o.index) +
           ",\"option_key\":" + Quoted(o.key) + ",\"path\":" + Quoted(o.path) + "}";
}

}  // namespace tongs
