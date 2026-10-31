#include "ui_service.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tongs {
namespace {

// UiType counts from 0 in the order of TongsUiType, which counts from 1.
static_assert(static_cast<int>(UiType::Window) + 1 == TONGS_UI_WINDOW &&
              static_cast<int>(UiType::Other) + 1 == TONGS_UI_OTHER);

// One visit; `path` may be null. False when the consumer stopped.
bool Visit(const UiView& view, int widget, int parent, std::uint32_t extra, std::uintptr_t hovered, const std::string* path,
           TongsUiVisit visit, void* context) {
    const UiWidget& w = view.tree().widgets[widget];
    TongsUiWidget out{};
    out.size = sizeof out;
    out.type = static_cast<std::uint32_t>(w.type) + 1;
    out.flags = extra;
    if (view.Visible(widget)) out.flags |= TONGS_UI_VISIBLE;
    if (!w.has_enabled || w.enabled) out.flags |= TONGS_UI_ENABLED;
    if (w.has_checked && w.checked) out.flags |= TONGS_UI_CHECKED;
    if (w.stale) out.flags |= TONGS_UI_IS_STALE;
    if (hovered != 0 && w.address == hovered) out.flags |= TONGS_UI_HOVERED;
    out.parent = parent;
    out.x = w.rect.x;
    out.y = w.rect.y;
    out.width = w.rect.w;
    out.height = w.rect.h;
    out.id = w.address;
    out.name = w.name.c_str();
    out.path = path ? path->c_str() : nullptr;
    out.text = w.has_text ? w.text.c_str() : nullptr;
    out.markup = w.has_text ? w.markup.c_str() : nullptr;
    // A stale widget's own definition may be live, but the window it is in was built from a freed one.
    out.definition_file = w.has_definition_file && !w.stale ? w.definition_file.c_str() : nullptr;
    out.definition_line = w.has_definition_file && !w.stale ? w.definition_line : 0;
    return visit(context, &out) != 0;
}

// Marks a visit running, so the consumer cannot call the service from inside it.
class Visiting {
public:
    explicit Visiting(bool& flag) : flag_(flag) { flag_ = true; }
    ~Visiting() { flag_ = false; }
    Visiting(const Visiting&) = delete;
    Visiting& operator=(const Visiting&) = delete;

private:
    bool& flag_;
};

}  // namespace

std::uint32_t VisitUiFind(const UiTree& tree, const UiSelector& selector, std::uint32_t flags, std::uintptr_t hovered,
                          TongsUiVisit visit, void* context) {
    const UiView view(tree, (flags & TONGS_UI_HIDDEN) != 0, (flags & TONGS_UI_STALE) != 0);
    const std::vector<int> hits = view.Resolve(selector);
    // Matches come in walk order, so an ancestor is either an earlier match or
    // visited right before the first match inside it; a usable widget's
    // ancestors are usable (shown and stale both pass down).
    std::vector<int> index(tree.widgets.size(), -1);  // widget -> its visit
    std::vector<std::string> paths;
    std::vector<int> chain;
    for (const int hit : hits) {
        chain.clear();
        for (int w = hit; w != -1 && index[w] == -1; w = view.Parent(w)) chain.push_back(w);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const int w = *it;
            const int parent = view.Parent(w) == -1 ? -1 : index[view.Parent(w)];
            paths.push_back(parent == -1 ? view.StepOf(w) : paths[parent] + "/" + view.StepOf(w));
            index[w] = static_cast<int>(paths.size()) - 1;
            if (!Visit(view, w, parent, w == hit ? TONGS_UI_MATCH : 0u, hovered, &paths.back(), visit, context)) {
                return static_cast<std::uint32_t>(hits.size());
            }
        }
    }
    return static_cast<std::uint32_t>(hits.size());
}

