#include "game_ui.hpp"

#include "game_memory.hpp"

#include <windows.h>

#include <unordered_set>

namespace tongs {
namespace {

constexpr int kMaxBoundsDepth = 4;  // levels of children read for a window that takes its rect from them

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

const ReadMemory& GameMemory() {
    static const ReadMemory read = [](std::uintptr_t address, void* out, std::size_t size) {
        return ReadRaw(address, out, size);
    };
    return read;
}

// The type registry's layout with its vtables as addresses in the game.
TypeTreeLayout LiveLayout(const UiLayout& l, std::uintptr_t base) {
    TypeTreeLayout layout = l.type_tree;
    layout.map_vtable += base;
    for (TypeTreeLayout::Nested& n : layout.nested) n.vtable += base;
    for (TypeTreeLayout::Pointer& p : layout.pointers) {
        p.vtable += base;
        p.target += base;
    }
    return layout;
}

// Everything of a widget but its name: kind, shown, rect, text and state.
class Describer {
public:
    Describer(const UiLayout& layout, std::uintptr_t image_base) : l_(layout), base_(image_base) {}

    void Describe(const UiEntry& e, UiWidget& w) const {
        w.type = e.kind ? e.kind->type : UiType::Other;
        w.address = e.pointer;
        std::uint8_t shown = 0;
        w.shown = tongs::Read(e.pointer + l_.shown, shown) && shown != 0;
        std::int32_t position[2], size[2];
        Pair(code_, e.pointer, l_.slot_position, position);
        Pair(code_, e.pointer, l_.slot_size, size);
        w.rect = {position[0], position[1], size[0], size[1]};
        if (!e.kind) return;
        DrawnSize(e, w);
        std::string text;
        // A button without a label has an empty string; a text or edit box shows even an empty one.
        if (e.kind->text != 0 && ReadString(e.pointer + e.kind->text, text) &&
            (w.type == UiType::Text || w.type == UiType::Editbox || !text.empty())) {
            w.has_text = true;
            w.text = GameTextToUtf8(text);
            w.markup = GameTextToUtf8(text, true);
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

    // The name and the definition's file, without calling through a freed definition (ui_definitions.hpp):
    // slot 0xA0 when `live`; otherwise the widget is stale and named by its
    // holder's child list or its own name, when it has either. With `direct`,
    // what slot 0xA0 would return is read instead of called: a window's or
    // container's own name when set, else the definition's (definition_name);
    // a slot Tongs does not know gives no name then.
    void Name(const UiEntry& e, bool live, bool direct, UiWidget& w) const {
        const auto children = e.kind ? e.kind->children : UiKind::Children::None;
        const bool owns_name = children == UiKind::Children::Window || children == UiKind::Children::Container;
        std::string own;
        const bool has_override =
            owns_name &&
            ReadString(e.pointer + (children == UiKind::Children::Window ? l_.window_override : l_.container_override),
                       own) &&
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
                std::uintptr_t function = 0, name = 0, definition = 0;
                if (!Slot(code_, e.pointer, l_.slot_name, function)) break;
                if (!direct) {
                    if (CallName(function, e.pointer, name) && name != 0) ReadString(name, w.name);
                    break;
                }
                const std::uint32_t rva = static_cast<std::uint32_t>(function - base_);
                const bool own_slot = rva == l_.window_name_slot_rva || rva == l_.container_name_slot_rva;
                if (own_slot && has_override) {
                    w.name = std::move(own);
                } else if ((own_slot || rva == l_.widget_name_slot_rva) && tongs::Read(e.pointer + l_.definition, definition)) {
                    ReadString(definition + l_.definition_name, w.name);
                }
                break;
            }
            case UiNamePlan::Source::None:
                break;
        }
        // The file only from a live definition: a freed one may hold anything.
        std::uintptr_t definition = 0;
        std::string file;
        if (live && tongs::Read(e.pointer + l_.definition, definition) && ReadString(definition + l_.definition_file, file)) {
            w.has_definition_file = true;
            w.definition_file = GameTextToUtf8(file);
            std::int32_t line = 0;
            if (tongs::Read(definition + l_.definition_line, line) && line > 0) {
                w.definition_line = static_cast<std::uint32_t>(line);
            }
        }
    }

private:
    // Where slot 0xE0 is not what the game draws: a list box shows only the
    // rows its layout placed, a masked shield is the size of its own sprite.
    void DrawnSize(const UiEntry& e, UiWidget& w) const {
        UiRect& r = w.rect;
        if (e.kind->children == UiKind::Children::Listbox) {
            std::int32_t extent[2] = {};
            std::uint8_t vertical = 0;
            if (!ReadRaw(e.pointer + l_.listbox_extent, extent, sizeof extent) ||
                !tongs::Read(e.pointer + l_.listbox_vertical, vertical)) {
                return;
            }
            int& length = vertical != 0 ? r.h : r.w;
            const std::int32_t shown = extent[vertical != 0 ? 1 : 0];
            if (shown > 0 && shown < length) length = shown;
            return;
        }
        if ((w.type != UiType::Button && w.type != UiType::Icon) || e.kind->node == 0 || l_.sprite_node_size_rva == 0) return;
        std::uintptr_t node = 0, vtable = 0, function = 0, sprite = 0;
        float scale = 0;
        std::int32_t size[2] = {};
        if (!tongs::Read(e.pointer + e.kind->node, node) || node == 0 || !tongs::Read(node, vtable) ||
            !tongs::Read(vtable + l_.node_size_slot, function) || function != base_ + l_.sprite_node_size_rva ||
            !tongs::Read(node + l_.sprite_node_sprite, sprite) || sprite == 0 || !tongs::Read(sprite, vtable) ||
            vtable != base_ + l_.masked_shield_vtable || !ReadRaw(sprite + l_.masked_shield_size, size, sizeof size) ||
            !tongs::Read(node + l_.sprite_node_scale, scale) || !(scale > 0 && scale < 100)) {
            return;
        }
        r.w = static_cast<int>(static_cast<float>(size[0]) * scale);
        r.h = static_cast<int>(static_cast<float>(size[1]) * scale);
    }

