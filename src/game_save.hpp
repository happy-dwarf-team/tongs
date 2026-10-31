#pragma once

#include "game_symbols.hpp"
#include "save_meta.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace tongs {

// Loading a save by name (docs/design.md, "Loading a save (0.22.0)"), from
// plugin/symbols.toml: the in-game Load dialog's load function and the request
// its accept builds (0x1176020), the session's loading flag, the running
// version and the tokens of the game's metadata reader.
struct SaveLayout {
    std::uint32_t root_rva = U32(sym::root_and_date.root);
    std::uint32_t session = U32(sym::session_of_root.session);  // root: the session
    std::uint32_t loading = U32(sym::session_loading.loading);  // session: byte, set while a save loads
    // The request, from the accept's stack frame: the name (std::string) at +0,
    // the subfolders (std::vector<std::string>) at +0x20, then these.
    std::uint32_t request_session = U32(sym::load_request_session.session - sym::load_request_call.request);
    std::uint32_t request_cloud = U32(sym::load_request_session.cloud - sym::load_request_call.request);
    std::uint32_t request_helper = U32(sym::load_request_helper.helper - sym::load_request_call.request);
    std::uint32_t app_rva = U32(sym::running_version_app.app);  // pointer to the application object
    std::array<std::uint32_t, 4> version = {U32(sym::running_version.major), U32(sym::running_version.minor),
                                            U32(sym::running_version.patch), U32(sym::running_version.build)};
    SaveTokens tokens = {static_cast<std::uint16_t>(sym::meta_date_token.token),
                         static_cast<std::uint16_t>(sym::meta_player_token.token),
                         static_cast<std::uint16_t>(sym::meta_ironman_tokens.token),
                         static_cast<std::uint16_t>(sym::meta_ironman_tokens.token_alt),
                         0x314B};  // not in symbols.toml: savegame_versions, read from binary saves (no code compares it)
    std::uint32_t load_rva = 0;         // RvaOf(sym::load_save): int (request)
    std::uint32_t helper_rva = 0;       // RvaOf(sym::load_helper): void* ()
    std::uint32_t close_rva = 0;        // RvaOf(sym::close_ingame_windows): void (session)
    std::uint32_t assign_rva = 0;       // RvaOf(sym::string_assign)
    std::uint32_t string_free_rva = 0;  // RvaOf(sym::string_free)

    static_assert(sym::load_request_session.session - sym::load_request_call.request == sym::load_save.session);
    static_assert(sym::load_request_helper.helper - sym::load_request_call.request == 0x48);  // the request's last field

    // The members only Anvil knows: call when the plugin is loading.
    static SaveLayout Load() {
        SaveLayout l;
        l.load_rva = RvaOf(sym::load_save);
        l.helper_rva = RvaOf(sym::load_helper);
        l.close_rva = RvaOf(sym::close_ingame_windows);
        l.assign_rva = RvaOf(sym::string_assign);
        l.string_free_rva = RvaOf(sym::string_free);
        return l;
    }
};

// Any thread. The file the game would load for NAME: `save games\NAME.eu4` in
// PhysFS's write directory. Returns an error text when the directory is not set.
std::string LocateSave(const std::string& name, std::string& path, bool& exists);

// Any thread. A zip is read whole (its directory is at the end), a text or
// binary save only its first megabyte, where the metadata is.
std::string ReadSaveHead(const std::string& path, std::string& bytes);

// Main thread only, in game. Builds the request as the dialog's accept does
// (the name with the game's own std::string functions, no subfolders, not the
// cloud, the session, the helper) and calls the loader; the call returns after
// the whole load. A structured exception is caught and reported.
LoadCall CallLoad(std::uintptr_t image_base, const SaveLayout& layout, const std::string& name);

// Main thread only: close the in-game windows, as the dialog does after a load.
bool CloseWindows(std::uintptr_t image_base, const SaveLayout& layout, unsigned long& fault_code);

// Main thread only: whether the session's loading flag is set; false when unreadable.
bool ReadLoading(std::uintptr_t image_base, const SaveLayout& layout, bool& loading);

// Any thread: the running version the Load dialog compares saves with, as a.b.c.d
// of savegame_versions.
bool ReadRunningVersion(std::uintptr_t image_base, const SaveLayout& layout, std::array<int, 4>& out);

}  // namespace tongs
