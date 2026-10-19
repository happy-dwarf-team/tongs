#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// The game's widget tree as read on the main thread (game_ui.cpp), and the
// selectors that find widgets in it (docs/design.md, "Widget tree"). Nothing
// here touches game memory, so the tests build trees by hand.

enum class UiType { Window, Button, Icon, Text, Listbox, Checkbox, Editbox, Other };

const char* UiTypeName(UiType type);

struct UiRect {
    int x = 0;  // client pixels of the game window
    int y = 0;
    int w = 0;
    int h = 0;
};

struct UiWidget {
    std::string name;  // the .gui name, case kept
    UiType type = UiType::Other;
    bool shown = false;  // the widget's own visible flag; `visible` in answers also needs every ancestor shown
    UiRect rect;
    bool has_text = false;
    std::string text;  // UTF-8
    bool has_enabled = false;
    bool enabled = false;
    bool has_checked = false;  // check boxes
    bool checked = false;
    std::vector<int> children;  // indices into UiTree::widgets, in the game's order
    std::uintptr_t address = 0;  // the widget pointer in the game: matches event buttons and the hovered widget; not reported
};

struct UiTree {
    std::vector<UiWidget> widgets;
    std::vector<int> roots;  // top-level windows, in the order of the game's widget registry
};

struct UiSelector {
    enum class Axis { Top, Child, Descendant };
    struct Filter {
        enum class Kind { Type, TextEquals, TextContains, TextStarts, Enabled, Disabled, Checked, Unchecked };
        Kind kind = Kind::Type;
        UiType type = UiType::Other;
        std::string value;  // lower case
    };
    struct Step {
        Axis axis = Axis::Top;
        std::string name;  // glob with * and ?, matched ignoring case
        std::vector<Filter> filters;
        int index = 0;  // [n], from 1; 0 = every match
    };
    std::vector<Step> steps;
};

// Returns an error text, or an empty string on success.
std::string ParseUiSelector(std::string_view text, UiSelector& out);

// A tree prepared for one command: walk order, effective visibility and the
// widgets a command may see (hidden ones only with `all`).
class UiView {
public:
    UiView(const UiTree& tree, bool include_hidden);

    // Matches in walk order (parents before children, siblings in the game's order).
    std::vector<int> Resolve(const UiSelector& selector) const;
    // The widget path: names from a top-level window down, with [n] where
    // siblings share a name. Resolve() of it with the same `all` finds the widget again.
    std::string PathOf(int widget) const;
    bool Visible(int widget) const { return visible_[widget] != 0; }
    bool Usable(int widget) const { return include_hidden_ || Visible(widget); }
    int Parent(int widget) const { return parent_[widget]; }
    // Usable children and top-level windows.
    std::vector<int> Children(int widget) const;
    std::vector<int> Roots() const;
    const UiTree& tree() const { return tree_; }

private:
    bool Matches(int widget, const UiSelector::Step& step) const;

    const UiTree& tree_;
    bool include_hidden_;
    std::vector<int> roots_;        // top-level windows, each once
    std::vector<int> order_;        // reachable widgets in walk order
    std::vector<int> position_;     // widget -> index in order_, -1 when unreachable
    std::vector<int> end_;          // widget -> index in order_ after its last descendant
    std::vector<int> parent_;       // -1 for top-level windows
    std::vector<char> visible_;
};

// `ui find SEL [all] [limit=N]`, `ui tree [SEL] [depth=N] [limit=N] [all]` and `ui click SEL`.
struct UiRequest {
    enum class Verb { Find, Tree, Click };
    Verb verb = Verb::Find;
    std::string selector_text;  // empty: none (tree only)
    UiSelector selector;
    bool all = false;
    int depth = -1;  // -1: no limit
    int limit = 0;
    int timeout_ms = 0;  // 0: the dispatcher's default
};

constexpr int kUiFindLimit = 50;
constexpr int kUiTreeLimit = 200;
constexpr int kUiMaxLimit = 5000;
constexpr int kUiMaxDepth = 64;

// Parses the words after `ui`. On failure returns the JSON members of the
// error (starting with a comma), otherwise an empty string.
std::string ParseUiRequest(std::string_view argument, UiRequest& out);

// The answer members (each starting with a comma) of a request run on a tree.
// Sets `found` to false when `ui tree SEL` matches nothing.
std::string UiAnswer(const UiTree& tree, const UiRequest& request, bool& found);

// The widget `ui click` would click, or why it refuses: "not_found", "ambiguous"
// (with up to 10 paths), "not_visible" (only hidden widgets match) or "disabled".
struct UiClickTarget {
    std::string error;
    std::vector<std::string> paths;  // ambiguous and not_visible: the matches, up to 10
    int count = 0;                   // all matches
    int widget = -1;
    std::string path;
};

constexpr int kUiClickPaths = 10;

UiClickTarget ResolveClickTarget(const UiTree& tree, const UiSelector& selector);

// After the pointer moved onto a target: the game hovers `hovered` (mgr+0x130,
// 0 = none). The click goes ahead only when that is the target or a widget
// inside it; otherwise `covered_by` names the hovered widget, when the tree has it.
struct UiCoverCheck {
    bool covered = true;
    std::string covered_by;  // path; empty when nothing is hovered or the widget is not in the tree
    std::string hovered;     // path of the hovered widget when it is the target or inside it
};

UiCoverCheck CheckCover(const UiTree& tree, std::uintptr_t target, std::uintptr_t hovered);

// The smallest rectangle around the children of `parent` that have a size; the
// parent's own rect when none has. Scroll bars have no render node, so their own
// position is not in client pixels; their buttons' are.
UiRect ChildrenBounds(const UiTree& tree, int parent);

// The game's 8-bit text (Windows-1252) as UTF-8, without its colour codes (§ and
// the next character, e.g. "§YName§!" -> "Name").
std::string GameTextToUtf8(std::string_view text);

}  // namespace tongs
