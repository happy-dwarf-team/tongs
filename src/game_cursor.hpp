#pragma once

#include <cstdint>
#include <string>

namespace tongs {

// The game's GUI does not take the pointer position from SDL mouse events: once
// per frame 0x14E5E50 asks Windows (GetCursorPos) or, without focus, SDL's
// internal mouse state. Tongs hooks that function: while a virtual cursor is
// set, the function returns its client coordinates instead, so clicks and
// hovering land where the mouse commands put them. The virtual cursor ends
// with `release`, or as soon as the real cursor moves. The function is
// symbols.toml's `gui_cursor`: POINT* (POINT* out, bool, bool).

// Installs the hook while loading; returns an error text, or an empty string on success.
std::string InstallCursorHook();

void SetVirtualCursor(int x, int y);
void ReleaseVirtualCursor();
// False after `release`, and once the hook saw the real cursor move and handed it back.
bool VirtualCursorHeld();

// Where the game's interface sees the pointer: Tongs' virtual pointer while it is held,
// else the real cursor in the game window's client pixels (the focused path of the
// cursor query; without focus the game asks SDL's mouse state, which follows the same
// pointer). False when the window is not found.
bool ReadGuiPointer(std::uintptr_t window, int& x, int& y, bool& tongs_pointer);

}  // namespace tongs
