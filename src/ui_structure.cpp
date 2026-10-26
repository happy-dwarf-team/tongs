#include "ui_structure.hpp"

#include <algorithm>
#include <utility>

namespace tongs {
namespace {

constexpr std::size_t kMaxChildren = 20000;  // per container
constexpr int kMaxNodeClimb = 8;
constexpr int kMaxDepth = 256;

template <typename T>
bool Get(const ReadMemory& read, std::uintptr_t address, T& out) {
    return read(address, &out, sizeof(T));
}

// A std::vector at `at` (begin, end): the pointer at `offset` of each element of
// `stride` bytes, and the std::string at `name` of the same element.
void ReadVector(const ReadMemory& read, std::uintptr_t at, std::size_t stride, std::size_t offset, std::size_t name,
                std::vector<UiHeld>& out) {
    std::uintptr_t begin = 0, end = 0;
    if (!Get(read, at, begin) || !Get(read, at + 8, end) || end < begin || (end - begin) % stride != 0) return;
    const std::size_t count = (end - begin) / stride;
    if (count > kMaxChildren) return;
    for (std::size_t i = 0; i < count; ++i) {
        UiHeld h;
        if (!Get(read, begin + i * stride + offset, h.pointer) || h.pointer == 0) continue;
        h.name = begin + i * stride + name;
        out.push_back(h);
    }
}

// A vector pair {pointers, names}: the names vector holds a std::string per pointer, in the same order.
void ReadPair(const ReadMemory& read, std::uintptr_t at, const UiStructureLayout& l, std::vector<UiHeld>& out) {
    std::uintptr_t begin = 0, end = 0, names = 0, names_end = 0;
    if (!Get(read, at, begin) || !Get(read, at + 8, end) || end < begin || (end - begin) % 8 != 0) return;
    const std::size_t count = (end - begin) / 8;
    if (count > kMaxChildren) return;
    // Names only when both vectors have the same length; otherwise the children come without one.
    const bool named = Get(read, at + l.pair_names, names) && Get(read, at + l.pair_names + 8, names_end) &&
                       names_end >= names && names_end - names == count * l.name_size;
    for (std::size_t i = 0; i < count; ++i) {
        UiHeld h;
        if (!Get(read, begin + i * 8, h.pointer) || h.pointer == 0) continue;
        if (named) h.name = names + i * l.name_size;
        out.push_back(h);
    }
}

// An insertion list of nodes {entry*, prev, next}: the pointer at `value` in each
// entry, and the std::string at `name` when `named`.
void ReadList(const ReadMemory& read, std::uintptr_t head, const UiStructureLayout& l, unsigned value, bool named,
              unsigned name, std::vector<UiHeld>& out) {
    std::uintptr_t node = 0;
    if (!Get(read, head, node)) return;
    for (std::size_t i = 0; node != 0 && i < kMaxChildren; ++i) {
        std::uintptr_t entry = 0;
        UiHeld h;
        if (Get(read, node, entry) && entry != 0 && Get(read, entry + value, h.pointer) && h.pointer != 0) {
            if (named) h.name = entry + name;
            out.push_back(h);
        }
        if (!Get(read, node + l.list_node_next, node)) return;
    }
}

// A std::vector at `at` of item pointers: the pointer at `value` in each item.
void ReadItems(const ReadMemory& read, std::uintptr_t at, unsigned value, std::vector<UiHeld>& out) {
    std::uintptr_t begin = 0, end = 0;
    if (!Get(read, at, begin) || !Get(read, at + 8, end) || end < begin || (end - begin) % 8 != 0) return;
    const std::size_t count = (end - begin) / 8;
    if (count > kMaxChildren) return;
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t item = 0;
        UiHeld h;
        if (Get(read, begin + i * 8, item) && item != 0 && Get(read, item + value, h.pointer) && h.pointer != 0) {
            out.push_back(h);
        }
    }
}

// Boxes and scroll bars: the holders a render-node climb skips (a list box's
// node may have no owner, a scroll bar has no node).
bool Inner(const UiKind* kind) {
    using C = UiKind::Children;
    return kind && (kind->children == C::Listbox || kind->children == C::SmoothListbox ||
                    kind->children == C::Overlapping || kind->children == C::Grid || kind->children == C::Scrollbar);
}

}  // namespace

