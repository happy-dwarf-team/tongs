#include "game_ui.hpp"

#include "game_memory.hpp"

#include <windows.h>

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace tongs {
namespace {

constexpr std::size_t kMaxWidgets = 200000;  // the registry reserves 64 000
constexpr std::size_t kMaxChildren = 20000;  // per container
constexpr int kMaxNodeClimb = 8;
constexpr int kMaxDepth = 256;

// The executable sections of eu4.exe: a vtable slot is called only when it points there.
struct Code {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;

    explicit Code(std::uintptr_t base) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
            const std::uintptr_t from = base + section->VirtualAddress;
            const std::uintptr_t to = from + section->Misc.VirtualSize;
            if (begin == 0 || from < begin) begin = from;
            if (to > end) end = to;
        }
    }
    bool Contains(std::uintptr_t address) const { return address >= begin && address < end; }
};

using PairSlot = void* (*)(void* self, std::int32_t* out);
using NameSlot = const void* (*)(void* self);

bool CallPair(std::uintptr_t function, std::uintptr_t self, std::int32_t (&out)[2]) {
    __try {
        reinterpret_cast<PairSlot>(function)(reinterpret_cast<void*>(self), out);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool CallName(std::uintptr_t function, std::uintptr_t self, std::uintptr_t& out) {
    __try {
        out = reinterpret_cast<std::uintptr_t>(reinterpret_cast<NameSlot>(function)(reinterpret_cast<void*>(self)));
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// The function in a widget's vtable slot, when it lies in the game's code.
bool Slot(const Code& code, std::uintptr_t widget, unsigned slot, std::uintptr_t& function) {
    std::uintptr_t vtable = 0;
    return Read(widget, vtable) && Read(vtable + slot, function) && code.Contains(function);
}

bool Pair(const Code& code, std::uintptr_t widget, unsigned slot, std::int32_t (&out)[2]) {
    std::uintptr_t function = 0;
    out[0] = out[1] = 0;
    return Slot(code, widget, slot, function) && CallPair(function, widget, out);
}

// A widget a holder keeps, and where the holder stores its name (a std::string; 0: nowhere).
// The name is read only when the widget's definition is gone.
struct Held {
    std::uintptr_t pointer = 0;
    std::uintptr_t name = 0;
};

// A std::vector at `at` (begin, end): the pointer at `offset` of each element of
// `stride` bytes, and the std::string at `name` of the same element.
void ReadVector(std::uintptr_t at, std::size_t stride, std::size_t offset, std::size_t name, std::vector<Held>& out) {
    std::uintptr_t begin = 0, end = 0;
    if (!Read(at, begin) || !Read(at + 8, end) || end < begin || (end - begin) % stride != 0) return;
    const std::size_t count = (end - begin) / stride;
    if (count > kMaxChildren) return;
    for (std::size_t i = 0; i < count; ++i) {
        Held h;
        if (!Read(begin + i * stride + offset, h.pointer) || h.pointer == 0) continue;
        h.name = begin + i * stride + name;
        out.push_back(h);
    }
}

// A vector pair {pointers, names}: the names vector holds a std::string per pointer, in the same order.
void ReadPair(std::uintptr_t at, const UiLayout& l, std::vector<Held>& out) {
    std::uintptr_t begin = 0, end = 0, names = 0, names_end = 0;
    if (!Read(at, begin) || !Read(at + 8, end) || end < begin || (end - begin) % 8 != 0) return;
    const std::size_t count = (end - begin) / 8;
    if (count > kMaxChildren) return;
    // Names only when both vectors have the same length; otherwise the children come without one.
    const bool named = Read(at + l.pair_names, names) && Read(at + l.pair_names + 8, names_end) && names_end >= names &&
                       names_end - names == count * l.name_size;
    for (std::size_t i = 0; i < count; ++i) {
        Held h;
        if (!Read(begin + i * 8, h.pointer) || h.pointer == 0) continue;
        if (named) h.name = names + i * l.name_size;
        out.push_back(h);
    }
}

// An insertion list of nodes {entry*, prev, next}: the pointer at `value` in each
// entry, and the std::string at `name` when `named`.
void ReadList(std::uintptr_t head, const UiLayout& l, unsigned value, bool named, unsigned name, std::vector<Held>& out) {
    std::uintptr_t node = 0;
    if (!Read(head, node)) return;
    for (std::size_t i = 0; node != 0 && i < kMaxChildren; ++i) {
        std::uintptr_t entry = 0;
        Held h;
        if (Read(node, entry) && entry != 0 && Read(entry + value, h.pointer) && h.pointer != 0) {
            if (named) h.name = entry + name;
            out.push_back(h);
        }
        if (!Read(node + l.list_node_next, node)) return;
    }
}

// A std::vector at `at` of item pointers: the pointer at `value` in each item.
void ReadItems(std::uintptr_t at, unsigned value, std::vector<Held>& out) {
    std::uintptr_t begin = 0, end = 0;
    if (!Read(at, begin) || !Read(at + 8, end) || end < begin || (end - begin) % 8 != 0) return;
    const std::size_t count = (end - begin) / 8;
    if (count > kMaxChildren) return;
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t item = 0;
        Held h;
        if (Read(begin + i * 8, item) && item != 0 && Read(item + value, h.pointer) && h.pointer != 0) out.push_back(h);
    }
}

// What a widget's containers hold, in their order. Not every pointer is a live widget.
std::vector<Held> HeldBy(std::uintptr_t widget, const UiKind& kind, const UiLayout& l) {
    std::vector<Held> out;
    const std::uintptr_t full = widget - l.window_part;
    switch (kind.children) {
        case UiKind::Children::Window:
            for (std::size_t i = 0; i < l.window_pair_count; ++i) ReadPair(widget + l.window_pairs + i * l.pair_stride, l, out);
            for (std::size_t i = 0; i < l.window_map_count; ++i) {
                ReadList(full + l.window_maps + i * l.map_stride + l.map_list_head, l, 0, true, l.map_entry_name, out);
            }
            ReadVector(full + l.window_subwindows, l.subwindow_size, l.subwindow_pointer, l.subwindow_name, out);
            break;
        case UiKind::Children::Container:
            for (std::size_t i = 0; i < l.container_pair_count; ++i) {
                ReadPair(widget + l.container_pairs + i * l.pair_stride, l, out);
            }
            break;
        case UiKind::Children::Listbox:
        case UiKind::Children::SmoothListbox: {
            const bool smooth = kind.children == UiKind::Children::SmoothListbox;
            ReadList(widget + l.list_items, l, smooth ? l.smooth_item_window : l.listbox_item_window, false, 0, out);
            Held bar;
            if (Read(widget + (smooth ? l.smooth_scrollbar : l.listbox_scrollbar), bar.pointer) && bar.pointer != 0) {
                out.push_back(bar);
            }
            break;
        }
        case UiKind::Children::Overlapping:
            ReadList(widget + l.overlapping_items, l, l.overlapping_item_window, false, 0, out);
            break;
        case UiKind::Children::Grid: {
            ReadItems(widget + l.grid_items, l.grid_item_window, out);
            Held bar;
            if (Read(widget + l.grid_scrollbar, bar.pointer) && bar.pointer != 0) out.push_back(bar);
            break;
        }
        case UiKind::Children::Scrollbar:
            for (const unsigned at : l.scrollbar_parts) {
                Held part;
                if (Read(widget + at, part.pointer) && part.pointer != 0) out.push_back(part);
            }
            break;
        case UiKind::Children::None:
            break;
    }
    return out;
}

struct Entry {
    std::uintptr_t pointer = 0;
    const UiKind* kind = nullptr;  // null: a class Tongs does not know
    int parent = -1;
    std::uintptr_t held_name = 0;  // the std::string a window's or container's child list keeps for it
};

class Reader {
public:
    Reader(const UiLayout& layout, std::uintptr_t image_base) : l_(layout), base_(image_base) {
        for (const UiKind& k : layout.kinds) kinds_.emplace(k.vtable, &k);
    }

    std::string Read(UiTree& out) {
        if (std::string error = ReadRegistry(); !error.empty()) return error;
        if (std::string error = ReadDefinitions(); !error.empty()) return error;
        out.widgets.resize(entries_.size());
        // Children held by windows, list, overlapping and grid boxes, and scroll bars: the first holder wins.
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (!entries_[i].kind) continue;
            for (const Held& h : HeldBy(entries_[i].pointer, *entries_[i].kind, l_)) Adopt(i, Find(h.pointer), out, &h);
        }
        // Widgets the game's own code holds: under the owner of their parent render node.
        // A widget without a node (a scroll bar) goes where its first child's node points.
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (entries_[i].parent != -1) continue;
            int holder = NodeParent(i);
            const bool has_node = entries_[i].kind && entries_[i].kind->node != 0;
            for (std::size_t c = 0; holder == -1 && !has_node && c < out.widgets[i].children.size(); ++c) {
                holder = NodeParent(out.widgets[i].children[c]);
            }
            if (holder != -1) Adopt(holder, i, out, nullptr);
        }
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            Describe(entries_[i], out.widgets[i]);
            if (entries_[i].parent == -1) out.roots.push_back(i);
        }
        PropagateStale(out);
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (entries_[i].kind && entries_[i].kind->children == UiKind::Children::Scrollbar) {
                out.widgets[i].rect = ChildrenBounds(out, i);
            }
        }
        return {};
    }

