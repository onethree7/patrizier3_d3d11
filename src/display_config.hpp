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
    // [display] max_fps: absent keeps the game's own 50 fps limiter; 0 removes it (presentation still waits for
    // vertical blank unless vsync=0); 10..1000 sets the limiter's minimum frame time to the nearest millisecond.
    std::optional<unsigned> max_fps;
    // [display] vsync: 1 (default) presents on the vertical blank; 0 presents at once. In a composited window the
    // compositor still shows at most one frame per refresh, so vsync=0 removes the wait, not the display limit.
    bool vsync = true;
    // [display] show_fps=1 draws frame rate and frame times into the top-left corner.
    bool show_fps = false;
};

bool is_supported_display_size(DisplaySize size) noexcept;
bool display_config(HWND window, const std::filesystem::path& path, DisplayConfig& config, std::string& error);
bool save_display_config(const std::filesystem::path& path, DisplaySize size, DisplayMode mode, std::string& error);

}
