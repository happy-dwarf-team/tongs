#include "keys.hpp"

#include <array>
#include <cctype>
#include <string>

namespace tongs {
namespace {

// SDL 2.0 key codes: printable keys are their lowercase character, the rest
// are the scancode with bit 30 set (SDLK_SCANCODE_MASK).
constexpr std::int32_t kScancodeMask = 1 << 30;

struct NamedKey {
    std::string_view name;
    KeyPress key;
};

constexpr NamedKey Special(std::string_view name, std::int32_t scancode, char text = 0) {
    return {name, {scancode | kScancodeMask, scancode, text}};
}

constexpr std::array kNamedKeys = {
    NamedKey{"return", {'\r', 40}},
    NamedKey{"escape", {27, 41}},
    NamedKey{"backspace", {8, 42}},
    NamedKey{"tab", {'\t', 43}},
    NamedKey{"space", {' ', 44, ' '}},
    NamedKey{"minus", {'-', 45, '-'}},
    NamedKey{"equals", {'=', 46, '='}},
    NamedKey{"grave", {'`', 53, '`'}},  // the key left of 1 (US layout); opens the game console
    NamedKey{"delete", {127, 76}},
    Special("home", 74),
    Special("page_up", 75),
    Special("end", 77),
    Special("page_down", 78),
    Special("right", 79),
    Special("left", 80),
    Special("down", 81),
    Special("up", 82),
    Special("kp_minus", 86, '-'),
    Special("kp_plus", 87, '+'),
    Special("kp_enter", 88),
    Special("ctrl", 224),   // SDL_SCANCODE_LCTRL
    Special("shift", 225),  // SDL_SCANCODE_LSHIFT
    Special("alt", 226),    // SDL_SCANCODE_LALT
};

std::string Lower(std::string_view text) {
    std::string lower(text);
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lower;
}

}  // namespace

std::optional<KeyPress> FindKey(std::string_view name) {
    const std::string lower = Lower(name);

    if (lower.size() == 1) {
        const char ch = lower[0];
        if (ch >= 'a' && ch <= 'z') return KeyPress{ch, 4 + (ch - 'a'), ch};   // SDL_SCANCODE_A = 4
        if (ch >= '1' && ch <= '9') return KeyPress{ch, 30 + (ch - '1'), ch};  // SDL_SCANCODE_1 = 30
        if (ch == '0') return KeyPress{'0', 39, '0'};
    }
    if (lower.size() >= 2 && lower.size() <= 3 && lower[0] == 'f') {
        int number = 0;
        for (std::size_t i = 1; i < lower.size(); ++i) {
            if (lower[i] < '0' || lower[i] > '9') return std::nullopt;
            number = number * 10 + (lower[i] - '0');
        }
        if (number >= 1 && number <= 12) {
            const std::int32_t scancode = 58 + (number - 1);  // SDL_SCANCODE_F1 = 58
            return KeyPress{scancode | kScancodeMask, scancode};
        }
        return std::nullopt;
    }
    for (const NamedKey& named : kNamedKeys) {
        if (named.name == lower) return named.key;
    }
    return std::nullopt;
}

std::optional<KeyChord> FindKeyChord(std::string_view text) {
    KeyChord chord;
    for (;;) {
        const std::size_t plus = text.find('+');
        // A `+` at the very end is part of no name (there is no `+` key); `kp_plus` is spelled out.
        if (plus == std::string_view::npos || plus == 0) break;
        const std::string mod = Lower(text.substr(0, plus));
        std::uint16_t bit = 0;
        if (mod == "shift") bit = kModShift;
        else if (mod == "ctrl" || mod == "control") bit = kModCtrl;
        else if (mod == "alt") bit = kModAlt;
        else return std::nullopt;
        if ((chord.mods & bit) != 0) return std::nullopt;
        chord.mods |= bit;
        text.remove_prefix(plus + 1);
    }
    const auto key = FindKey(text);
    if (!key) return std::nullopt;
    chord.key = *key;
    return chord;
}

}  // namespace tongs
