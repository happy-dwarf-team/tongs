#pragma once

#include "protocol.hpp"

#include <filesystem>
#include <string_view>

namespace tongs {

// Captures the client area of the game window into a PNG file in `folder`
// (created when missing), named <name>.png or shot-<time>.png, and keeps at
// most kMaxScreenshots files there by deleting the oldest. When the Present
// hook is installed (only under Wine, where GDI sees no frame) it copies the
// game's back buffer at its next Present first (game_present.hpp). Otherwise,
// and when that fails, PrintWindow with PW_RENDERFULLCONTENT (works while the
// window is covered), then a copy from the screen. A non-empty `present_off`
// says why the hook does not run now (Anvil suspended Tongs): the frame at
// Present is then skipped at once, with that reason in `present_error`.
// A `region` that is not whole is cut from the captured client area at native
// resolution (never scaled); one that does not lie inside it (RegionError) is
// refused and nothing is written. Any thread but the main thread may call it.
ScreenshotResult TakeScreenshot(const std::filesystem::path& folder, std::string_view name,
                                const ScreenshotRegion& region = {}, std::string_view present_off = {});

inline constexpr int kMaxScreenshots = 100;

}  // namespace tongs