void VisitUiHovered(const UiTree& tree, std::uintptr_t hovered, std::uint32_t flags, TongsUiVisit visit, void* context) {
    if (hovered == 0) return;
    int hit = -1;
    for (int i = 0; i < static_cast<int>(tree.widgets.size()) && hit == -1; ++i) {
        if (tree.widgets[i].address == hovered) hit = i;
    }
    const UiView view(tree, (flags & TONGS_UI_HIDDEN) != 0, (flags & TONGS_UI_STALE) != 0);
    // A widget the walk does not reach (a malformed tree) has no ancestors to give.
    if (hit == -1 || !view.Usable(hit) || (view.Parent(hit) == -1 && !std::count(tree.roots.begin(), tree.roots.end(), hit))) {
        return;
    }
    std::vector<int> chain;
    for (int w = hit; w != -1; w = view.Parent(w)) chain.push_back(w);
    const bool paths = (flags & TONGS_UI_PATHS) != 0;
    for (std::size_t k = 0; k < chain.size(); ++k) {
        const int parent = k + 1 < chain.size() ? static_cast<int>(k) + 1 : -1;
        std::string path;
        if (paths) path = view.PathOf(chain[k]);
        if (!Visit(view, chain[k], parent, 0, hovered, paths ? &path : nullptr, visit, context)) return;
    }
}

AnvilResult UiService::Admit() const {
    const std::uint32_t main = host_.main_thread();
    if (main == 0 || host_.current_thread() != main || visiting_) return ANVIL_ERROR_WRONG_PHASE;
    return ANVIL_OK;
}

AnvilResult UiService::Report(std::string error) {
    if (error.empty()) {
        last_error_.clear();
        return ANVIL_OK;
    }
    if (error != last_error_) host_.log_error("tongs.ui: " + error);
    last_error_ = std::move(error);
    return ANVIL_ERROR_INTERNAL;
}

AnvilResult UiService::Find(const char* selector, std::uint32_t flags, TongsUiVisit visit, void* context,
                            std::uint32_t* count) {
    if (count) *count = 0;
    UiSelector parsed;
    if (selector == nullptr || visit == nullptr || (flags & ~kUiServiceFlags) != 0 ||
        !ParseUiSelector(selector, parsed).empty()) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if (const AnvilResult admitted = Admit(); admitted != ANVIL_OK) return admitted;
    try {
        UiTree tree;
        if (const AnvilResult read = Report(host_.read_tree(tree)); read != ANVIL_OK) return read;
        const std::uintptr_t hovered = host_.read_hovered();
        const Visiting guard(visiting_);
        const std::uint32_t matches = VisitUiFind(tree, parsed, flags, hovered, visit, context);
        if (count) *count = matches;
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    } catch (const std::exception&) {
        return ANVIL_ERROR_INTERNAL;
    }
}

AnvilResult UiService::Hovered(std::uint32_t flags, TongsUiVisit visit, void* context) {
    if (visit == nullptr || (flags & ~kUiServiceFlags) != 0) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (const AnvilResult admitted = Admit(); admitted != ANVIL_OK) return admitted;
    try {
        UiTree tree;
        std::uintptr_t hovered = 0;
        bool complete = false;
        if ((flags & TONGS_UI_PATHS) == 0 && host_.read_hovered_chain) {
            // A few reads up from the hovered widget instead of the whole tree.
            if (const AnvilResult read = Report(host_.read_hovered_chain(tree, hovered, complete)); read != ANVIL_OK) {
                return read;
            }
        }
        if (!complete) {
            hovered = host_.read_hovered();
            if (hovered == 0) return ANVIL_OK;
            if (const AnvilResult read = Report(host_.read_tree(tree)); read != ANVIL_OK) return read;
        }
        const Visiting guard(visiting_);
        VisitUiHovered(tree, hovered, flags, visit, context);
        return ANVIL_OK;
    } catch (const std::bad_alloc&) {
        return ANVIL_ERROR_OUT_OF_MEMORY;
    } catch (const std::exception&) {
        return ANVIL_ERROR_INTERNAL;
    }
}

}  // namespace tongs
