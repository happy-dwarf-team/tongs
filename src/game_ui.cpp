#include "game_ui.hpp"

#include <windows.h>

#include <cstring>
#include <unordered_map>

namespace tongs {
namespace {

constexpr std::size_t kMaxWidgets = 200000;  // the registry reserves 64 000
constexpr std::size_t kMaxChildren = 20000;  // per container
constexpr std::size_t kMaxText = 4096;
constexpr int kMaxNodeClimb = 8;
constexpr int kMaxDepth = 256;

// Each read of game memory is guarded on its own, so a widget that changes
// under us costs that widget, not the whole tree.
bool ReadRaw(std::uintptr_t address, void* out, std::size_t size) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

template <typename T>
bool Read(std::uintptr_t address, T& out) {
    return ReadRaw(address, &out, sizeof(T));
}

// MSVC std::string: 16-byte buffer or pointer, size at +0x10, capacity at +0x18.
bool ReadString(std::uintptr_t address, std::string& out) {
    unsigned char raw[0x20];
    if (!ReadRaw(address, raw, sizeof raw)) return false;
    std::uint64_t size = 0, capacity = 0;
    std::memcpy(&size, raw + 0x10, 8);
    std::memcpy(&capacity, raw + 0x18, 8);
    if (size > capacity || size > kMaxText) return false;
    if (capacity < 16) {
        out.assign(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(size));
        return true;
    }
    std::uintptr_t data = 0;
    std::memcpy(&data, raw, 8);
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || ReadRaw(data, out.data(), out.size());
}

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

// The pointers of a std::vector (begin at `at`, end at `at + 8`), one per element of `stride` bytes.
void ReadVector(std::uintptr_t at, std::size_t stride, std::size_t offset, std::vector<std::uintptr_t>& out) {
    std::uintptr_t begin = 0, end = 0;
    if (!Read(at, begin) || !Read(at + 8, end) || end < begin || (end - begin) % stride != 0) return;
    const std::size_t count = (end - begin) / stride;
    if (count > kMaxChildren) return;
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t p = 0;
        if (Read(begin + i * stride + offset, p) && p != 0) out.push_back(p);
    }
}

// An insertion list of nodes {entry*, prev, next}: the pointer at `value` in each entry.
void ReadList(std::uintptr_t head, const UiLayout& l, unsigned value, std::vector<std::uintptr_t>& out) {
    std::uintptr_t node = 0;
    if (!Read(head, node)) return;
    for (std::size_t i = 0; node != 0 && i < kMaxChildren; ++i) {
        std::uintptr_t entry = 0, p = 0;
        if (Read(node, entry) && entry != 0 && Read(entry + value, p) && p != 0) out.push_back(p);
        if (!Read(node + l.list_node_next, node)) return;
    }
}

// What a widget's containers hold, in their order. Not every pointer is a live widget.
std::vector<std::uintptr_t> HeldBy(std::uintptr_t widget, const UiKind& kind, const UiLayout& l) {
    std::vector<std::uintptr_t> out;
    const std::uintptr_t full = widget - l.window_part;
    switch (kind.children) {
        case UiKind::Children::Window:
            for (unsigned i = 0; i < l.window_pair_count; ++i) ReadVector(full + l.window_pairs + i * l.pair_stride, 8, 0, out);
            for (unsigned i = 0; i < l.window_map_count; ++i) ReadList(full + l.window_maps + i * l.map_stride + l.map_list_head, l, 0, out);
            ReadVector(full + l.window_subwindows, l.subwindow_size, l.subwindow_pointer, out);
            break;
        case UiKind::Children::Container:
            for (unsigned i = 0; i < l.container_pair_count; ++i) {
                ReadVector(full + l.container_pairs + i * l.pair_stride, 8, 0, out);
            }
            break;
        case UiKind::Children::Listbox:
        case UiKind::Children::SmoothListbox: {
            const bool smooth = kind.children == UiKind::Children::SmoothListbox;
            ReadList(widget + l.list_items, l, smooth ? l.smooth_item_window : l.listbox_item_window, out);
            std::uintptr_t bar = 0;
            if (Read(widget + (smooth ? l.smooth_scrollbar : l.listbox_scrollbar), bar) && bar != 0) out.push_back(bar);
            break;
        }
        case UiKind::Children::Scrollbar:
            for (const unsigned part : l.scrollbar_parts) {
                std::uintptr_t p = 0;
                if (Read(widget + part, p) && p != 0) out.push_back(p);
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
};

class Reader {
public:
    Reader(const UiLayout& layout, std::uintptr_t image_base) : l_(layout), base_(image_base) {
        for (const UiKind& k : layout.kinds) kinds_.emplace(k.vtable, &k);
    }

    std::string Read(UiTree& out) {
        if (std::string error = ReadRegistry(); !error.empty()) return error;
        out.widgets.resize(entries_.size());
        // Children held by windows, list boxes and scroll bars: the first holder wins.
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (!entries_[i].kind) continue;
            for (const std::uintptr_t p : HeldBy(entries_[i].pointer, *entries_[i].kind, l_)) Adopt(i, Find(p), out);
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
            if (holder != -1) Adopt(holder, i, out);
        }
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            Describe(entries_[i], out.widgets[i]);
            if (entries_[i].parent == -1) out.roots.push_back(i);
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

    // The live widget at a pointer, which may also be the full object of a window.
    int Find(std::uintptr_t p) const {
        auto it = index_.find(p);
        if (it == index_.end()) it = index_.find(p + l_.window_part);
        return it == index_.end() ? -1 : it->second;
    }

    // Makes `child` a child of `parent`, unless it has one or that would close a loop.
    void Adopt(int parent, int child, UiTree& out) {
        if (child < 0 || parent < 0 || child == parent || entries_[child].parent != -1) return;
        int depth = 0;
        for (int up = parent; up != -1; up = entries_[up].parent) {
            if (up == child || ++depth > kMaxDepth) return;
        }
        entries_[child].parent = parent;
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
        std::uint8_t shown = 0;
        w.shown = tongs::Read(e.pointer + l_.shown, shown) && shown != 0;
        std::uintptr_t function = 0, name = 0;
        if (Slot(code_, e.pointer, l_.slot_name, function) && CallName(function, e.pointer, name) && name != 0) {
            ReadString(name, w.name);
        }
        std::int32_t position[2], size[2];
        Pair(code_, e.pointer, l_.slot_position, position);
        Pair(code_, e.pointer, l_.slot_size, size);
        w.rect = {position[0], position[1], size[0], size[1]};
        if (!e.kind) return;
        std::string text;
        // A button without a label has an empty string; a text box shows even an empty one.
        if (e.kind->text != 0 && ReadString(e.pointer + e.kind->text, text) && (w.type == UiType::Text || !text.empty())) {
            w.has_text = true;
            w.text = GameTextToUtf8(text);
        }
        std::int32_t state = 0;
        if (e.kind->state && tongs::Read(e.pointer + l_.button_state, state)) {
            w.has_enabled = true;
            w.enabled = state != 3;
        }
    }

    const UiLayout& l_;
    std::uintptr_t base_;
    Code code_{base_};
    std::unordered_map<unsigned, const UiKind*> kinds_;
    std::vector<Entry> entries_;
    std::unordered_map<std::uintptr_t, int> index_;
};

}  // namespace

std::string ReadUiTree(std::uintptr_t image_base, const UiLayout& layout, UiTree& out) {
    out = {};
    return Reader(layout, image_base).Read(out);
}

}  // namespace tongs
