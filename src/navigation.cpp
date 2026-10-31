#include "navigation.hpp"

#include <cmath>
#include <cstdio>

namespace tongs {
namespace {

std::string Number(float value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", static_cast<double>(value));
    std::string out = text;
    while (out.back() == '0') out.pop_back();
    if (out.back() == '.') out.pop_back();
    return out == "-0" ? "0" : out;
}

}  // namespace

WorldFailure NavigationRefusal(bool open, const NavigationFacts& f) {
    const std::string province = "province " + std::to_string(f.id);
    if (!open) {
        if (f.camera_locked) {
            return {"camera_locked", "the camera is locked (debug_lockcamera, or a window that holds it such as the in-game menu)"};
        }
        return {};
    }
    if (!f.land || f.wasteland) {
        return {"not_selectable", province + (f.wasteland ? " is a wasteland" : " is not land (a sea or a lake)")};
    }
    if (f.terra_incognita) return {"terra_incognita", province + " is in terra incognita for the player"};
    if (f.selection_blocked) {
        return {"selection_blocked", "the game does not select provinces now (a province-picking mode or the tutorial)"};
    }
    return {};
}

bool CameraReached(const CameraRaw& c, float x, float z, float tolerance) {
    float dx = std::fabs(c.x - x);
    if (c.wrap && c.width > 0) {
        dx = std::fmod(dx, c.width);
        dx = std::fmin(dx, c.width - dx);
    }
    return dx <= tolerance && std::fabs(c.z - z) <= tolerance;
}

std::string CameraMembers(float target_x, float target_z, bool settled, bool locked) {
    std::string out = std::string(",\"camera\":{\"settled\":") + (settled ? "true" : "false");
    out += ",\"target\":{\"x\":" + Number(target_x) + ",\"z\":" + Number(target_z) + "}";
    if (locked) out += ",\"locked\":true";
    return out + "}";
}

}  // namespace tongs
