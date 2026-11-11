#include "input.hpp"

#include <cstdio>

namespace tongs {
namespace {

std::uint16_t ModifierBit(std::int32_t scancode) {
    switch (scancode) {
        case 224: return kModCtrl;
        case 225: return kModShift;
        case 226: return kModAlt;
        default: return 0;
    }
}

}  // namespace

std::vector<InputEvent> KeyPressEvents(const KeyPress& key) {
    std::vector<InputEvent> events;
    InputEvent down;
    down.type = InputEvent::Type::KeyDown;
    down.key = key;
    events.push_back(down);
    if (key.text != 0) {
        InputEvent text;
        text.type = InputEvent::Type::Text;
        text.text[0] = key.text;
        events.push_back(text);
    }
    InputEvent up = down;
    up.type = InputEvent::Type::KeyUp;
    events.push_back(up);
    return events;
}

std::vector<InputEvent> KeyChordEvents(const KeyChord& chord) {
    struct Modifier {
        std::uint16_t bit;
        const char* name;
    };
    static constexpr Modifier kOrder[] = {{kModCtrl, "ctrl"}, {kModAlt, "alt"}, {kModShift, "shift"}};
    std::vector<InputEvent> events;
    std::vector<KeyPress> held;
    std::uint16_t mods = 0;
    for (const Modifier& m : kOrder) {
        if ((chord.mods & m.bit) == 0) continue;
        mods |= m.bit;
        KeyPress modifier = *FindKey(m.name);
        modifier.mod = mods;
        InputEvent down;
        down.type = InputEvent::Type::KeyDown;
        down.key = modifier;
        events.push_back(down);
        held.push_back(modifier);
    }
    KeyPress key = chord.key;
    key.mod = mods;
    if (mods == kModShift && key.text >= 'a' && key.text <= 'z') {
        key.text = static_cast<char>(key.text - 'a' + 'A');
    } else if (mods != 0) {
        key.text = 0;
    }
    for (InputEvent& event : KeyPressEvents(key)) events.push_back(event);
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        InputEvent up;
        up.type = InputEvent::Type::KeyUp;
        up.key = *it;
        events.push_back(up);
    }
    // A key-up carries the modifiers still down after it, as SDL reports them.
    for (std::size_t i = events.size() - held.size(); i < events.size(); ++i) {
        mods = static_cast<std::uint16_t>(mods & ~ModifierBit(events[i].key.scancode));
        events[i].key.mod = mods;
    }
    return events;
}

std::string TextEvents(std::string_view utf8, std::vector<InputEvent>& out) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        unsigned code = 0;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0) == 0xC0 && i + 1 < utf8.size() &&
                   (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80) {
            code = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            length = 2;
        } else {
            return "text: only characters U+0020-U+007E and U+00A0-U+00FF can be typed";
        }
        if (code < 0x20 || code == 0x7F || (code >= 0x80 && code < 0xA0) || code > 0xFF) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "text: U+%04X cannot be typed (only U+0020-U+007E and U+00A0-U+00FF)", code);
            return buf;
        }
        InputEvent event;
        event.type = InputEvent::Type::Text;
        for (std::size_t b = 0; b < length; ++b) event.text[b] = utf8[i + b];
        out.push_back(event);
        i += length;
    }
    return {};
}

}  // namespace tongs
