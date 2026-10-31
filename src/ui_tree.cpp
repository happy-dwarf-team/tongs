#include "ui_tree.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <charconv>

namespace tongs {
namespace {

constexpr const char* kTypeNames[] = {"window", "button", "icon", "text", "listbox", "checkbox", "editbox", "other"};

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string LowerCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = Lower(c);
    return out;
}

std::string TypeList() {
    std::string out;
    for (const char* name : kTypeNames) out += (out.empty() ? "" : ", ") + std::string(name);
    return out;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

// `*` matches any run of characters, `?` one character; letters ignore case.
bool GlobMatch(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, resume = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || Lower(pattern[p]) == Lower(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) return false;
    }
    return true;
}

// A name that can stand in a path as it is: not empty, and nothing the grammar reads.
bool PlainName(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F || c == '/' || c == '[' || c == ']' || c == '*' || c == '?' || c == '"') return false;
    }
    return true;
}

bool ParseNumber(std::string_view text, int& out) {
    if (text.empty()) return false;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::string ParseFilter(std::string_view body, UiSelector::Step& step) {
    using Kind = UiSelector::Filter::Kind;
    if (!body.empty() && body.find_first_not_of("0123456789") == std::string_view::npos) {
        int n = 0;
        if (!ParseNumber(body, n) || n < 1) return "[n] counts from 1";
        if (step.index != 0) return "a step takes one [n]";
        step.index = n;
        return {};
    }
    UiSelector::Filter filter;
    if (body == "enabled" || body == "disabled") {
        filter.kind = body == "enabled" ? Kind::Enabled : Kind::Disabled;
        step.filters.push_back(filter);
        return {};
    }
    if (body == "checked" || body == "unchecked") {
        filter.kind = body == "checked" ? Kind::Checked : Kind::Unchecked;
        step.filters.push_back(filter);
        return {};
    }
    std::string_view key;
    if (body.starts_with("type")) {
        key = "type";
    } else if (body.starts_with("text")) {
        key = "text";
    } else {
        return "unknown filter [" + std::string(body.substr(0, 40)) + "]";
    }
    std::string_view rest = Trim(body.substr(4));
    std::string_view op;
    for (const std::string_view candidate : {"~=", "^=", "="}) {
        if (rest.starts_with(candidate)) {
            op = candidate;
            break;
        }
    }
    if (op.empty()) return "unknown filter [" + std::string(body.substr(0, 40)) + "]";
    std::string_view value = Trim(rest.substr(op.size()));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    } else if (value.empty()) {
        return "[" + std::string(key) + std::string(op) + "] needs a value";
    }
    if (key == "type") {
        if (op != "=") return "type takes only =";
        bool known = false;
        for (int i = 0; i < static_cast<int>(std::size(kTypeNames)); ++i) {
            if (value == kTypeNames[i]) {
                filter.type = static_cast<UiType>(i);
                known = true;
            }
        }
        if (!known) {
            return "unknown type \"" + std::string(value.substr(0, 20)) + "\" (" + TypeList() + ")";
        }
        filter.kind = Kind::Type;
    } else {
        filter.kind = op == "=" ? Kind::TextEquals : op == "~=" ? Kind::TextContains : Kind::TextStarts;
        filter.value = LowerCopy(value);
    }
    step.filters.push_back(std::move(filter));
    return {};
}

std::string Num(int value) { return std::to_string(value); }

}  // namespace

const char* UiTypeName(UiType type) { return kTypeNames[static_cast<int>(type)]; }

std::string UiSelectorHint() {
    std::string out =
        "A selector is .gui names joined by / (a child) or // (anywhere below), starting at a top-level "
        "window or with // (anywhere); names ignore case and take * and ?. Filters follow a name, or stand "
        "alone for any name:";
    for (const auto& note : kUiSelectorFilters) {
        out += ' ';
        out += note.filter;
        if (note.filter.starts_with("[type=")) {
            out += " (" + TypeList() + ")";
        } else if (!note.meaning.empty()) {
            out += " (" + std::string(note.meaning) + ")";
        }
    }
    out += ". Examples:";
    for (const std::string_view example : kUiSelectorExamples) out += ' ' + std::string(example) + ',';
    out.back() = '.';
    out += " Not XPath or globs:";
    for (const auto& fix : kUiSelectorFixes) out += ' ' + std::string(fix.wrong) + " is " + std::string(fix.right) + ',';
    out.back() = '.';
    return out;
}

