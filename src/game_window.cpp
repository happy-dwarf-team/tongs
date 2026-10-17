#include "game_window.hpp"

#include <windows.h>

namespace tongs {
namespace {

struct Search {
    DWORD process_id;
    HWND best;
    long best_area;
};

BOOL CALLBACK Visit(HWND hwnd, LPARAM param) {
    auto& search = *reinterpret_cast<Search*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search.process_id || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[32] = {};
    GetClassNameW(hwnd, cls, 32);
    if (wcscmp(cls, L"SDL_app") != 0) return TRUE;
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const long area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (search.best == nullptr || area > search.best_area) {
        search.best = hwnd;
        search.best_area = area;
    }
    return TRUE;
}

}  // namespace

WindowSnapshot ReadGameWindow() {
    WindowSnapshot w;
    Search search{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(Visit, reinterpret_cast<LPARAM>(&search));
    if (search.best == nullptr) {
        w.reason = "no visible game window (SDL_app) yet";
        return w;
    }
    const HWND hwnd = search.best;
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    w.found = true;
    w.handle = reinterpret_cast<std::uintptr_t>(hwnd);
    w.width = client.right - client.left;
    w.height = client.bottom - client.top;
    w.screen_x = origin.x;
    w.screen_y = origin.y;
    w.focused = GetForegroundWindow() == hwnd;
    w.minimized = IsIconic(hwnd) != 0;
    w.thread_id = GetWindowThreadProcessId(hwnd, nullptr);
    w.dpi = GetDpiForWindow(hwnd);
    return w;
}

}  // namespace tongs