private:
    std::string ReadRegistry() {
        std::uintptr_t manager = 0, begin = 0, end = 0;
        if (!tongs::Read(base_ + l_.manager_rva, manager) || manager == 0) return "no GUI manager";
        if (!tongs::Read(manager + l_.registry_begin, begin) || !tongs::Read(manager + l_.registry_end, end) ||
            end < begin || (end - begin) % 8 != 0 || (end - begin) / 8 > kMaxWidgets) {
            return "the widget registry is unreadable";
        }
        std::vector<std::uintptr_t> registry((end - begin) / 8);
        if (!registry.empty() && !ReadRaw(begin, registry.data(), registry.size() * 8)) {
            return "the widget registry is unreadable";
        }
        entries_.reserve(registry.size());
        for (const std::uintptr_t p : registry) {
            std::uintptr_t vtable = 0;
            std::uint8_t dead = 1;
            if (p == 0 || !tongs::Read(p, vtable) || !tongs::Read(p + l_.dead, dead) || dead != 0 || index_.contains(p)) continue;
            const auto kind = kinds_.find(static_cast<unsigned>(vtable - base_));
            index_.emplace(p, static_cast<int>(entries_.size()));
            entries_.push_back({p, kind == kinds_.end() ? nullptr : kind->second});
        }
        return {};
    }

    // Every live definition, read once per tree: a widget whose definition is
    // not among them was built from one a reload freed.
    std::string ReadDefinitions() {
        std::uintptr_t manager = 0;
        if (!tongs::Read(base_ + l_.manager_rva, manager) || manager == 0) return "no GUI manager";
        TypeTreeLayout layout = l_.type_tree;
        layout.map_vtable += base_;
        for (TypeTreeLayout::Nested& n : layout.nested) n.vtable += base_;
        for (TypeTreeLayout::Pointer& p : layout.pointers) {
            p.vtable += base_;
            p.target += base_;
        }
        const ReadMemory read = [](std::uintptr_t address, void* out, std::size_t size) { return ReadRaw(address, out, size); };
        return CollectLiveDefinitions(manager + l_.type_registry, layout, read, definitions_);
    }

    // The live widget at a pointer, which may also be the full object of a window.
    int Find(std::uintptr_t p) const {
        auto it = index_.find(p);
        if (it == index_.end()) it = index_.find(p + l_.window_part);
        return it == index_.end() ? -1 : it->second;
    }

    // Makes `child` a child of `parent`, unless it has one or that would close a loop.
    // `held`: what the parent's container says about it; null for a render-node holder.
    void Adopt(int parent, int child, UiTree& out, const Held* held) {
        if (child < 0 || parent < 0 || child == parent || entries_[child].parent != -1) return;
        int depth = 0;
        for (int up = parent; up != -1; up = entries_[up].parent) {
            if (up == child || ++depth > kMaxDepth) return;
        }
        Entry& e = entries_[child];
        e.parent = parent;
        if (held) e.held_name = held->name;
        out.widgets[parent].children.push_back(child);
    }

    int NodeParent(int i) const {
        const Entry& e = entries_[i];
        std::uintptr_t node = 0, owner = 0;
        // A list box's node may have no owner set.
        if (!e.kind || e.kind->node == 0 || !tongs::Read(e.pointer + e.kind->node, node) || node == 0 ||
            !tongs::Read(node + l_.node_owner, owner) || (owner != e.pointer && owner != 0)) {
            return -1;
        }
        for (int step = 0; step < kMaxNodeClimb; ++step) {
            if (!tongs::Read(node + l_.node_parent, node) || node == 0 || !tongs::Read(node + l_.node_owner, owner)) return -1;
            const int found = owner == 0 ? -1 : Find(owner);
            if (found != -1 && found != i) return found;
        }
        return -1;
    }

    void Describe(const Entry& e, UiWidget& w) const {
        w.type = e.kind ? e.kind->type : UiType::Other;
        w.address = e.pointer;
        std::uint8_t shown = 0;
        w.shown = tongs::Read(e.pointer + l_.shown, shown) && shown != 0;
        Name(e, w);
        std::int32_t position[2], size[2];
        Pair(code_, e.pointer, l_.slot_position, position);
        Pair(code_, e.pointer, l_.slot_size, size);
        w.rect = {position[0], position[1], size[0], size[1]};
        if (!e.kind) return;
        std::string text;
        // A button without a label has an empty string; a text or edit box shows even an empty one.
        if (e.kind->text != 0 && ReadString(e.pointer + e.kind->text, text) &&
            (w.type == UiType::Text || w.type == UiType::Editbox || !text.empty())) {
            w.has_text = true;
            w.text = GameTextToUtf8(text);
        }
        std::int32_t state = 0;
        if (e.kind->state && tongs::Read(e.pointer + l_.button_state, state)) {
            w.has_enabled = true;
            w.enabled = state != 3;
        }
        std::uint8_t checked = 0;
        if (e.kind->checked != 0 && tongs::Read(e.pointer + e.kind->checked, checked)) {
            w.has_checked = true;
            w.checked = checked != 0;
        }
    }

    // The name, without calling through a freed definition (ui_definitions.hpp):
    // slot 0xA0 when the definition is live; otherwise the widget is stale and
    // named by its holder's child list or its own name, when it has either.
    void Name(const Entry& e, UiWidget& w) const {
        const auto children = e.kind ? e.kind->children : UiKind::Children::None;
        std::uintptr_t definition = 0;
        const bool live = tongs::Read(e.pointer + l_.definition, definition) && definitions_.contains(definition);
        std::string own;
        const bool has_override =
            !live && (children == UiKind::Children::Window || children == UiKind::Children::Container) &&
            ReadString(e.pointer + (children == UiKind::Children::Window ? l_.window_override : l_.container_override), own) &&
            !own.empty();
        std::string held;
        const bool has_held = !live && e.held_name != 0 && ReadString(e.held_name, held);
        const UiNamePlan plan = PlanWidgetName(live, has_held, has_override);
        w.stale = plan.stale;
        switch (plan.source) {
            case UiNamePlan::Source::Holder:
                w.name = std::move(held);
                break;
            case UiNamePlan::Source::Override:
                w.name = std::move(own);
                break;
            case UiNamePlan::Source::Definition: {
                std::uintptr_t function = 0, name = 0;
                if (Slot(code_, e.pointer, l_.slot_name, function) && CallName(function, e.pointer, name) && name != 0) {
                    ReadString(name, w.name);
                }
                break;
            }
            case UiNamePlan::Source::None:
                break;
        }
    }

    const UiLayout& l_;
    std::uintptr_t base_;
    Code code_{base_};
    std::unordered_map<unsigned, const UiKind*> kinds_;
    std::vector<Entry> entries_;
    std::unordered_map<std::uintptr_t, int> index_;
    std::unordered_set<std::uintptr_t> definitions_;
};

}  // namespace

std::string ReadUiTree(std::uintptr_t image_base, const UiLayout& layout, UiTree& out) {
    out = {};
    return Reader(layout, image_base).Read(out);
}

std::uintptr_t ReadHoveredWidget(std::uintptr_t image_base, const UiLayout& layout) {
    std::uintptr_t manager = 0, hovered = 0;
    if (!Read(image_base + layout.manager_rva, manager) || manager == 0 || !Read(manager + layout.hovered, hovered)) return 0;
    return hovered;
}

}  // namespace tongs
