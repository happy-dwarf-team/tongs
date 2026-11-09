// Map modes in the game (map_mode): SetCurrentMapMode, and anvil-map's service.
#pragma once

#include <cstdint>
#include <string_view>

namespace tongs {

// anvil-map.modes version 1 as far as Tongs uses it (include/anvil/anvil_map.h of Anvil;
// Tongs builds against an SDK that may predate it). Optional: anvil-map may be absent.
struct MapModesService {
    std::uint32_t size;
    void* register_mode;
    void* set_colors;
    const char* (*mode_id)(std::int32_t number);
    const void* (*shown)();
    const char* (*id)(const void* mode);
};
constexpr const char* kMapModesService = "anvil-map.modes";

// The number of the plugin mode with `id`, or -1. Any thread.
std::int32_t PluginMapModeNumber(const MapModesService* service, std::string_view id);

// SetCurrentMapMode(session, number, false, false) on the main thread; false with the
// exception code when it faulted.
bool CallSetMapMode(std::uintptr_t base, std::uintptr_t session, std::int32_t number, unsigned long& fault_code);

}  // namespace tongs
