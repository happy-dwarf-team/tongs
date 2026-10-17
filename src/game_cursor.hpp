#pragma once

#include <anvil/anvil_plugin.h>

#include <string>

namespace tongs {

// The game's GUI does not take the pointer position from SDL mouse events: once
// per frame 0x14E5E50 asks Windows (GetCursorPos) or, without focus, SDL's
// internal mouse state. Tongs hooks that function: while a virtual cursor is
// set, the function returns its client coordinates instead, so clicks and
// hovering land where the mouse commands put them. The virtual cursor ends
// with `release`, or as soon as the real cursor moves.
struct CursorLayout {
    unsigned rva = 0x14E5E50;  // POINT* GetGuiCursor(POINT* out, bool, bool), client coordinates
};

// Installs the hook; returns an error text, or an empty string on success.
std::string InstallCursorHook(const AnvilHost* host, AnvilPlugin* self, const CursorLayout& layout);

void SetVirtualCursor(int x, int y);
void ReleaseVirtualCursor();

}  // namespace tongs
