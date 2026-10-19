#include "game_console.hpp"

#include "game_memory.hpp"

#include <windows.h>

#include <cstring>

namespace tongs {
namespace {

constexpr std::size_t kMaxEntries = 4096;
constexpr std::size_t kMaxName = 128;
constexpr int kMaxAliases = 3;
constexpr std::uint64_t kMaxText = 1 << 20;  // a result text longer than this is taken as garbage

// The game's MSVC std::string and std::vector<std::string>, built by hand so
// that the plugin's own STL settings do not matter: a 16-byte buffer or a
// pointer, the size, the capacity (15 = inline); three pointers.
struct RawString {
    union {
        char buffer[16];
        const char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;
};
static_assert(sizeof(RawString) == 0x20);

struct RawVector {
    RawString* begin;
    RawString* end;
    RawString* capacity_end;
};
static_assert(sizeof(RawVector) == 0x18);

// The dispatcher's result: a success byte and the text at +8.
struct RawResult {
    std::uint8_t success;
    std::uint8_t padding[7];
    RawString text;
};
static_assert(sizeof(RawResult) == 0x28);

using DispatchFn = RawResult* (*)(void* manager, RawResult* result, RawVector* tokens);
using StringFreeFn = void (*)(RawString* text);
using MultiplayerFn = bool (*)(void* session);

bool ReadCString(std::uintptr_t address, std::string& out) {
    out.clear();
    if (address == 0) return false;
    for (std::size_t i = 0; i < kMaxName; ++i) {
        char c = 0;
        if (!Read(address + i, c)) return false;
        if (c == 0) return true;
        out += c;
    }
    return false;
}

// No C++ objects with destructors here: the bodies run under __try.
bool IsMultiplayer(MultiplayerFn fn, std::uintptr_t session, bool& out) {
    __try {
        out = fn(reinterpret_cast<void*>(session));
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Any structured exception that leaves the game's code ends here: nothing may
// unwind into the frame function. The game's own handlers run first.
bool Dispatch(DispatchFn fn, std::uintptr_t manager, RawResult* result, RawVector* tokens, std::uint32_t* code) {
    __try {
        fn(reinterpret_cast<void*>(manager), result, tokens);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FreeText(StringFreeFn fn, RawString* text, std::uint32_t* code) {
    __try {
        fn(text);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

std::string ReadGameConsole(std::uintptr_t base, const ConsoleLayout& layout, ConsoleRaw& out) {
    out = {};
    std::uintptr_t root = 0;
    if (!Read(base + layout.root_rva, root) || root == 0) return "no game loaded (root object is null)";
    std::uintptr_t session = 0, vtable = 0;
    if (!Read(root + layout.session, session) || session == 0) return "no game loaded (session is null)";
    if (!Read(session, vtable) || vtable != base + layout.session_vtable_rva) return "session object has an unexpected vtable";
    out.session = session;
    std::uint8_t ironman = 0;
    if (!Read(root + layout.ironman, ironman)) return "cannot read the ironman flag";
    out.ironman = ironman != 0;
    if (!IsMultiplayer(reinterpret_cast<MultiplayerFn>(base + layout.multiplayer_rva), session, out.multiplayer)) {
        return "the multiplayer check faulted";
    }

    std::uintptr_t manager = 0;
    if (!Read(base + layout.manager_rva, manager)) return "cannot read the console manager pointer";
    if (manager == 0) return {};  // never created: refused by the caller
    out.manager = true;
    std::uintptr_t begin = 0, end = 0;
    if (!Read(manager + layout.entries, begin) || !Read(manager + layout.entries + 8, end) ||
        !Read(manager + layout.developer_lock, out.developer_lock) || !Read(manager + layout.mode_lock, out.mode_lock)) {
        return "cannot read the console manager";
    }
    if (end < begin || (end - begin) % layout.entry_size != 0 || (end - begin) / layout.entry_size > kMaxEntries) {
        return "the console manager's command vector is not readable";
    }
    for (std::uintptr_t entry = begin; entry < end; entry += layout.entry_size) {
        ConsoleEntry e;
        std::uintptr_t name = 0;
        std::int32_t aliases = 0;
        if (!Read(entry + layout.entry_name, name) || !ReadCString(name, e.name) ||
            !Read(entry + layout.entry_alias_count, aliases) || aliases < 0 || aliases > kMaxAliases) {
            return "a console command entry is not readable";
        }
        for (int i = 0; i < aliases; ++i) {
            std::uintptr_t alias = 0;
            std::string text;
            if (!Read(entry + layout.entry_aliases + 8 * i, alias) || !ReadCString(alias, text)) {
                return "a console command alias is not readable";
            }
            e.aliases.push_back(std::move(text));
        }
        out.entries.push_back(std::move(e));
    }
    return {};
}

void CallGameConsole(std::uintptr_t base, const ConsoleLayout& layout, const std::vector<std::string>& tokens,
                     ConsoleCallResult& out) {
    out = {};
    std::uintptr_t manager = 0;
    if (tokens.empty() || !Read(base + layout.manager_rva, manager) || manager == 0) return;  // checked by the caller
    // Inline copies of short tokens, pointers into `tokens` for long ones. The
    // dispatcher only reads them and copies the arguments it passes on.
    std::vector<RawString> strings(tokens.size());
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        RawString& s = strings[i];
        std::memset(&s, 0, sizeof s);
        s.size = tokens[i].size();
        if (s.size < 16) {
            std::memcpy(s.buffer, tokens[i].c_str(), tokens[i].size() + 1);
            s.capacity = 15;
        } else {
            s.pointer = tokens[i].c_str();
            s.capacity = s.size;
        }
    }
    RawVector vector{strings.data(), strings.data() + strings.size(), strings.data() + strings.size()};
    RawResult result;
    std::memset(&result, 0, sizeof result);
    if (!Dispatch(reinterpret_cast<DispatchFn>(base + layout.dispatch_rva), manager, &result, &vector, &out.fault_code)) {
        out.faulted = true;
        return;  // the result may be half built; it is not freed
    }
    out.success = result.success != 0;
    const RawString& text = result.text;
    if (text.size <= text.capacity && text.size <= kMaxText) {
        const char* data = text.capacity >= 16 ? text.pointer : text.buffer;
        out.text.resize(static_cast<std::size_t>(text.size));
        if (text.size > 0 && !ReadRaw(reinterpret_cast<std::uintptr_t>(data), out.text.data(), out.text.size())) out.text.clear();
    }
    if (!FreeText(reinterpret_cast<StringFreeFn>(base + layout.string_free_rva), &result.text, &out.fault_code)) {
        out.faulted = true;
    }
}

}  // namespace tongs
