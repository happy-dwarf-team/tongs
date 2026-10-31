#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// `save load NAME` (docs/design.md, "Loading a save (0.22.0)"): the name a
// client gives and the metadata Tongs reads from the file before it asks the
// game to load it. Nothing here touches game memory or files, so the tests
// feed bytes by hand.

// NAME: a file at the top level of `save games`, quoted when it has spaces;
// a trailing `.eu4` (any case) is stripped, because the game appends it.
// Returns an error text for `bad_name`, or empty. `rest` is what follows NAME.
std::string ParseSaveName(std::string_view argument, std::string& name, std::string_view& rest);

// The token ids of the game's binary metadata (EU4bin) that Tongs reads; the
// layout takes them from the game's own metadata reader in symbols.toml.
struct SaveTokens {
    std::uint16_t date = 0;
    std::uint16_t player = 0;
    std::uint16_t ironman = 0;
    std::uint16_t ironman_alt = 0;
    std::uint16_t versions = 0;  // savegame_versions
};

// What the metadata says. A text save keeps it at the top of the file, a
// compressed save in the zip member `meta`, as text or binary.
struct SaveMeta {
    std::string format;              // "text", "binary", "compressed_text", "compressed_binary"
    std::int32_t date_raw = 0;       // as the game stores dates (game_date.hpp)
    std::string player;              // tag
    bool ironman = false;
    std::vector<std::string> versions;  // savegame_versions, e.g. "1.37.5.0"
};

// Reads the metadata from the first bytes of a save (all of `meta` for a zip:
// the caller passes the whole file then). Returns an error text, or empty.
std::string ReadSaveMeta(std::string_view file, const SaveTokens& tokens, SaveMeta& out);

// What a call of the game's loader returned.
struct LoadCall {
    int result = -1;  // the loader's return code: 0 loaded, 1 empty name, 2 no such file, 3 metadata rejected
    bool faulted = false;
    unsigned long fault_code = 0;
};

// Raw DEFLATE (RFC 1951), as a zip member's method 8. Returns false on bad data
// or when the output would pass `max`.
bool Inflate(std::string_view in, std::string& out, std::size_t max);

// Whether `versions` lists the running version as "a.b.c.d", as the Load dialog's check does.
bool VersionListed(const std::vector<std::string>& versions, const std::array<int, 4>& running);

}  // namespace tongs