    const UiLayout& l_;
    std::uintptr_t base_;
    Code code_{base_};
};

// Scroll bars have no render node: their rect is the bounds of their buttons.
void ScrollbarRects(const std::vector<UiEntry>& entries, UiTree& out) {
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (entries[i].kind && entries[i].kind->children == UiKind::Children::Scrollbar) {
            out.widgets[i].rect = ChildrenBounds(out, i);
        }
    }
}

class Reader {
public:
    Reader(const UiLayout& layout, std::uintptr_t image_base, UiRect client)
        : l_(layout), base_(image_base), client_(client), structure_(layout, image_base, GameMemory()),
          describer_(layout, image_base) {}

    std::string Read(UiTree& out) {
        std::uintptr_t manager = 0;
        if (!tongs::Read(base_ + l_.manager_rva, manager) || manager == 0) return "no GUI manager";
        if (std::string error = structure_.ReadAll(manager); !error.empty()) return error;
        // Every live definition, read once per tree: a widget whose definition is
        // not among them was built from one a reload freed.
        if (std::string error = CollectLiveDefinitions(manager + l_.type_registry, LiveLayout(l_, base_), GameMemory(),
                                                       definitions_);
            !error.empty()) {
            return error;
        }
        const std::vector<UiEntry>& entries = structure_.entries();
        out.widgets.resize(entries.size());
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            UiWidget& w = out.widgets[i];
            w.children = structure_.children()[i];
            describer_.Describe(entries[i], w);
            std::uintptr_t definition = 0;
            const bool live = tongs::Read(entries[i].pointer + l_.definition, definition) && definitions_.contains(definition);
            describer_.Name(entries[i], live, false, w);
        }
        out.roots = structure_.roots();
        PropagateStale(out);
        ScrollbarRects(entries, out);
        FitRects(out, client_);
        return {};
    }

private:
    const UiLayout& l_;
    std::uintptr_t base_;
    UiRect client_;
    UiStructure structure_;
    Describer describer_;
    std::unordered_set<std::uintptr_t> definitions_;
};

// FitRects gives a window without a size of its own, or one outside the
// client area, the bounds of its children (and a scroll bar always has them).
bool TakesChildrenBounds(const UiEntry& e, const UiWidget& w, UiRect client) {
    if (e.kind && e.kind->children == UiKind::Children::Scrollbar) return true;
    if (w.type != UiType::Window) return false;
    const UiRect& r = w.rect;
    const bool known = client.w > 0 && client.h > 0;
    const bool inside = r.x >= 0 && r.y >= 0 && r.x + r.w <= client.w && r.y + r.h <= client.h;
    return r.w <= 0 || r.h <= 0 || (known && !inside);
}

