#pragma once

#include "notifications.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace tongs {

// error.log for the `error_log` notifications (docs/design.md, "Notifications").
//
// The game writes its logs to `logs/` in PhysFS's write directory, which it
// sets at start-up (Documents\Paradox Interactive\Europa Universalis IV, or
// what userdir= / userdir.txt choose). eu4.exe exports PHYSFS_getWriteDir
// (symbols.toml `physfs_get_write_dir`), so Tongs asks the game.
ErrorLogLocation LocateErrorLog();

// Reads up to `max` bytes at `offset`, with the file's size and identity
// (volume serial and file index). The file is opened only for the read, with
// every sharing mode, so the game can still write, move or delete it.
// Returns an error text, or empty; sets chunk.missing when there is no file.
std::string ReadLogChunk(const std::string& path, std::uint64_t offset, std::size_t max, LineTail::Chunk& chunk);

}  // namespace tongs