std::string ParseUiSelector(std::string_view text, UiSelector& out) {
    out = {};
    const std::string_view s = Trim(text);
    if (s.empty()) return "empty selector";
    std::size_t i = 0;
    UiSelector::Axis axis = UiSelector::Axis::Top;
    if (s.starts_with("//")) {
        axis = UiSelector::Axis::Descendant;
        i = 2;
    } else if (s.front() == '/') {
        return "a selector starts with a name or with //";
    }
    for (;;) {
        UiSelector::Step step;
        step.axis = axis;
        const std::size_t name_start = i;
        while (i < s.size() && s[i] != '[' && s[i] != '/') ++i;
        step.name = std::string(Trim(s.substr(name_start, i - name_start)));
        if (step.name.find_first_of("]\"") != std::string::npos) return "unexpected ] or \" in a name";
        bool any_filter = false;
        while (i < s.size() && s[i] == '[') {
            std::size_t j = i + 1;
            bool quoted = false;
            while (j < s.size() && (quoted || s[j] != ']')) {
                if (s[j] == '"') quoted = !quoted;
                ++j;
            }
            if (j >= s.size()) return "unclosed [";
            if (std::string error = ParseFilter(Trim(s.substr(i + 1, j - i - 1)), step); !error.empty()) return error;
            any_filter = true;
            i = j + 1;
        }
        if (step.name.empty()) {
            if (!any_filter) return "missing name at position " + std::to_string(i + 1);
            step.name = "*";  // a step made only of filters means any name
        }
        out.steps.push_back(std::move(step));
        if (i >= s.size()) break;
        if (s.substr(i).starts_with("//")) {
            axis = UiSelector::Axis::Descendant;
            i += 2;
        } else if (s[i] == '/') {
            axis = UiSelector::Axis::Child;
            i += 1;
        } else {
            return std::string("unexpected \"") + s[i] + "\" at position " + std::to_string(i + 1);
        }
        if (i >= s.size()) return "selector ends with /";
    }
    return {};
}

UiView::UiView(const UiTree& tree, bool include_hidden, bool include_stale)
    : tree_(tree), include_hidden_(include_hidden), include_stale_(include_stale) {
    const int n = static_cast<int>(tree.widgets.size());
    position_.assign(n, -1);
    end_.assign(n, -1);
    parent_.assign(n, -1);
    visible_.assign(n, 0);
    // Iterative preorder; a widget reached twice (a malformed tree) keeps its first place.
    struct Frame {
        int widget;
        std::size_t next_child;
    };
    std::vector<Frame> stack;
    for (const int root : tree.roots) {
        if (root < 0 || root >= n || position_[root] != -1) continue;
        roots_.push_back(root);
        position_[root] = static_cast<int>(order_.size());
        order_.push_back(root);
        visible_[root] = tree.widgets[root].shown;
        stack.push_back({root, 0});
        while (!stack.empty()) {
            Frame& top = stack.back();
            const auto& children = tree.widgets[top.widget].children;
            if (top.next_child >= children.size()) {
                end_[top.widget] = static_cast<int>(order_.size());
                stack.pop_back();
                continue;
            }
            const int child = children[top.next_child++];
            if (child < 0 || child >= n || position_[child] != -1) continue;
            position_[child] = static_cast<int>(order_.size());
            order_.push_back(child);
            parent_[child] = top.widget;
            visible_[child] = visible_[top.widget] && tree.widgets[child].shown;
            stack.push_back({child, 0});
        }
    }
}

std::vector<int> UiView::Children(int widget) const {
    std::vector<int> out;
    for (const int child : tree_.widgets[widget].children) {
        if (child >= 0 && child < static_cast<int>(parent_.size()) && parent_[child] == widget && Usable(child)) {
            out.push_back(child);
        }
    }
    return out;
}

std::vector<int> UiView::Roots() const {
    std::vector<int> out;
    for (const int root : roots_) {
        if (Usable(root)) out.push_back(root);
    }
    return out;
}

