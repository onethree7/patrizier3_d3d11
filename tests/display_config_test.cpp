#include "presentation.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

int main() {
    wchar_t directory[MAX_PATH] = {}, filename[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"p3d", 0, filename)) return 1;
    const std::filesystem::path path(filename);
    struct RemoveFile { std::filesystem::path path; ~RemoveFile() { std::error_code error; std::filesystem::remove(path, error); } } remove_file{path};
    const auto write = [&path](const char* text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        return static_cast<bool>(output);
    };
    const auto read = [&path]() {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    sgl::DisplayConfig config{};
    std::string error;
    constexpr std::array<std::array<unsigned, 2>, 4> supported = {{
        {{1280, 1024}}, {{1600, 900}}, {{1920, 1080}}, {{2560, 1440}}
    }};
    for (const auto& resolution : supported) {
        const auto configuration = "[display]\nwidth=" + std::to_string(resolution[0]) +
            "\nheight=" + std::to_string(resolution[1]) + "\n";
        if (!write(configuration.c_str()) || !sgl::display_config(GetDesktopWindow(), path, config, error) ||
            config.size.width != resolution[0] || config.size.height != resolution[1] ||
            config.mode != sgl::DisplayMode::Borderless) return 2;
    }
    for (const char* invalid : {"[display]\nwidth=800\nheight=600\n", "[display]\nwidth=1024\nheight=768\n",
        "[display]\nwidth=1920\n", "[display]\nwidth=1920\nheight=1081\n",
        "[display]\nwidth=1920x\nheight=1080\n", "[display]\nwidth 1920\nheight=1080\n",
        "[display]\nwidth=\nheight=1080\n", "[display]\nwidth=1920\nheight=1080\nmode=fullscreen\n",
        "[display]\nwidth=1920\nheight=1080\nmax_fps=5\n", "[display]\nwidth=1920\nheight=1080\nmax_fps=1001\n",
        "[display]\nwidth=1920\nheight=1080\nmax_fps=\n", "[display]\nwidth=1920\nheight=1080\nshow_fps=yes\n",
        "[display]\nwidth=1920\nheight=1080\nvsync=2\n", "[display]\nwidth=1920\nheight=1080\nexact_blend=on\n"}) {
        if (!write(invalid) || sgl::display_config(GetDesktopWindow(), path, config, error) || error.empty()) return 3;
    }
    if (config.max_fps || config.show_fps) return 22;
    if (!write("[display]\nwidth=1920\nheight=1080\nshow_fps=1\n") ||
        !sgl::display_config(GetDesktopWindow(), path, config, error) || !config.show_fps) return 25;
    if (!config.exact_blend) return 28;
    if (!write("[display]\nwidth=1920\nheight=1080\nexact_blend=0\n") ||
        !sgl::display_config(GetDesktopWindow(), path, config, error) || config.exact_blend) return 29;
    // Unknown keys are skipped; a repeated key takes its last value.
    if (!write("[display]\nwidth=1280\nwidth=1920\nheight=1080\nextra=1\nmode=windowed\nmode=borderless\n"
            "show_fps=1\nshow_fps=0\nmax_fps=60\nmax_fps=144\n") ||
        !sgl::display_config(GetDesktopWindow(), path, config, error) || config.size.width != 1920 ||
        config.mode != sgl::DisplayMode::Borderless || config.show_fps || !config.max_fps || *config.max_fps != 144)
        return 27;
    for (const auto& [text, expected] : std::array<std::pair<const char*, unsigned>, 3>{{
        {"[display]\nwidth=1920\nheight=1080\nmax_fps=0\n", 0u},
        {"[display]\nwidth=1920\nheight=1080\nmax_fps=144\n", 144u},
        {"[display]\nwidth=1920\nheight=1080\nmax_fps=1000\n", 1000u}}}) {
        if (!write(text) || !sgl::display_config(GetDesktopWindow(), path, config, error) ||
            !config.max_fps || *config.max_fps != expected) return 23;
    }
    if (!write("[display]\nwidth=1920\nheight=1080\nmax_fps=144\n") ||
        !sgl::save_display_config(path, {2560, 1440}, sgl::DisplayMode::Windowed, error) ||
        !sgl::display_config(GetDesktopWindow(), path, config, error) || !config.max_fps || *config.max_fps != 144)
        return 24;
    if (!write("; keep this comment\n[other]\nvalue=preserved\n[display]\nwidth=1920\nheight=1080\nmode=windowed\n[tail]\nvalue=also preserved\n") ||
        !sgl::save_display_config(path, {2560, 1440}, sgl::DisplayMode::Borderless, error)) return 13;
    const auto saved = read();
    if (saved.find("; keep this comment\r\n[other]\r\nvalue=preserved\r\n[display]\r\nwidth=2560\r\nheight=1440\r\nmode=borderless\r\n[tail]\r\nvalue=also preserved\r\n") == std::string::npos ||
        !sgl::display_config(GetDesktopWindow(), path, config, error) || config.size.width != 2560 ||
        config.size.height != 1440 || config.mode != sgl::DisplayMode::Borderless) return 14;
    const HANDLE keep_destination_open = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (keep_destination_open == INVALID_HANDLE_VALUE) return 19;
    const bool replaced_open_file = sgl::save_display_config(path, {1920, 1080}, sgl::DisplayMode::Windowed, error);
    const auto unchanged = read();
    CloseHandle(keep_destination_open);
    if (replaced_open_file || error.empty() || unchanged != saved) return 20;
    if (sgl::save_display_config(path, {1920, 1200}, sgl::DisplayMode::Windowed, error) || error.empty() || read() != saved) return 15;
    if (sgl::save_display_config(path, {1024, 768}, sgl::DisplayMode::Windowed, error) || error.empty() || read() != saved) return 17;
    if (sgl::save_display_config(path, {800, 600}, sgl::DisplayMode::Windowed, error) || error.empty() || read() != saved) return 18;
    if (sgl::save_display_config(path, {1920, 1080}, static_cast<sgl::DisplayMode>(99), error) ||
        error.empty() || read() != saved) return 21;
    auto missing_parent = path;
    missing_parent += L".missing-parent\\high_res.ini";
    if (sgl::save_display_config(missing_parent, {1920, 1080}, sgl::DisplayMode::Windowed, error) || error.empty()) return 16;
    if (!write("[display]\nwidth=1920\nheight=1080\n")) return 4;
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 8, 8,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 5;
    MONITORINFO output_monitor = {};
    output_monitor.cbSize = sizeof(output_monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &output_monitor)) return 6;
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, nullptr))) return 7;
        sgl::Presentation presentation;
        if (FAILED(presentation.initialize_configured(device.Get(), window, path, error))) return 8;
        const auto render_size = presentation.render_size();
        const auto output_size = presentation.output_size();
        D3D11_TEXTURE2D_DESC desc = {};
        presentation.back_buffer()->GetDesc(&desc);
        if (render_size.width != 1920 || render_size.height != 1080 ||
            output_size.width != static_cast<UINT>(output_monitor.rcMonitor.right - output_monitor.rcMonitor.left) ||
            output_size.height != static_cast<UINT>(output_monitor.rcMonitor.bottom - output_monitor.rcMonitor.top) ||
            desc.Width != output_size.width || desc.Height != output_size.height) return 9;
    }
    const LONG_PTR prior_style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!write("[display]\nwidth=1280\nheight=1024\nmode=windowed\n")) return 22;
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, nullptr))) return 23;
        sgl::Presentation presentation;
        if (FAILED(presentation.initialize_configured(device.Get(), window, path, error)) ||
            presentation.display_mode() != sgl::DisplayMode::Windowed) return 24;
        RECT client = {};
        if (!GetClientRect(window, &client) || client.right - client.left != 1280 ||
            client.bottom - client.top != 1024 || (GetWindowLongPtrW(window, GWL_STYLE) & WS_POPUP) ||
            !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) || presentation.output_size().width != 1280 ||
            presentation.output_size().height != 1024) return 25;
        presentation.close();
    }
    if (GetWindowLongPtrW(window, GWL_STYLE) != prior_style) return 26;
    DestroyWindow(window);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) return 9;
    if (sgl::display_config(nullptr, path, config, error) || error.empty()) return 12;
    MONITORINFOEXW monitor = {};
    monitor.cbSize = sizeof(monitor);
    const HMONITOR handle = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!handle || !GetMonitorInfoW(handle, &monitor) || !EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 10;
    const bool native_supported = (mode.dmPelsWidth == 1280 && mode.dmPelsHeight == 1024) ||
        (mode.dmPelsWidth == 1600 && mode.dmPelsHeight == 900) ||
        (mode.dmPelsWidth == 1920 && mode.dmPelsHeight == 1080) || (mode.dmPelsWidth == 2560 && mode.dmPelsHeight == 1440);
    const bool selected = sgl::display_config(GetDesktopWindow(), path, config, error);
    if (selected != native_supported || (selected && (config.size.width != mode.dmPelsWidth || config.size.height != mode.dmPelsHeight ||
        config.mode != sgl::DisplayMode::Borderless)) ||
        (!selected && error.empty())) return 11;
    return 0;
}
