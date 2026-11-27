#pragma once

#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>

namespace sgl {

struct DisplaySize { unsigned width, height; };

enum class DisplayMode { Windowed, Borderless };

struct DisplayConfig {
    DisplaySize size{};
    DisplayMode mode = DisplayMode::Borderless;
    // [display] max_fps: absent keeps the game's own 50 fps limiter; 0 removes it; 10..1000 sets the limiter's
    // minimum frame time to the nearest millisecond.
    std::optional<unsigned> max_fps;
    // [display] vsync: the default 1 makes presentation wait for the vertical blank, which paces frames evenly and
    // caps them at the refresh rate; 0 presents at once, so max_fps alone limits the frame rate.
    bool vsync = true;
    // [display] show_fps=1 draws frame rate and frame times into the top-left corner.
    bool show_fps = false;
    // [display] exact_blend: the default 1 blends translucent draws through a copy of the target with the original
    // integer formulas; 0 uses hardware blend states, at most one step apart, without the copies.
    bool exact_blend = true;
};

// high_res.ini in the directory of the module containing this code: the renderer DLL, which normally sits next to the
// game executable but may also be loaded from a mods directory. Empty when the module path cannot be determined.
std::filesystem::path display_config_path();
// The four sizes the renderer's own layout hooks support on the verified GOG/Steam executable.
bool is_supported_display_size(DisplaySize size) noexcept;
// Any size a game with its own wide-screen layout (a patched executable) may request: 640x480 to 8192x8192.
bool is_valid_display_size(DisplaySize size) noexcept;
// True when [display] width or height is 0 or missing, or the file does not exist: the executable is patched to lay
// out its own screen size, so the renderer follows the size the game requests and installs no layout hooks.
bool display_size_from_game(const std::filesystem::path& path);
// game_size set (a game with its own layout): every valid size is accepted and [display] width/height are ignored.
// The render size is then game_size when valid, otherwise the desktop size.
bool display_config(HWND window, const std::filesystem::path& path, DisplayConfig& config, std::string& error,
    std::optional<DisplaySize> game_size = std::nullopt);
bool save_display_config(const std::filesystem::path& path, DisplaySize size, DisplayMode mode, std::string& error);

}