bool UiView::Matches(int widget, const UiSelector::Step& step) const {
    using Kind = UiSelector::Filter::Kind;
    const UiWidget& w = tree_.widgets[widget];
    if (!GlobMatch(step.name, w.name)) return false;
    for (const auto& f : step.filters) {
        switch (f.kind) {
            case Kind::Type:
                if (w.type != f.type) return false;
                break;
            case Kind::Enabled:
            case Kind::Disabled:
                if (!w.has_enabled || w.enabled != (f.kind == Kind::Enabled)) return false;
                break;
            case Kind::Checked:
            case Kind::Unchecked:
                if (!w.has_checked || w.checked != (f.kind == Kind::Checked)) return false;
                break;
            default: {
                if (!w.has_text) return false;
                const std::string text = LowerCopy(w.text);
                if (f.kind == Kind::TextEquals && text != f.value) return false;
                if (f.kind == Kind::TextContains && text.find(f.value) == std::string::npos) return false;
                if (f.kind == Kind::TextStarts && !text.starts_with(f.value)) return false;
            }
        }
    }
    return true;
}

std::vector<int> UiView::Resolve(const UiSelector& selector) const {
    std::vector<int> context{-1};  // -1: the whole screen
    std::vector<char> taken(tree_.widgets.size(), 0);
    for (const auto& step : selector.steps) {
        std::vector<int> next;
        std::fill(taken.begin(), taken.end(), char{0});
        for (const int ctx : context) {
            std::vector<int> pool;
            if (step.axis == UiSelector::Axis::Top) {
                pool = Roots();
            } else if (step.axis == UiSelector::Axis::Child) {
                pool = Children(ctx);
            } else {
                const int from = ctx == -1 ? 0 : position_[ctx] + 1;
                const int to = ctx == -1 ? static_cast<int>(order_.size()) : end_[ctx];
                for (int k = from; k < to; ++k) {
                    if (Usable(order_[k])) pool.push_back(order_[k]);
                }
            }
            int seen = 0;
            for (const int candidate : pool) {
                if (!Matches(candidate, step)) continue;
                ++seen;
                if (step.index != 0 && seen != step.index) continue;
                if (!taken[candidate]) {
                    taken[candidate] = 1;
                    next.push_back(candidate);
                }
                if (step.index != 0) break;
            }
        }
        context = std::move(next);
        if (context.empty()) break;
    }
    if (context.size() == 1 && context[0] == -1) return {};
    std::sort(context.begin(), context.end(), [this](int a, int b) { return position_[a] < position_[b]; });
    return context;
}

std::string UiView::PathOf(int widget) const {
    std::vector<int> chain;
    for (int w = widget; w != -1; w = parent_[w]) chain.push_back(w);
    std::string path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += StepOf(*it);
    }
    return path;
}

std::string UiView::StepOf(int widget) const {
    const std::vector<int> siblings = parent_[widget] == -1 ? Roots() : Children(parent_[widget]);
    const std::string& name = tree_.widgets[widget].name;
    if (PlainName(name)) {
        int same = 0, index = 0;
        for (const int s : siblings) {
            if (SameName(tree_.widgets[s].name, name)) {
                ++same;
                if (s == widget) index = same;
            }
        }
        return same > 1 ? name + "[" + std::to_string(index) + "]" : name;
    }
    // Positional, as a step made only of filters: the n-th sibling of any name.
    const auto at = std::find(siblings.begin(), siblings.end(), widget);
    return "[" + std::to_string(at - siblings.begin() + 1) + "]";
}

