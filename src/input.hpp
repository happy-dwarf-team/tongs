#pragma once

#include "keys.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// One SDL event to queue for the game; game_input.cpp turns it into an
// SDL_Event. Mouse positions are in the game window's client pixels, as SDL
// reports them.
struct InputEvent {
    enum class Type { KeyDown, KeyUp, Text, MouseMove, MouseDown, MouseUp, Wheel };
    Type type = Type::KeyDown;
    KeyPress key;           // KeyDown, KeyUp
    char text[5] = {};      // Text: one UTF-8 character, zero-terminated
    int x = 0;              // mouse events
    int y = 0;
    int xrel = 0;           // MouseMove: movement since the previous position
    int yrel = 0;
    int button = 1;         // MouseDown / MouseUp: 1 left, 2 middle, 3 right
    int clicks = 1;         // MouseDown / MouseUp: 2 for the second press of a double click
    int wheel = 0;          // Wheel: steps, positive = away from the player (scroll up)
};

// A key press as SDL reports it: key-down, the key's character if it has
// one, key-up.
std::vector<InputEvent> KeyPressEvents(const KeyPress& key);

// A chord as a player types it: each modifier key down (Ctrl, Alt, Shift),
// the key down, its character, the key up, the modifier keys up in reverse;
// every event's `mod` holds the modifiers down at that moment. The character
// is the key's own without modifiers, an upper-case letter with Shift alone,
// and none otherwise (characters a layout gives a shifted key are typed with
// `text`). Without modifiers the same events as KeyPressEvents.
std::vector<InputEvent> KeyChordEvents(const KeyChord& chord);

// One Text event per character. The game decodes only the first byte or
// two of SDL_TEXTINPUT into one 8-bit character, so only U+0020-U+007E and
// U+00A0-U+00FF arrive intact; returns an error text for anything else.
std::string TextEvents(std::string_view utf8, std::vector<InputEvent>& out);

}  // namespace tongs
