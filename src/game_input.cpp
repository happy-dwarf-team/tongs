#include "game_input.hpp"

#include <windows.h>

#include <cstring>

namespace tongs {
namespace {

// SDL 2.0 event layout (SDL_events.h): SDL_Event is a 56-byte union; for
// SDL_KeyboardEvent the members are type, timestamp, windowID, state, repeat,
// two padding bytes and SDL_Keysym {scancode, sym, mod, unused}.
constexpr std::uint32_t kKeyDown = 0x300;  // SDL_KEYDOWN
constexpr std::uint32_t kKeyUp = 0x301;    // SDL_KEYUP
constexpr std::uint32_t kTextInput = 0x303;  // SDL_TEXTINPUT: type, timestamp, windowID, char text[32]
constexpr std::size_t kEventSize = 56;

struct alignas(8) SdlEvent {
    unsigned char bytes[kEventSize];
};

SdlEvent KeyEvent(std::uint32_t type, const KeyPress& key) {
    SdlEvent event{};
    const std::uint8_t state = type == kKeyDown ? 1 : 0;  // SDL_PRESSED / SDL_RELEASED
    std::memcpy(event.bytes + 0x00, &type, sizeof type);
    // timestamp (+0x04) is set by SDL_PushEvent; windowID (+0x08) stays 0:
    // the game's event loop does not read it for key events.
    event.bytes[0x0C] = state;
    event.bytes[0x0D] = 0;  // repeat
    std::memcpy(event.bytes + 0x10, &key.scancode, sizeof key.scancode);
    std::memcpy(event.bytes + 0x14, &key.sym, sizeof key.sym);
    // mod (+0x18) stays 0: no modifier keys held.
    return event;
}

SdlEvent TextEvent(char text) {
    SdlEvent event{};
    std::memcpy(event.bytes + 0x00, &kTextInput, sizeof kTextInput);
    event.bytes[0x0C] = static_cast<unsigned char>(text);  // UTF-8, zero-terminated
    return event;
}

using PushEventFn = int (*)(SdlEvent* event);

// 1 = queued, 0 = dropped by an event filter, -1 = SDL error (queue full or shut down).
int PushGuarded(PushEventFn push, SdlEvent* event) {
    __try {
        return push(event);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return -2;
    }
}

const char* PushError(int result) {
    switch (result) {
        case 0: return "SDL_PushEvent: the event was filtered out";
        case -2: return "SDL_PushEvent: access violation";
        default: return "SDL_PushEvent failed (event queue full or shut down)";
    }
}

}  // namespace

std::string PressKey(std::uintptr_t image_base, const InputLayout& layout, const KeyPress& key) {
    const std::uintptr_t expected = image_base + layout.push_event_rva;
    std::uintptr_t slot = 0;
    std::memcpy(&slot, reinterpret_cast<const void*>(image_base + layout.push_event_slot_rva), sizeof slot);
    if (slot != expected) {
        // Before SDL starts the slot holds a stub; with SDL_DYNAMIC_API set it points into another SDL.
        return "SDL is not ready or its dynamic API points elsewhere";
    }
    const auto push = reinterpret_cast<PushEventFn>(expected);
    SdlEvent down = KeyEvent(kKeyDown, key);
    if (const int result = PushGuarded(push, &down); result != 1) return PushError(result);
    if (key.text != 0) {
        // SDL sends the character after the key-down, while text input is on (the SDL 2 default).
        SdlEvent text = TextEvent(key.text);
        if (const int result = PushGuarded(push, &text); result != 1) return PushError(result);
    }
    SdlEvent up = KeyEvent(kKeyUp, key);
    if (const int result = PushGuarded(push, &up); result != 1) return PushError(result);
    return {};
}

}  // namespace tongs
