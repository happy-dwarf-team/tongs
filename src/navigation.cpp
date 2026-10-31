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
        if (f.camera_locked) return {"camera_locked", "the camera is locked (debug_lockcamera)"};
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

bool CameraSettled(const CameraRaw& c, float tolerance) {
    return std::fabs(c.x - c.target_x) <= tolerance && std::fabs(c.z - c.target_z) <= tolerance;
}

std::string CameraMembers(const CameraRaw& c, bool settled) {
    std::string out = std::string(",\"camera\":{\"settled\":") + (settled ? "true" : "false");
    out += ",\"target\":{\"x\":" + Number(c.target_x) + ",\"z\":" + Number(c.target_z) + "}";
    if (c.locked) out += ",\"locked\":true";
    return out + "}";
}

}  // namespace tongs
