#pragma once

#include "console.hpp"
#include "game_symbols.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tongs {

// The console of EU4 1.37.5 (docs/design.md, "Game calls (0.7.0)"), from
// plugin/symbols.toml. RVAs are relative to eu4.exe.
struct ConsoleLayout {
    unsigned manager_rva = U32(sym::console_manager_get.manager);  // pointer to the console manager; null before its first use
    unsigned dispatch_rva = 0;      // RvaOf(sym::console_dispatch): (manager, result, tokens) -> result, one tokenised line
    unsigned string_free_rva = 0;   // RvaOf(sym::string_free): the game's std::string destructor, for the result text
    unsigned entries = 0x00;        // not in symbols.toml: manager, vector of command entries (begin, end)
    unsigned entry_size = U32(sym::console_lookup.entry_size);
    unsigned entry_name = U32(sym::console_lookup.name);                // const char*
    unsigned entry_alias_count = U32(sym::console_lookup.alias_count);  // int32, up to 3
    unsigned entry_aliases = U32(sym::console_lookup.aliases);          // const char*[3]
    unsigned developer_lock = U32(sym::console_dispatch_gates.developer_lock);  // manager byte: developer commands locked
    unsigned mode_lock = U32(sym::console_mode_gate.mode_lock);  // manager byte: multiplayer/ironman lock, set at registration
    unsigned root_rva = U32(sym::root_and_date.root);
    unsigned session = U32(sym::session_of_root.session);  // root: the session
    unsigned session_vtable_rva = 0;                        // SessionVtableRva(), set when loading
    unsigned ironman = U32(sym::console_registration.ironman);  // root byte: ironman, as the registration reads it
    unsigned multiplayer_rva = 0;  // RvaOf(sym::is_multiplayer): bool (session*), as the registration calls it

    // The members only Anvil knows: call when the plugin is loading.
    static ConsoleLayout Load() {
        ConsoleLayout l;
        l.dispatch_rva = RvaOf(sym::console_dispatch);
        l.string_free_rva = RvaOf(sym::string_free);
        l.session_vtable_rva = SessionVtableRva();
        l.multiplayer_rva = RvaOf(sym::is_multiplayer);
        return l;
    }
};

// Main thread only. Reads the manager, its commands and the game's mode;
// returns an error text, or empty. Every read is guarded.
std::string ReadGameConsole(std::uintptr_t image_base, const ConsoleLayout& layout, ConsoleRaw& out);

// Main thread only, after ReadGameConsole in the same job. Builds the token vector
// in Tongs' memory (the game only reads it), calls the dispatcher with a
// zeroed result, copies the text out and frees it with the game's destructor.
// A structured exception is caught and reported in `out`.
void CallGameConsole(std::uintptr_t image_base, const ConsoleLayout& layout, const std::vector<std::string>& tokens,
                     ConsoleCallResult& out);

}  // namespace tongs
