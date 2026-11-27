#include "game_window_binding.hpp"

#include "mouse_input_transform.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

using SetWindowPosFunction = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using MoveWindowFunction = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
using SetCaptureFunction = HWND(WINAPI*)(HWND);
using SetCursorPosFunction = BOOL(WINAPI*)(int, int);

HWND bound_window = nullptr;
bool foreground_capture = false;
SetWindowPosFunction original_set_window_pos = nullptr;
MoveWindowFunction original_move_window = nullptr;
SetCaptureFunction original_set_capture = nullptr;
SetCursorPosFunction original_set_cursor_pos = nullptr;

BOOL WINAPI game_set_window_pos(HWND window, HWND insert_after, int x, int y, int width, int height, UINT flags) {
    if (window && window == bound_window) flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER;
    return original_set_window_pos(window, insert_after, x, y, width, height, flags);
}

BOOL WINAPI game_move_window(HWND window, int x, int y, int width, int height, BOOL repaint) {
    if (window && window == bound_window) return TRUE;
    return original_move_window(window, x, y, width, height, repaint);
}

HWND WINAPI game_set_capture(HWND window) {
    if (window && window == bound_window && !(foreground_capture && GetForegroundWindow() == window))
        return GetCapture();
    return original_set_capture(window);
}

BOOL WINAPI game_set_cursor_pos(int x, int y) {
    POINT origin{0, 0};
    if (bound_window && ClientToScreen(bound_window, &origin)) {
        int client_x = x - origin.x;
        int client_y = y - origin.y;
        if (sgl::map_game_point_to_client(client_x, client_y)) {
            x = origin.x + client_x;
            y = origin.y + client_y;
        }
    }
    return original_set_cursor_pos(x, y);
}

struct ImportSlot {
    const char* name;
    void* replacement;
    void** original;
    void** address;
};

std::array<ImportSlot, 4> slots{{
    {"SetWindowPos", reinterpret_cast<void*>(&game_set_window_pos), reinterpret_cast<void**>(&original_set_window_pos), nullptr},
    {"MoveWindow", reinterpret_cast<void*>(&game_move_window), reinterpret_cast<void**>(&original_move_window), nullptr},
    {"SetCapture", reinterpret_cast<void*>(&game_set_capture), reinterpret_cast<void**>(&original_set_capture), nullptr},
    {"SetCursorPos", reinterpret_cast<void*>(&game_set_cursor_pos), reinterpret_cast<void**>(&original_set_cursor_pos), nullptr},
}};

void** find_user32_import(HMODULE module, const char* name) noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return nullptr;
    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), "USER32.dll") != 0) continue;
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) == 0)
                return reinterpret_cast<void**>(&addresses->u1.Function);
        }
    }
    return nullptr;
}

bool write_slot(void** address, void* value) noexcept {
    DWORD protection = 0;
    if (!VirtualProtect(address, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *address = value;
    DWORD discarded = 0;
    VirtualProtect(address, sizeof(void*), protection, &discarded);
    return true;
}

}

namespace sgl {

bool install_game_window_binding(HWND window, bool keep_foreground_capture) noexcept {
    if (!window || !IsWindow(window)) return false;
    if (bound_window) return bound_window == window;
    foreground_capture = keep_foreground_capture;
    const HMODULE game = GetModuleHandleW(nullptr);
    if (!game) return false;
    for (auto& slot : slots) {
        slot.address = find_user32_import(game, slot.name);
        if (!slot.address || !*slot.address) {
            for (auto& reset : slots) reset.address = nullptr;
            return false;
        }
    }
    bound_window = window;
    for (auto& slot : slots) {
        *slot.original = *slot.address;
        if (!write_slot(slot.address, slot.replacement)) {
            remove_game_window_binding();
            return false;
        }
    }
    return true;
}

void remove_game_window_binding() noexcept {
    for (auto& slot : slots) {
        if (slot.address && *slot.original && *slot.address == slot.replacement) write_slot(slot.address, *slot.original);
        slot.address = nullptr;
    }
    bound_window = nullptr;
    foreground_capture = false;
}

}
