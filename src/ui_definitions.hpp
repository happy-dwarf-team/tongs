#pragma once

#include "ui_tree.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
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

// Where a live definition was found: the slot that holds it (the value of a
// registry node, of a nested map's node, or a pointer field) and the definition
// that slot belongs to (0 for the registry).
struct DefinitionWitness {
    std::uintptr_t slot = 0;
    std::uintptr_t parent = 0;
};

// As above, and records the first witness of each definition in `witnesses`.
std::string CollectLiveDefinitions(std::uintptr_t registry, const TypeTreeLayout& layout, const ReadMemory& read,
                                   std::unordered_set<std::uintptr_t>& out,
                                   std::unordered_map<std::uintptr_t, DefinitionWitness>& witnesses);

// Tells whether single definitions are live without collecting all of them
// each time (tongs.ui's cheap hovered). It keeps the last collection with its
// witnesses: a known definition is live while every slot on its way up to the
// registry still holds it, a few reads. A reload replaces the registry's value,
// so the way up breaks for everything the old definition held. A definition it
// does not know, or whose way up broke, makes it collect again, at most once
// between two Begin(); a definition still unknown after that is stale until
// the next collection.
//
// A freed address that a later definition reuses can still pass, as in the
// whole collection, and so can a slot in freed memory that still holds its old
// value. So the answer decides what is read (the name and file strings of a
// definition), never what is called.
class LiveDefinitionCache {
public:
    void Begin() { collected_now_ = false; }
    // Sets `live`; returns an error text when a collection failed (then `live` is false).
    std::string IsLive(std::uintptr_t registry, const TypeTreeLayout& layout, const ReadMemory& read,
                       std::uintptr_t definition, bool& live);
    std::size_t collections() const { return collections_; }

private:
    bool Verify(std::uintptr_t definition, const ReadMemory& read) const;

    std::unordered_map<std::uintptr_t, DefinitionWitness> live_;
    std::unordered_set<std::uintptr_t> stale_;
    bool collected_now_ = false;
    std::size_t collections_ = 0;
};

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