std::string ParseUiRequest(std::string_view argument, UiRequest& out) {
    out = {};
    const auto fail = [](const char* code, const std::string& detail) {
        std::string members = std::string(",\"error\":\"") + code + "\",\"detail\":\"" + JsonEscape(detail) + "\"";
        if (std::string_view(code) == "bad_selector") members += ",\"hint\":\"" + JsonEscape(UiSelectorHint()) + "\"";
        return members;
    };
    std::string_view rest = Trim(argument);
    const std::size_t space = rest.find_first_of(" \t");
    const std::string_view verb = rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view{} : Trim(rest.substr(space));
    if (verb == "find") {
        out.verb = UiRequest::Verb::Find;
        out.limit = kUiFindLimit;
    } else if (verb == "tree") {
        out.verb = UiRequest::Verb::Tree;
        out.limit = kUiTreeLimit;
    } else if (verb == "click") {
        out.verb = UiRequest::Verb::Click;
    } else {
        return fail("bad_command",
                    "usage: ui find SEL [all] [stale] [limit=N] | ui tree [SEL] [depth=N] [limit=N] [all] | ui click SEL");
    }
    // The selector is the first word; spaces inside [...] belong to it.
    std::size_t end = 0;
    for (int depth = 0; end < rest.size(); ++end) {
        const char c = rest[end];
        if (c == '[') {
            ++depth;
        } else if (c == ']' && depth > 0) {
            --depth;
        } else if (c == '"' && depth > 0) {
            const std::size_t close = rest.find('"', end + 1);
            if (close == std::string_view::npos) {
                end = rest.size();
                break;
            }
            end = close;
        } else if (IsSpace(c) && depth == 0) {
            break;
        }
    }
    const auto is_option = [](std::string_view word) {
        return word == "all" || word == "stale" || word.starts_with("depth=") || word.starts_with("limit=") || word.starts_with("timeout=");
    };
    std::string_view first = rest.substr(0, end);
    std::string_view options = Trim(rest.substr(end));
    if (!first.empty() && is_option(first)) {
        options = rest;
        first = {};
    }
    if (out.verb != UiRequest::Verb::Tree && first.empty()) {
        return fail("bad_selector", std::string("ui ") + std::string(verb) + " needs a selector");
    }
    if (!first.empty()) {
        out.selector_text = std::string(first);
        if (std::string error = ParseUiSelector(first, out.selector); !error.empty()) return fail("bad_selector", error);
    }
    while (!options.empty()) {
        const std::size_t cut = options.find_first_of(" \t");
        const std::string_view word = options.substr(0, cut);
        options = cut == std::string_view::npos ? std::string_view{} : Trim(options.substr(cut));
        int number = 0;
        if (out.verb == UiRequest::Verb::Click && !word.starts_with("timeout=")) {
            return fail("bad_command", "bad option: " + std::string(word.substr(0, 40)) + " (ui click takes only timeout=1..600)");
        }
        if (word == "all") {
            out.all = true;
        } else if (word == "stale" && out.verb == UiRequest::Verb::Find) {
            out.stale = true;
        } else if (word.starts_with("limit=") && ParseNumber(word.substr(6), number) && number >= 1 &&
                   number <= kUiMaxLimit) {
            out.limit = number;
        } else if (word.starts_with("depth=") && out.verb == UiRequest::Verb::Tree && ParseNumber(word.substr(6), number) &&
                   number >= 0 && number <= kUiMaxDepth) {
            out.depth = number;
        } else if (word.starts_with("timeout=") && ParseNumber(word.substr(8), number) && number >= 1 && number <= 600) {
            out.timeout_ms = number * 1000;
        } else {
            return fail("bad_command", "bad option: " + std::string(word.substr(0, 40)) +
                                           (out.verb == UiRequest::Verb::Tree
                                                ? " (all, depth=0.." + Num(kUiMaxDepth) + ", limit=1.." + Num(kUiMaxLimit)
                                                : " (all, stale, limit=1.." + Num(kUiMaxLimit)) +
                                           ", timeout=1..600)");
        }
    }
    return {};
}

namespace {

// path, type, visible, rect, click_point, and text / enabled / checked / stale where they apply.
std::string WidgetMembers(const UiView& view, int widget) {
    const UiWidget& w = view.tree().widgets[widget];
    const UiRect& r = w.rect;
    std::string out = "\"path\":\"" + JsonEscape(view.PathOf(widget)) + "\",\"type\":\"" + UiTypeName(w.type) +
                      "\",\"visible\":" + (view.Visible(widget) ? "true" : "false");
    out += ",\"rect\":{\"x\":" + Num(r.x) + ",\"y\":" + Num(r.y) + ",\"w\":" + Num(r.w) + ",\"h\":" + Num(r.h) + "}";
    out += ",\"click_point\":{\"x\":" + Num(r.x + r.w / 2) + ",\"y\":" + Num(r.y + r.h / 2) + "}";
    if (w.has_text) out += ",\"text\":\"" + JsonEscape(w.text) + "\"";
    if (w.has_enabled) out += std::string(",\"enabled\":") + (w.enabled ? "true" : "false");
    if (w.has_checked) out += std::string(",\"checked\":") + (w.checked ? "true" : "false");
    if (w.stale) out += ",\"stale\":true";
    return out;
}

struct TreeWriter {
    const UiView& view;
    int depth_limit;
    int limit;
    int count = 0;
    bool truncated = false;

