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
    std::uint16_t mod = 0;  // SDL_Keymod of the event: the modifier keys held (KMOD_LSHIFT, ...)
};

// SDL_Keymod bits of the left modifier keys, which a chord holds.
inline constexpr std::uint16_t kModShift = 0x0001;  // KMOD_LSHIFT
inline constexpr std::uint16_t kModCtrl = 0x0040;   // KMOD_LCTRL
inline constexpr std::uint16_t kModAlt = 0x0100;    // KMOD_LALT

// A key with the modifier keys held while it is pressed: `ctrl+a`,
// `shift+left`, `ctrl+shift+end`. The game keeps its own record of the keys
// held, from the SDL_KEYDOWN and SDL_KEYUP it handles (CPdxKeyBoard), and asks
// it for Shift, Ctrl and Alt; so a chord is queued as a player types it: the
// modifier keys down, the key, the modifier keys up (docs/design.md, "Key
// chords").
struct KeyChord {
    KeyPress key;
    std::uint16_t mods = 0;  // kModShift | kModCtrl | kModAlt
};

// Key names accepted by `key <name>`: a-z, 0-9, f1-f12, space, return,
// escape, tab, backspace, delete, minus, equals, grave, kp_plus, kp_minus, kp_enter,
// up, down, left, right, home, end, page_up, page_down. Case-insensitive.
std::optional<KeyPress> FindKey(std::string_view name);

// `[ctrl+][alt+][shift+]NAME`, the modifiers in any order and case, each at
// most once; NAME as FindKey takes it, or `shift`, `ctrl`, `alt` alone (the
// left modifier keys). Nullopt for anything else.
std::optional<KeyChord> FindKeyChord(std::string_view text);

// The keys behind pause, resume and speed (see docs/design.md, "Input").
inline constexpr std::string_view kPauseKeyName = "space";
inline constexpr std::string_view kSpeedUpKeyName = "kp_plus";
inline constexpr std::string_view kSpeedDownKeyName = "kp_minus";

}  // namespace tongs
