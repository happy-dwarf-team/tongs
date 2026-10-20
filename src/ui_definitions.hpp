#pragma once

#include "ui_tree.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace tongs {

// Stale widgets (docs/design.md, "Stale widgets"). A reload of a .gui file
// deletes its old definitions, but widgets the game does not rebuild keep
// pointing at them, and asking such a widget for its name (slot 0xA0) calls
// through the freed definition. The tree reader therefore asks only widgets
// whose definition is live: in the GUI type registry, or nested in a live
// definition. Nothing here touches game memory itself, so the tests run it over
// fake memory.

// The type registry at mgr+0xC8 is a case-insensitive ternary search tree from
// a top-level .gui name to its definition. Window, container, list box and
// scroll bar definitions keep their nested definitions in a map of the same
// class; extended scroll bars and containers also hold button definitions in
// plain pointer fields. A definition is read only once it is known to be live.
struct TypeTreeLayout {
    unsigned root = 0;       // map: the root node pointer
    unsigned node_size = 0;  // bytes of a node
    unsigned value = 0;      // node: the definition, 0 where no name ends
    unsigned lower = 0;      // node: the subtree of smaller characters at this position
    unsigned higher = 0;     // and of larger ones
    unsigned next = 0;       // the subtree of the following character
    std::uintptr_t map_vtable = 0;  // the map class's vtable (address in the game)
    // Definition classes (by vtable address) and the offset of their nested map.
    struct Nested {
        std::uintptr_t vtable = 0;
        unsigned map = 0;
    };
    std::vector<Nested> nested;
    // Definition classes (by vtable address) with a pointer field to a definition
    // that lives as long as they do, and the class that definition must have.
    struct Pointer {
        std::uintptr_t vtable = 0;
        unsigned field = 0;
        std::uintptr_t target = 0;
    };
    std::vector<Pointer> pointers;
};

// Copies `size` bytes of game memory; false when they are unreadable.
using ReadMemory = std::function<bool(std::uintptr_t address, void* out, std::size_t size)>;

constexpr std::size_t kMaxTypeTreeNodes = std::size_t{1} << 22;  // all maps together; vanilla: about 9 000 definitions
constexpr unsigned kMaxTypeNodeSize = 0x40;

// Adds every live definition to `out`: the registry's values, then the values of
// the nested maps and pointer fields of every definition found, recursively. Returns an error
// text, or empty on success; on an error `out` is incomplete and must not be used.
std::string CollectLiveDefinitions(std::uintptr_t registry, const TypeTreeLayout& layout, const ReadMemory& read,
                                   std::unordered_set<std::uintptr_t>& out);

struct UiNamePlan {
    enum class Source { None, Definition, Holder, Override };
    Source source = Source::None;
    bool stale = false;  // built from a definition that is no longer live
};

// `definition_live`: the widget's definition (+0xB0) is live, so slot 0xA0 may be called.
// `has_holder_name`: a window's or container's child list names the widget.
// `has_override`: a window's or container's own name (+0xA28 / +0x5F8) is set.
UiNamePlan PlanWidgetName(bool definition_live, bool has_holder_name, bool has_override);

// Marks everything inside a stale widget stale too.
void PropagateStale(UiTree& tree);

}  // namespace tongs
