#include "game_input.hpp"

#include <windows.h>

#include <cstring>

namespace tongs {
namespace {

// SDL 2.0.4 event layout (SDL_events.h). SDL_Event is a 56-byte union; every
// member starts with type (+0x00), timestamp (+0x04, set by SDL_PushEvent) and
// windowID (+0x08, left 0: the game's event loop does not read it).
constexpr std::uint32_t kKeyDown = 0x300;      // SDL_KEYDOWN
constexpr std::uint32_t kKeyUp = 0x301;        // SDL_KEYUP
constexpr std::uint32_t kTextInput = 0x303;    // SDL_TEXTINPUT
constexpr std::uint32_t kMouseMotion = 0x400;  // SDL_MOUSEMOTION
constexpr std::uint32_t kMouseDown = 0x401;    // SDL_MOUSEBUTTONDOWN
constexpr std::uint32_t kMouseUp = 0x402;      // SDL_MOUSEBUTTONUP
constexpr std::uint32_t kMouseWheel = 0x403;   // SDL_MOUSEWHEEL
constexpr std::size_t kEventSize = 56;

struct alignas(8) SdlEvent {
    unsigned char bytes[kEventSize];

    template <typename T>
    void Put(std::size_t offset, T value) {
        std::memcpy(bytes + offset, &value, sizeof value);
    }
};

SdlEvent ToSdl(const InputEvent& in) {
    SdlEvent e{};
    switch (in.type) {
        case InputEvent::Type::KeyDown:
        case InputEvent::Type::KeyUp:
            // SDL_KeyboardEvent: state +0x0C, repeat +0x0D, keysym {scancode +0x10, sym +0x14, mod +0x18 (0)}.
            e.Put(0x00, in.type == InputEvent::Type::KeyDown ? kKeyDown : kKeyUp);
            e.Put<std::uint8_t>(0x0C, in.type == InputEvent::Type::KeyDown ? 1 : 0);  // SDL_PRESSED / SDL_RELEASED
            e.Put(0x10, in.key.scancode);
            e.Put(0x14, in.key.sym);
            break;
        case InputEvent::Type::Text:
            // SDL_TextInputEvent: char text[32] at +0x0C, UTF-8, zero-terminated.
            e.Put(0x00, kTextInput);
            std::memcpy(e.bytes + 0x0C, in.text, sizeof in.text);
            break;
        case InputEvent::Type::MouseMove:
            // SDL_MouseMotionEvent: which +0x0C (0, not SDL_TOUCH_MOUSEID, which the game skips),
            // button state +0x10, x +0x14, y +0x18, xrel +0x1C, yrel +0x20.
            e.Put(0x00, kMouseMotion);
            e.Put(0x14, in.x);
            e.Put(0x18, in.y);
            e.Put(0x1C, in.xrel);
            e.Put(0x20, in.yrel);
            break;
        case InputEvent::Type::MouseDown:
        case InputEvent::Type::MouseUp:
            // SDL_MouseButtonEvent: which +0x0C, button +0x10, state +0x11, clicks +0x12, x +0x14, y +0x18.
            e.Put(0x00, in.type == InputEvent::Type::MouseDown ? kMouseDown : kMouseUp);
            e.Put(0x10, static_cast<std::uint8_t>(in.button));
            e.Put<std::uint8_t>(0x11, in.type == InputEvent::Type::MouseDown ? 1 : 0);
            e.Put(0x12, static_cast<std::uint8_t>(in.clicks));
            e.Put(0x14, in.x);
            e.Put(0x18, in.y);
            break;
        case InputEvent::Type::Wheel:
            // SDL_MouseWheelEvent: which +0x0C, x +0x10, y +0x14 (the game reads y), direction +0x18 (normal).
            e.Put(0x00, kMouseWheel);
            e.Put(0x14, in.wheel);
            break;
    }
    return e;
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

std::string PushInput(std::uintptr_t image_base, const InputLayout& layout, const std::vector<InputEvent>& events) {
    const std::uintptr_t expected = image_base + layout.push_event_rva;
    std::uintptr_t slot = 0;
    std::memcpy(&slot, reinterpret_cast<const void*>(image_base + layout.push_event_slot_rva), sizeof slot);
    if (slot != expected) {
        // Before SDL starts the slot holds a stub; with SDL_DYNAMIC_API set it points into another SDL.
        return "SDL is not ready or its dynamic API points elsewhere";
    }
    const auto push = reinterpret_cast<PushEventFn>(expected);
    for (const InputEvent& event : events) {
        SdlEvent sdl = ToSdl(event);
        if (const int result = PushGuarded(push, &sdl); result != 1) return PushError(result);
    }
    return {};
}

}  // namespace tongs
