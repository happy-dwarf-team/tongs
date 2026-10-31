#pragma once

#include "world.hpp"

#include <cstdint>
#include <string>

namespace tongs {

// Navigation (docs/design.md, "Navigation (0.21.0)"): `province open ID` and
// `province goto ID`, client calls (ADR 0003) made on the main thread after an
// outer frame (game_navigation.cpp). Nothing here touches game memory.

// What the main thread read before calling, on the frame of the call.
struct NavigationFacts {
    std::int32_t id = 0;
    bool land = false;
    bool wasteland = false;
    bool terra_incognita = false;    // the province links' test fails: the game would do nothing
    bool selection_blocked = false;  // session mode 0x25 or the tutorial guard: selecting does nothing
    bool camera_locked = false;      // debug_lockcamera
    bool discover = false;           // opening the view would post the discover-capital command
    // For the call: the province, the session and its selection object.
    std::uintptr_t province = 0;
    std::uintptr_t session = 0;
    std::uintptr_t selection = 0;
};

// The camera: where it is and where it is heading (map coordinates). The game
// moves the target itself too: the pointer at the screen edge scrolls it.
struct CameraRaw {
    bool locked = false;
    bool wrap = false;  // the map wraps around horizontally
    float width = 0;    // the map's width, for the wrap
    float x = 0, z = 0;
    float target_x = 0, target_z = 0;
};

// How close the current position must come to the target for `settled`.
constexpr float kCameraTolerance = 1.0f;

// The refusal of `province open` (open) or `province goto` before anything is
// called: not_selectable, terra_incognita, selection_blocked or camera_locked.
// The discover case is the caller's: it needs the game-call settings.
WorldFailure NavigationRefusal(bool open, const NavigationFacts& facts);

// Whether the camera's current position is within `tolerance` of (x, z), the
// target the call set; on a wrapping map x is compared around the wrap.
bool CameraReached(const CameraRaw& camera, float x, float z, float tolerance = kCameraTolerance);

// `,"camera":{"settled":…,"target":{"x":…,"z":…}}`, with `"locked":true` when it is.
std::string CameraMembers(float target_x, float target_z, bool settled, bool locked);

}  // namespace tongs
