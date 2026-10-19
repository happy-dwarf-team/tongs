#pragma once

#include "console.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tongs {

// The console of EU4 1.37.5 (docs/design.md, "Game calls (0.7.0)"). RVAs are
// relative to eu4.exe; plugin.toml checks the code that uses them.
struct ConsoleLayout {
    unsigned manager_rva = 0x2357B58;   // pointer to the console manager; null before its first use
    unsigned dispatch_rva = 0x1721320;  // (manager, result, tokens) -> result: runs one tokenised line
    unsigned string_free_rva = 0x95660; // the game's std::string destructor, for the result text
    unsigned entries = 0x00;            // manager: vector of command entries (begin, end)
    unsigned entry_size = 0xA8;
    unsigned entry_name = 0x08;         // const char*
    unsigned entry_alias_count = 0x10;  // int32, up to 3
    unsigned entry_aliases = 0x18;      // const char*[3]
    unsigned developer_lock = 0x18;     // manager byte: developer commands locked
    unsigned mode_lock = 0x19;          // manager byte: multiplayer/ironman lock, set at registration
    unsigned root_rva = 0x233FE78;
    unsigned session = 0x1E00;          // root: the session
    unsigned session_vtable_rva = 0x1CB8460;
    unsigned ironman = 0x24D0;          // root byte: ironman, as the registration 0x6E90F0 reads it
    unsigned multiplayer_rva = 0x6EAA10;  // bool (session*): multiplayer, as the registration calls it
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
