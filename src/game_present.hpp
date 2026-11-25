#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tongs {

// A frame read from the game's back buffer: top-down, 4 bytes per pixel in
// B, G, R, A order, alpha 0xFF.
struct FramePixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

// The game draws through Direct3D 9 and shows each frame with one call of
// IDirect3DDevice9::Present, in its frame presentation (docs/design.md, "Screenshots").
// HookPresent puts a mid hook on that call (symbols.toml `present_call`), where
// the back buffer holds the finished frame and rcx the device. The hook does
// nothing until CapturePresentedFrame asks for a frame.
//
// Returns an error text, or an empty string on success. Call while loading.
std::string HookPresent();

// True once HookPresent succeeded.
bool PresentHooked();

// Asks the main thread to copy the back buffer of the next frame it presents
// and waits up to `timeout_ms` for it. Not on the main thread (it would wait
// for itself). Returns an error text, or an empty string on success.
std::string CapturePresentedFrame(int timeout_ms, FramePixels& out);

}  // namespace tongs
