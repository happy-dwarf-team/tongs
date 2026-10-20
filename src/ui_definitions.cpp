#include "ui_definitions.hpp"

#include <cstring>

namespace tongs {
namespace {

// Adds the values of the map at `map` that are new to `out`, and queues them.
std::string WalkMap(std::uintptr_t map, const TypeTreeLayout& layout, const ReadMemory& read, std::size_t& nodes,
                    std::unordered_set<std::uintptr_t>& out, std::vector<std::uintptr_t>& found) {
    std::uintptr_t root = 0;
    if (!read(map + layout.root, &root, sizeof root)) return "the GUI type registry is unreadable";
    // Iterative, and bounded: a tree changing under us (never on the main thread) cannot loop forever.
    std::vector<std::uintptr_t> stack;
    if (root != 0) stack.push_back(root);
    unsigned char raw[kMaxTypeNodeSize];
    while (!stack.empty()) {
        const std::uintptr_t node = stack.back();
        stack.pop_back();
        if (++nodes > kMaxTypeTreeNodes) return "the GUI type registry has too many nodes";
        if (!read(node, raw, layout.node_size)) return "the GUI type registry is unreadable";
        const auto field = [&raw](unsigned at) {
            std::uintptr_t p = 0;
            std::memcpy(&p, raw + at, sizeof p);
            return p;
        };
        if (const std::uintptr_t value = field(layout.value); value != 0 && out.insert(value).second) found.push_back(value);
        for (const unsigned link : {layout.higher, layout.next, layout.lower}) {
            if (const std::uintptr_t child = field(link); child != 0) stack.push_back(child);
        }
    }
    return {};
}

}  // namespace

std::string CollectLiveDefinitions(std::uintptr_t registry, const TypeTreeLayout& layout, const ReadMemory& read,
                                   std::unordered_set<std::uintptr_t>& out) {
    const unsigned fields[] = {layout.value, layout.lower, layout.higher, layout.next};
    if (layout.node_size == 0 || layout.node_size > kMaxTypeNodeSize) return "bad type registry layout";
    for (const unsigned field : fields) {
        if (field > layout.node_size - sizeof(std::uintptr_t)) return "bad type registry layout";
    }
    std::size_t nodes = 0;
    std::vector<std::uintptr_t> pending;
    if (std::string error = WalkMap(registry, layout, read, nodes, out, pending); !error.empty()) return error;
    // Every definition in `pending` is live, so reading it is safe. Its nested
    // map counts only when the class is known and the map's vtable is in place.
    while (!pending.empty()) {
        const std::uintptr_t definition = pending.back();
        pending.pop_back();
        std::uintptr_t vtable = 0;
        if (!read(definition, &vtable, sizeof vtable)) return "a live GUI definition is unreadable";
        for (const TypeTreeLayout::Nested& n : layout.nested) {
            if (n.vtable != vtable) continue;
            std::uintptr_t map_vtable = 0;
            if (!read(definition + n.map, &map_vtable, sizeof map_vtable)) return "a live GUI definition is unreadable";
            if (map_vtable != layout.map_vtable) continue;
            if (std::string error = WalkMap(definition + n.map, layout, read, nodes, out, pending); !error.empty()) {
                return error;
            }
        }
    }
    return {};
}

UiNamePlan PlanWidgetName(bool definition_live, bool has_holder_name, bool has_override) {
    using Source = UiNamePlan::Source;
    if (definition_live) return {Source::Definition, false};
    if (has_holder_name) return {Source::Holder, true};
    if (has_override) return {Source::Override, true};
    return {Source::None, true};
}

void PropagateStale(UiTree& tree) {
    const int n = static_cast<int>(tree.widgets.size());
    std::vector<char> seen(n, 0);
    std::vector<int> stack;
    for (int i = 0; i < n; ++i) {
        if (!tree.widgets[i].stale || seen[i]) continue;
        stack.push_back(i);
        while (!stack.empty()) {
            const int w = stack.back();
            stack.pop_back();
            if (seen[w]) continue;
            seen[w] = 1;
            tree.widgets[w].stale = true;
            for (const int child : tree.widgets[w].children) {
                if (child >= 0 && child < n && !seen[child]) stack.push_back(child);
            }
        }
    }
}

}  // namespace tongs
