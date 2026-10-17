#pragma once

#include "protocol.hpp"

#include <filesystem>
#include <string_view>

namespace tongs {

// Captures the client area of the game window into a PNG file in `folder`
// (created when missing), named <name>.png or shot-<time>.png, and keeps at
// most kMaxScreenshots files there by deleting the oldest. Tries PrintWindow
// with PW_RENDERFULLCONTENT first (works while the window is covered), then
// a copy from the screen. Any thread may call it.
ScreenshotResult TakeScreenshot(const std::filesystem::path& folder, std::string_view name);

inline constexpr int kMaxScreenshots = 100;

}  // namespace tongs
