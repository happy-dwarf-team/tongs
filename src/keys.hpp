#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace tongs {

// One key as SDL 2 describes it: the layout-dependent key code (SDL_Keycode)
// and the physical key (SDL_Scancode), which the game reads from SDL_KEYDOWN,
// and the character SDL reports in SDL_TEXTINPUT for it (0 = none), as for a
// key pressed without modifiers on a US layout.
struct KeyPress {
    std::int32_t sym = 0;
    std::int32_t scancode = 0;
    char text = 0;
};

// Key names accepted by `key <name>`: a-z, 0-9, f1-f12, space, return,
// escape, tab, backspace, delete, minus, equals, kp_plus, kp_minus, kp_enter,
// up, down, left, right, home, end, page_up, page_down. Case-insensitive.
std::optional<KeyPress> FindKey(std::string_view name);

// The keys behind pause, resume and speed (see docs/design.md, "Input").
inline constexpr std::string_view kPauseKeyName = "space";
inline constexpr std::string_view kSpeedUpKeyName = "kp_plus";
inline constexpr std::string_view kSpeedDownKeyName = "kp_minus";

}  // namespace tongs
