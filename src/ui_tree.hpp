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

// What a widget's class does with a left click the game hands it (its
// ProcessInput; docs/design.md, "Clicks through widgets").
enum class UiClickInput {
    Takes,   // acts on it (check and edit boxes, scroll bar sliders) or a class Tongs has not checked
    Passes,  // windows, text boxes, list boxes, scroll bars: only the wheel, or every event with takes_all_input
    Button,  // buttons and icons (CButton): acts on it when `clickable`
};

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
    std::string text;    // UTF-8
    std::string markup;  // UTF-8 with the colour codes kept (§ as U+00A7); tongs.ui only
    bool has_enabled = false;
    bool enabled = false;
    bool has_checked = false;  // check boxes
    bool checked = false;
    bool stale = false;  // built from a definition a reload freed, or inside such a widget (ui_definitions.hpp)
    bool has_definition_file = false;  // only when the widget's definition is live
    std::string definition_file;       // the game path of the defining .gui file, UTF-8; tongs.ui only
    std::uint32_t definition_line = 0;  // the line of its opening `{` in that file, 0 when unknown; tongs.ui only
    // How a click under the pointer reaches it or passes it (CheckCover); not reported.
    UiClickInput click_input = UiClickInput::Takes;
    bool clickable = false;        // Button: the left or right click byte is set
    bool takes_all_input = false;  // consumes every mouse event it gets (slot 0x18)
    bool pointer_over = false;     // the game had it under the pointer on the last frame (its copy of +0x8F)
    std::vector<int> children;  // indices into UiTree::widgets, in the game's order
    std::uintptr_t address = 0;  // the widget pointer in the game: matches event buttons and the hovered widget; not reported
};

struct UiTree {
    std::vector<UiWidget> widgets;
    std::vector<int> roots;  // top-level windows, in the order of the game's widget registry
    // The game's GUI scale (game_ui_scale) the rects were multiplied by, from the
    // game's own value; 0 when it could not be read, and the rects are then the
    // game's own coordinates, which are client pixels only at scale 1.
    float ui_scale = 0;
};

// The GUI scale read from the game, or 0 for a value no game setting gives
// (not finite, or outside 0.25-8).
float PlausibleUiScale(float scale);

// A rect in the game's interface coordinates (client pixels divided by the GUI
// scale) as client pixels: the edges are multiplied by `scale` and rounded, so
// adjacent widgets stay adjacent. A scale of 0 or 1 leaves it as it is.
UiRect ScaleRect(const UiRect& r, float scale);

// `,"ui_scale":1.2` for an answer, or `,"ui_scale":null` when it was not read.
std::string UiScaleMember(const UiTree& tree);

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

// The grammar in short, sent as "hint" with every bad_selector answer and built
// from the tables below, which the tests run through ParseUiSelector.
std::string UiSelectorHint();

struct UiSelectorFilterNote {
    std::string_view filter;   // as it stands after a name
    std::string_view meaning;  // empty: the filter says it
};

inline constexpr UiSelectorFilterNote kUiSelectorFilters[] = {
    {"[2]", "the 2nd match, from 1"},
    {"[type=button]", ""},  // the hint adds the type names
    {"[text=Save]", "the whole text, any case; \"...\" for ] inside"},
    {"[text~=Save]", "contains"},
    {"[text^=Save]", "starts with"},
    {"[enabled]", ""},
    {"[disabled]", ""},
    {"[checked]", ""},
    {"[unchecked]", ""},
};

inline constexpr std::string_view kUiSelectorExamples[] = {
    "speed_controls/button_speedup",
    "EventWindow//event_option_button[2]",
    "//[type=button][enabled]",
};

// Mistakes agents make (XPath, globs) and the selector they meant.
struct UiSelectorFix {
    std::string_view wrong;
    std::string_view right;
};

inline constexpr UiSelectorFix kUiSelectorFixes[] = {
    {"//*[name='text_MIL']", "//text_MIL"},
    {"//*[@text='Save']", "//[text=Save]"},
    {"**/*[Ss]ave*", "//*save*"},
};

// A tree prepared for one command: walk order, effective visibility and the
// widgets a command may see (hidden ones only with `all`, stale ones only when asked).
class UiView {
public:
    UiView(const UiTree& tree, bool include_hidden, bool include_stale = false);