std::vector<UiHeld> HeldBy(std::uintptr_t widget, const UiKind& kind, const UiStructureLayout& l, const ReadMemory& read) {
    std::vector<UiHeld> out;
    const std::uintptr_t full = widget - l.window_part;
    switch (kind.children) {
        case UiKind::Children::Window:
            for (std::size_t i = 0; i < l.window_pair_count; ++i) {
                ReadPair(read, widget + l.window_pairs + i * l.pair_stride, l, out);
            }
            for (std::size_t i = 0; i < l.window_map_count; ++i) {
                ReadList(read, full + l.window_maps + i * l.map_stride + l.map_list_head, l, 0, true, l.map_entry_name, out);
            }
            ReadVector(read, full + l.window_subwindows, l.subwindow_size, l.subwindow_pointer, l.subwindow_name, out);
            break;
        case UiKind::Children::Container:
            for (std::size_t i = 0; i < l.container_pair_count; ++i) {
                ReadPair(read, widget + l.container_pairs + i * l.pair_stride, l, out);
            }
            break;
        case UiKind::Children::Listbox:
        case UiKind::Children::SmoothListbox: {
            const bool smooth = kind.children == UiKind::Children::SmoothListbox;
            ReadList(read, widget + l.list_items, l, smooth ? l.smooth_item_window : l.listbox_item_window, false, 0, out);
            UiHeld bar;
            if (Get(read, widget + (smooth ? l.smooth_scrollbar : l.listbox_scrollbar), bar.pointer) && bar.pointer != 0) {
                out.push_back(bar);
            }
            break;
        }
        case UiKind::Children::Overlapping:
            ReadList(read, widget + l.overlapping_items, l, l.overlapping_item_window, false, 0, out);
            break;
        case UiKind::Children::Grid: {
            ReadItems(read, widget + l.grid_items, l.grid_item_window, out);
            UiHeld bar;
            if (Get(read, widget + l.grid_scrollbar, bar.pointer) && bar.pointer != 0) out.push_back(bar);
            break;
        }
        case UiKind::Children::Scrollbar:
            for (const unsigned at : l.scrollbar_parts) {
                UiHeld part;
                if (Get(read, widget + at, part.pointer) && part.pointer != 0) out.push_back(part);
            }
            break;
        case UiKind::Children::None:
            break;
    }
    return out;
}

UiStructure::UiStructure(const UiStructureLayout& layout, std::uintptr_t image_base, ReadMemory read)
    : l_(layout), base_(image_base), read_(std::move(read)) {
    for (const UiKind& k : layout.kinds) kinds_.emplace(k.vtable, &k);
}