    // Appends the widget and what fits of its subtree; false when the limit was hit first.
    bool Write(int widget, int depth, std::string& out) {
        if (count >= limit) {
            truncated = true;
            return false;
        }
        ++count;
        out += "{\"name\":\"" + JsonEscape(view.tree().widgets[widget].name) + "\"," + WidgetMembers(view, widget);
        const std::vector<int> children = view.Children(widget);
        if (!children.empty()) {
            if (depth_limit >= 0 && depth >= depth_limit) {
                out += ",\"children_count\":" + Num(static_cast<int>(children.size()));
            } else {
                std::string inner;
                for (const int child : children) {
                    std::string one;
                    if (!Write(child, depth + 1, one)) break;
                    if (!inner.empty()) inner += ',';
                    inner += one;
                }
                if (!inner.empty()) out += ",\"children\":[" + inner + "]";
            }
        }
        out += '}';
        return true;
    }
};

}  // namespace

std::string UiAnswer(const UiTree& tree, const UiRequest& request, bool& found) {
    found = true;
    // The tree shows stale widgets, marked; find leaves them out unless asked.
    const UiView view(tree, request.all, request.verb == UiRequest::Verb::Tree || request.stale);
    const bool has_selector = !request.selector.steps.empty();
    std::string out;
    if (has_selector) out += ",\"selector\":\"" + JsonEscape(request.selector_text) + "\"";
    if (request.verb == UiRequest::Verb::Find) {
        const std::vector<int> hits = view.Resolve(request.selector);
        const int count = static_cast<int>(hits.size());
        out += ",\"count\":" + Num(count) + ",\"truncated\":" + (count > request.limit ? "true" : "false") + ",\"matches\":[";
        for (int i = 0; i < count && i < request.limit; ++i) {
            if (i) out += ',';
            out += "{" + WidgetMembers(view, hits[i]) + "}";
        }
        return out + "]";
    }
    std::vector<int> roots;
    if (has_selector) {
        // A match inside another match is shown once, inside it.
        std::vector<char> inside(tree.widgets.size(), 0);
        for (const int hit : view.Resolve(request.selector)) {
            if (inside[hit]) continue;
            roots.push_back(hit);
            std::vector<int> stack{hit};
            while (!stack.empty()) {
                const int w = stack.back();
                stack.pop_back();
                inside[w] = 1;
                for (const int child : view.Children(w)) stack.push_back(child);
            }
        }
        if (roots.empty()) {
            found = false;
            return out;
        }
    } else {
        roots = view.Roots();
    }
    TreeWriter writer{view, request.depth, request.limit};
    std::string widgets;
    for (const int root : roots) {
        std::string one;
        if (!writer.Write(root, 0, one)) break;
        if (!widgets.empty()) widgets += ',';
        widgets += one;
    }
    out += ",\"count\":" + Num(writer.count) + ",\"truncated\":" + (writer.truncated ? "true" : "false") +
           ",\"widgets\":[" + widgets + "]";
    return out;
}

UiClickTarget ResolveClickTarget(const UiTree& tree, const UiSelector& selector) {
    UiClickTarget t;
    const UiView view(tree, false);
    std::vector<int> hits = view.Resolve(selector);
    const UiView all(tree, true);
    const UiView stale(tree, true, true);
    const UiView* used = &view;
    if (hits.size() > 1) t.error = "ambiguous";
    if (hits.empty()) {
        hits = all.Resolve(selector);
        used = &all;
        t.error = "not_visible";
    }
    if (hits.empty()) {
        hits = stale.Resolve(selector);
        used = &stale;
        t.error = hits.empty() ? "not_found" : "stale";
    }
    t.count = static_cast<int>(hits.size());
    if (!t.error.empty()) {
        for (int i = 0; i < t.count && i < kUiClickPaths; ++i) t.paths.push_back(used->PathOf(hits[i]));
        return t;
    }
    t.widget = hits[0];
    t.path = view.PathOf(t.widget);
    const UiWidget& w = tree.widgets[t.widget];
    if (w.has_enabled && !w.enabled) t.error = "disabled";
    return t;
}