    // Matches in walk order (parents before children, siblings in the game's order).
    std::vector<int> Resolve(const UiSelector& selector) const;
    // The widget path: names from a top-level window down, with [n] where
    // siblings share a name. Resolve() of it with the same `all` finds the widget again.
    std::string PathOf(int widget) const;
    // The last step of PathOf: the widget's name, or position, among its siblings.
    std::string StepOf(int widget) const;
    bool Visible(int widget) const { return visible_[widget] != 0; }
    bool Usable(int widget) const {
        return (include_hidden_ || Visible(widget)) && (include_stale_ || !tree_.widgets[widget].stale);
    }
    int Parent(int widget) const { return parent_[widget]; }
    // Usable children and top-level windows.
    std::vector<int> Children(int widget) const;
    std::vector<int> Roots() const;
    const UiTree& tree() const { return tree_; }

private:
    bool Matches(int widget, const UiSelector::Step& step) const;

    const UiTree& tree_;
    bool include_hidden_;
    bool include_stale_;
    std::vector<int> roots_;        // top-level windows, each once
    std::vector<int> order_;        // reachable widgets in walk order
    std::vector<int> position_;     // widget -> index in order_, -1 when unreachable
    std::vector<int> end_;          // widget -> index in order_ after its last descendant
    std::vector<int> parent_;       // -1 for top-level windows
    std::vector<char> visible_;
};

// `ui find SEL [all] [stale] [limit=N]`, `ui tree [SEL] [depth=N] [limit=N] [all]` and `ui click SEL`.
// `ui tree` shows stale widgets (marked "stale"); `ui find` only with `stale`; `ui click` never.
struct UiRequest {
    enum class Verb { Find, Tree, Click };
    Verb verb = Verb::Find;
    std::string selector_text;  // empty: none (tree only)
    UiSelector selector;
    bool all = false;
    bool stale = false;  // ui find: include stale widgets
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
// (with up to 10 paths), "not_visible" (only hidden widgets match), "stale" (only
// stale widgets match: hovering one may make the game read its freed definition)
// or "disabled".
struct UiClickTarget {
    std::string error;
    std::vector<std::string> paths;  // ambiguous, not_visible and stale: the matches, up to 10
    int count = 0;                   // all matches
    int widget = -1;
    std::string path;
};

constexpr int kUiClickPaths = 10;

UiClickTarget ResolveClickTarget(const UiTree& tree, const UiSelector& selector);

// After the pointer moved onto a target: the game hovers `hovered` (mgr+0x130,
// 0 = none), the topmost of the widgets under the pointer `under_pointer`
// (topmost first, as the game hands them a click). The click goes ahead when
// the hovered widget is the target or a widget inside it, or when every widget
// above the target lets the click through (shown, not takes_all_input, and a
// class that passes it or a button that is not clickable) and the game had the
// target under the pointer (pointer_over). Otherwise `covered_by` names the
// widget that would take the click: the first above the target that does not
// let it through, else the hovered widget, when the tree has it.
struct UiCoverCheck {
    bool covered = true;
    std::string covered_by;  // path; empty when nothing is hovered or the widget is not in the tree
    std::string hovered;     // path of the hovered widget when the click goes ahead
    std::vector<std::string> through;  // the widgets above the target the click passes, topmost first
};

UiCoverCheck CheckCover(const UiTree& tree, std::uintptr_t target, std::uintptr_t hovered,
                        const std::vector<std::uintptr_t>& under_pointer = {});

// The smallest rectangle around the children of `parent` that have a size,
// the shown ones when any of them has; the parent's own rect when none has.
// Scroll bars have no render node, so their own position is not in client
// pixels; their buttons' are.
UiRect ChildrenBounds(const UiTree& tree, int parent);

// Fits the rects to what the game draws on its client area `client` (w x h;
// 0 x 0 when unknown), children before their parents: a window without a size
// of its own, or not inside the client area (a fullscreen window), takes the
// bounds of its children; then every rect is clipped to the client area, and
// a widget wholly outside it is not shown (the game parks some widgets far
// off the screen).
void FitRects(UiTree& tree, UiRect client);

// The game's 8-bit text (Windows-1252) as UTF-8, without its colour codes (§ and
// the next character, e.g. "§YName§!" -> "Name"), or with them when `colour_codes`.
// Text that a mod keeps as UTF-8 (Mei's Chinese) is read as UTF-8: a string whose
// bytes from 0x80 up are all well-formed UTF-8, apart from the game's lone format
// bytes (§ £ ¤), with at least one sequence of three or four bytes or one for
// U+0080-U+00FF; the lone bytes in it are still Windows-1252 (docs/design.md,
// "Text encoding").
// The path of the widget at `address` (the hovered widget), named as CheckCover names
// widgets: its visible path, or its path among all widgets when it is hidden; empty when
// the tree does not have it.
std::string WidgetPathOf(const UiTree& tree, std::uintptr_t address);

std::string GameTextToUtf8(std::string_view text, bool colour_codes = false);

}  // namespace tongs