std::string UiStructure::CopyRegistry(std::uintptr_t manager) {
    std::uintptr_t begin = 0, end = 0;
    if (!Get(read_, manager + l_.registry_begin, begin) || !Get(read_, manager + l_.registry_end, end) || end < begin ||
        (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxUiWidgets) {
        return "the widget registry is unreadable";
    }
    registry_.assign((end - begin) / 8, 0);
    if (!registry_.empty() && !read_(begin, registry_.data(), registry_.size() * 8)) {
        registry_.clear();
        return "the widget registry is unreadable";
    }
    return {};
}

const UiKind* UiStructure::KindOf(std::uintptr_t widget) const {
    std::uintptr_t vtable = 0;
    if (!Get(read_, widget, vtable)) return nullptr;
    const auto kind = kinds_.find(static_cast<unsigned>(vtable - base_));
    return kind == kinds_.end() ? nullptr : kind->second;
}

int UiStructure::Find(std::uintptr_t p) const {
    auto it = index_.find(p);
    if (it == index_.end()) it = index_.find(p + l_.window_part);
    return it == index_.end() ? -1 : it->second;
}

bool UiStructure::LiveWidget(std::uintptr_t p, std::uintptr_t& widget, const UiKind*& kind) const {
    if (p == 0) return false;
    for (const std::uintptr_t candidate : {p, p + l_.window_part}) {
        if (std::find(registry_.begin(), registry_.end(), candidate) == registry_.end()) continue;
        std::uint8_t dead = 1;
        std::uintptr_t vtable = 0;
        if (!Get(read_, candidate, vtable) || !Get(read_, candidate + l_.dead, dead) || dead != 0) return false;
        widget = candidate;
        kind = KindOf(candidate);
        return true;
    }
    return false;
}

std::string UiStructure::ReadAll(std::uintptr_t manager) {
    entries_.clear();
    index_.clear();
    roots_.clear();
    if (std::string error = CopyRegistry(manager); !error.empty()) return error;
    entries_.reserve(registry_.size());
    for (const std::uintptr_t p : registry_) {
        std::uintptr_t vtable = 0;
        std::uint8_t dead = 1;
        if (p == 0 || !Get(read_, p, vtable) || !Get(read_, p + l_.dead, dead) || dead != 0 || index_.contains(p)) continue;
        index_.emplace(p, static_cast<int>(entries_.size()));
        entries_.push_back({p, KindOf(p)});
    }
    children_.assign(entries_.size(), {});
    // Children held by windows, list, overlapping and grid boxes, and scroll bars: the first holder wins.
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (!entries_[i].kind) continue;
        for (const UiHeld& h : HeldBy(entries_[i].pointer, *entries_[i].kind, l_, read_)) Adopt(i, Find(h.pointer), &h);
    }
    // Widgets the game's own code holds: under the owner of their parent render node.
    // A widget without a node (a scroll bar) goes where its first child's node points.
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].parent != -1) continue;
        std::uintptr_t owner = NodeOwnerAbove(entries_[i].pointer, entries_[i].kind);
        const bool has_node = entries_[i].kind && entries_[i].kind->node != 0;
        for (std::size_t c = 0; owner == 0 && !has_node && c < children_[i].size(); ++c) {
            const UiEntry& child = entries_[children_[i][c]];
            owner = NodeOwnerAbove(child.pointer, child.kind);
        }
        if (owner != 0) Adopt(Find(owner), i, nullptr);
    }
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].parent == -1) roots_.push_back(i);
    }
    return {};
}

void UiStructure::Adopt(int parent, int child, const UiHeld* held) {
    if (child < 0 || parent < 0 || child == parent || entries_[child].parent != -1) return;
    int depth = 0;
    for (int up = parent; up != -1; up = entries_[up].parent) {
        if (up == child || ++depth > kMaxDepth) return;
    }
    UiEntry& e = entries_[child];
    e.parent = parent;
    if (held) e.held_name = held->name;
    children_[parent].push_back(child);
}

std::uintptr_t UiStructure::NodeOwnerAbove(std::uintptr_t widget, const UiKind* kind) const {
    std::uintptr_t node = 0, owner = 0;
    // A list box's node may have no owner set.
    if (!kind || kind->node == 0 || !Get(read_, widget + kind->node, node) || node == 0 ||
        !Get(read_, node + l_.node_owner, owner) || (owner != widget && owner != 0)) {
        return 0;
    }
    for (int step = 0; step < kMaxNodeClimb; ++step) {
        if (!Get(read_, node + l_.node_parent, node) || node == 0 || !Get(read_, node + l_.node_owner, owner)) return 0;
        if (owner == 0) continue;
        if (!index_.empty()) {
            // ReadAll: an owner counts when it is a widget of the index.
            const int found = Find(owner);
            if (found != -1 && entries_[found].pointer != widget) return entries_[found].pointer;
            continue;
        }
        std::uintptr_t live = 0;
        const UiKind* live_kind = nullptr;
        if (LiveWidget(owner, live, live_kind) && live != widget) return live;
    }
    return 0;
}