UiCoverCheck CheckCover(const UiTree& tree, std::uintptr_t target, std::uintptr_t hovered) {
    UiCoverCheck c;
    if (hovered == 0) return c;
    int hit = -1;
    for (int i = 0; i < static_cast<int>(tree.widgets.size()) && hit == -1; ++i) {
        if (tree.widgets[i].address == hovered) hit = i;
    }
    if (hit == -1) {
        c.covered = hovered != target;
        return c;
    }
    // The hovered widget may be stale: its path is named all the same.
    const UiView view(tree, false, true);
    const UiView all(tree, true, true);
    const std::string path = view.Visible(hit) ? view.PathOf(hit) : all.PathOf(hit);
    for (int w = hit; w != -1; w = all.Parent(w)) {
        if (tree.widgets[w].address == target) {
            c.covered = false;
            c.hovered = path;
            return c;
        }
    }
    c.covered_by = path;
    return c;
}

UiRect ChildrenBounds(const UiTree& tree, int parent) {
    const UiWidget& p = tree.widgets[parent];
    for (const bool shown_only : {true, false}) {
        int left = 0, top = 0, right = 0, bottom = 0;
        bool any = false;
        for (const int c : p.children) {
            const UiWidget& child = tree.widgets[c];
            const UiRect& r = child.rect;
            if (r.w <= 0 || r.h <= 0 || (shown_only && !child.shown)) continue;
            left = any ? std::min(left, r.x) : r.x;
            top = any ? std::min(top, r.y) : r.y;
            right = any ? std::max(right, r.x + r.w) : r.x + r.w;
            bottom = any ? std::max(bottom, r.y + r.h) : r.y + r.h;
            any = true;
        }
        if (any) return {left, top, right - left, bottom - top};
    }
    return p.rect;
}

void FitRects(UiTree& tree, UiRect client) {
    const bool known = client.w > 0 && client.h > 0;
    // Walk order from the top-level windows; each widget once.
    std::vector<int> order;
    std::vector<char> seen(tree.widgets.size(), 0);
    std::vector<int> stack(tree.roots.rbegin(), tree.roots.rend());
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        if (i < 0 || i >= static_cast<int>(tree.widgets.size()) || seen[i]) continue;
        seen[i] = 1;
        order.push_back(i);
        const std::vector<int>& children = tree.widgets[i].children;
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        UiWidget& w = tree.widgets[*it];
        UiRect& r = w.rect;
        const bool inside = r.x >= 0 && r.y >= 0 && r.x + r.w <= client.w && r.y + r.h <= client.h;
        if (w.type == UiType::Window && !w.children.empty() && (r.w <= 0 || r.h <= 0 || (known && !inside))) {
            r = ChildrenBounds(tree, *it);
        }
        if (!known) continue;
        const int left = std::max(r.x, 0), top = std::max(r.y, 0);
        const int right = std::min(r.x + r.w, client.w), bottom = std::min(r.y + r.h, client.h);
        if (r.w <= 0 || r.h <= 0) {
            if (r.x < 0 || r.y < 0 || r.x > client.w || r.y > client.h) w.shown = false;
        } else if (left < right && top < bottom) {
            r = {left, top, right - left, bottom - top};
        } else {
            w.shown = false;
        }
    }
}

std::string GameTextToUtf8(std::string_view text, bool colour_codes) {
    // 0x80-0x9F of Windows-1252; 0 marks the five bytes it leaves undefined (kept as U+0080-U+009F).
    static constexpr char16_t kHigh[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                           0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xA7 && !colour_codes) {  // a colour code: § and the character after it are not displayed
            ++i;
            continue;
        }
        char32_t cp = c;
        if (c >= 0x80 && c < 0xA0 && kHigh[c - 0x80] != 0) cp = kHigh[c - 0x80];
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace tongs
