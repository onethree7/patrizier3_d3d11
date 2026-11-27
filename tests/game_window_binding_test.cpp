#include "game_window_binding.hpp"
#include "mouse_input_transform.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

bool same_rect(HWND window, const RECT& expected) {
    RECT actual = {};
    return GetWindowRect(window, &actual) && std::memcmp(&actual, &expected, sizeof(RECT)) == 0;
}

} // namespace

// These calls compile to this executable's USER32 import slots, the same mechanism the game uses.
int wmain() {
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    RECT frame{0, 0, 800, 600};
    if (!AdjustWindowRectEx(&frame, style, FALSE, 0)) return 1;
    const HWND window = CreateWindowExW(0, L"STATIC", L"", style, 120, 90, frame.right - frame.left,
        frame.bottom - frame.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND other = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 200, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window || !other) return 2;
    RECT placed = {};
    if (!GetWindowRect(window, &placed)) return 3;
    if (!sgl::install_mouse_input_transform(window, 400, 300, 400, 300)) return 4;
    if (!sgl::install_game_window_binding(window, false) || !sgl::install_game_window_binding(window, false) ||
        sgl::install_game_window_binding(other, true)) return 5;

    int result = 0;
    if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 640, 480, SWP_NOACTIVATE) || !same_rect(window, placed) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST)) result = 10;
    else if (!MoveWindow(window, 0, 0, 640, 480, TRUE) || !same_rect(window, placed)) result = 11;
    else if (!SetWindowPos(other, nullptr, 30, 40, 300, 200, SWP_NOZORDER | SWP_NOACTIVATE) ||
        !same_rect(other, RECT{30, 40, 330, 240})) result = 12;
    if (!result) {
        SetCapture(window);
        if (GetCapture() == window) result = 13;
    }

    POINT saved = {};
    const bool have_cursor = GetCursorPos(&saved) != FALSE;
    if (!result && have_cursor) {
        POINT origin{0, 0};
        POINT actual = {};
        // Logical (10,20) in a 400x300 game on an 800x600 client is the center of physical pixel block (20,40).
        if (!ClientToScreen(window, &origin) || !SetCursorPos(origin.x + 10, origin.y + 20) || !GetCursorPos(&actual))
            std::puts("Cursor warp check skipped: no interactive cursor.");
        else if (actual.x != origin.x + 21 || actual.y != origin.y + 41) result = 14;
    }

    sgl::remove_game_window_binding();
    if (!result && !sgl::install_game_window_binding(window, true)) result = 16;
    if (!result && GetForegroundWindow() != window) {
        SetCapture(window);
        if (GetCapture() == window) result = 17;
    }
    sgl::remove_game_window_binding();
    if (have_cursor) SetCursorPos(saved.x, saved.y);
    sgl::remove_mouse_input_transform();
    if (!result && (!SetWindowPos(window, nullptr, 10, 20, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE) ||
        !same_rect(window, RECT{10, 20, 650, 500}))) result = 15;
    DestroyWindow(other);
    DestroyWindow(window);
    if (!result) std::puts("Game window placement, capture and cursor warps are rebound for the bound window only.");
    return result;
}