bool UiStructure::Holds(std::uintptr_t holder, const UiKind* kind, std::uintptr_t widget, UiHeld& held) const {
    if (!kind) return false;
    for (const UiHeld& h : HeldBy(holder, *kind, l_, read_)) {
        if (h.pointer == widget || h.pointer + l_.window_part == widget) {
            held = h;
            return true;
        }
    }
    return false;
}

std::uintptr_t UiStructure::HolderUnder(std::uintptr_t owner, std::uintptr_t widget, UiHeld& held) const {
    const UiKind* kind = KindOf(owner);
    if (Holds(owner, kind, widget, held)) return owner;
    if (kind) {
        for (const UiHeld& h : HeldBy(owner, *kind, l_, read_)) {
            // The kind first: a scan of the registry only for the few boxes and scroll bars.
            std::uintptr_t box = 0;
            const UiKind* box_kind = nullptr;
            if (!Inner(KindOf(h.pointer)) || !LiveWidget(h.pointer, box, box_kind) || !Inner(box_kind)) continue;
            if (Holds(box, box_kind, widget, held)) return box;
            for (const UiHeld& inner : HeldBy(box, *box_kind, l_, read_)) {
                std::uintptr_t bar = 0;
                const UiKind* bar_kind = nullptr;
                const UiKind* inner_kind = KindOf(inner.pointer);
                if (inner_kind && inner_kind->children == UiKind::Children::Scrollbar &&
                    LiveWidget(inner.pointer, bar, bar_kind) && bar_kind &&
                    bar_kind->children == UiKind::Children::Scrollbar && Holds(bar, bar_kind, widget, held)) {
                    return bar;
                }
            }
        }
    }
    held = {};
    return owner;
}

std::string UiStructure::Climb(std::uintptr_t manager, std::uintptr_t widget) {
    entries_.clear();
    index_.clear();
    roots_.clear();
    children_.clear();
    complete_ = true;
    if (std::string error = CopyRegistry(manager); !error.empty()) return error;
    std::uintptr_t at = 0;
    const UiKind* kind = nullptr;
    if (!LiveWidget(widget, at, kind)) return {};
    entries_.push_back({at, kind});
    for (int depth = 0; depth < kMaxDepth; ++depth) {
        std::uintptr_t owner = NodeOwnerAbove(at, kind);
        // A scroll bar has no node: its buttons' nodes say where it is.
        if (owner == 0 && kind && kind->node == 0) {
            for (const UiHeld& part : HeldBy(at, *kind, l_, read_)) {
                std::uintptr_t live = 0;
                const UiKind* part_kind = nullptr;
                if (LiveWidget(part.pointer, live, part_kind) && (owner = NodeOwnerAbove(live, part_kind)) != 0) break;
            }
        }
        if (owner == 0) {
            // A top-level window, unless the widget's own node said nothing.
            std::uintptr_t node = 0, node_owner = 0;
            const bool readable = kind && (kind->node == 0 || (Get(read_, at + kind->node, node) && node != 0 &&
                                                               Get(read_, node + l_.node_owner, node_owner) &&
                                                               (node_owner == at || node_owner == 0)));
            complete_ = readable;
            break;
        }
        UiHeld held;
        const std::uintptr_t parent = HolderUnder(owner, at, held);
        const bool loop = std::any_of(entries_.begin(), entries_.end(), [parent](const UiEntry& e) { return e.pointer == parent; });
        if (loop) break;
        entries_.back().parent = static_cast<int>(entries_.size());
        entries_.back().held_name = held.name;
        at = parent;
        kind = KindOf(parent);
        entries_.push_back({at, kind});
    }
    return {};
}

}  // namespace tongs