class ChainReader {
public:
    ChainReader(const UiLayout& layout, std::uintptr_t image_base, UiRect client, LiveDefinitionCache& definitions)
        : l_(layout), base_(image_base), client_(client), definitions_(definitions),
          structure_(layout, image_base, GameMemory()), describer_(layout, image_base) {}

    std::string Read(UiTree& out, std::uintptr_t& hovered, bool& complete) {
        out = {};
        hovered = 0;
        complete = true;
        std::uintptr_t manager = 0, pointer = 0;
        if (!tongs::Read(base_ + l_.manager_rva, manager) || manager == 0) return "no GUI manager";
        if (!tongs::Read(manager + l_.hovered, pointer) || pointer == 0) return {};
        if (std::string error = structure_.Climb(manager, pointer); !error.empty()) return error;
        const std::vector<UiEntry>& chain = structure_.entries();
        if (chain.empty()) return {};
        complete = structure_.complete();
        if (!complete) return {};
        registry_ = manager + l_.type_registry;
        layout_ = LiveLayout(l_, base_);
        definitions_.Begin();
        // The chain is widgets 0..n-1, each the child of the next; the last is the root.
        out.widgets.resize(chain.size());
        entries_ = chain;
        for (int i = 0; i < static_cast<int>(chain.size()); ++i) {
            if (i + 1 < static_cast<int>(chain.size())) out.widgets[i + 1].children.push_back(i);
            if (std::string error = Describe(i, true, out); !error.empty()) return error;
        }
        out.roots.push_back(static_cast<int>(chain.size()) - 1);
        // Children only where a rect is made of them; they are not visited.
        for (int i = 0, n = static_cast<int>(chain.size()); i < n; ++i) AddBoundsChildren(i, 0, out);
        PropagateStale(out);
        ScrollbarRects(entries_, out);
        FitRects(out, client_);
        hovered = chain[0].pointer;
        return {};
    }

private:
    std::string Describe(int i, bool named, UiTree& out) {
        UiWidget& w = out.widgets[i];
        describer_.Describe(entries_[i], w);
        if (!named) return {};
        std::uintptr_t definition = 0;
        bool live = false;
        if (tongs::Read(entries_[i].pointer + l_.definition, definition)) {
            if (std::string error = definitions_.IsLive(registry_, layout_, GameMemory(), definition, live); !error.empty()) {
                return error;
            }
        }
        describer_.Name(entries_[i], live, true, w);
        return {};
    }

    void AddBoundsChildren(int i, int depth, UiTree& out) {
        if (depth >= kMaxBoundsDepth || !entries_[i].kind || !TakesChildrenBounds(entries_[i], out.widgets[i], client_)) {
            return;
        }
        for (const UiHeld& h : HeldBy(entries_[i].pointer, *entries_[i].kind, structure_.layout(), GameMemory())) {
            UiEntry child;
            if (!structure_.LiveWidget(h.pointer, child.pointer, child.kind)) continue;
            bool known = false;
            for (const int c : out.widgets[i].children) known = known || entries_[c].pointer == child.pointer;
            if (known) continue;
            child.parent = i;
            const int index = static_cast<int>(entries_.size());
            entries_.push_back(child);
            out.widgets.emplace_back();
            out.widgets[i].children.push_back(index);
            Describe(index, false, out);
            AddBoundsChildren(index, depth + 1, out);
        }
    }

    const UiLayout& l_;
    std::uintptr_t base_;
    UiRect client_;
    LiveDefinitionCache& definitions_;
    UiStructure structure_;
    Describer describer_;
    std::vector<UiEntry> entries_;
    std::uintptr_t registry_ = 0;
    TypeTreeLayout layout_;
};

}  // namespace

std::string ReadUiTree(std::uintptr_t image_base, const UiLayout& layout, UiRect client, UiTree& out) {
    out = {};
    return Reader(layout, image_base, client).Read(out);
}

std::uintptr_t ReadHoveredWidget(std::uintptr_t image_base, const UiLayout& layout) {
    std::uintptr_t manager = 0, hovered = 0;
    if (!Read(image_base + layout.manager_rva, manager) || manager == 0 || !Read(manager + layout.hovered, hovered)) return 0;
    return hovered;
}

std::string ReadHoveredChain(std::uintptr_t image_base, const UiLayout& layout, UiRect client,
                             LiveDefinitionCache& definitions, UiTree& out, std::uintptr_t& hovered, bool& complete) {
    return ChainReader(layout, image_base, client, definitions).Read(out, hovered, complete);
}

}  // namespace tongs
