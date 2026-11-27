#include "display_config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

bool equal_ascii(std::string_view value, std::string_view expected) {
    if (value.size() != expected.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c != expected[i]) return false;
    }
    return true;
}

// Its address identifies the module this file is linked into.
const char module_marker = 0;

}

std::filesystem::path sgl::display_config_path() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_marker), &module)) return {};
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    return std::filesystem::path(path.data()).parent_path() / L"high_res.ini";
}

bool sgl::is_supported_display_size(DisplaySize size) noexcept {
    return (size.width == 1280 && size.height == 1024) || (size.width == 1600 && size.height == 900) ||
        (size.width == 1920 && size.height == 1080) ||
        (size.width == 2560 && size.height == 1440);
}

bool sgl::is_valid_display_size(DisplaySize size) noexcept {
    return size.width >= 640 && size.height >= 480 && size.width <= 8192 && size.height <= 8192;
}

bool sgl::display_size_from_game(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return !ec;
    // An unreadable file or a malformed value counts as a set size, so display_config reports it.
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bool in_display = false;
    std::optional<unsigned> width, height;
    std::string line;
    while (std::getline(file, line)) {
        const auto text = trim(line);
        if (text.empty() || text.front() == ';' || text.front() == '#') continue;
        if (text.front() == '[' && text.back() == ']') {
            in_display = equal_ascii(text, "[display]");
            continue;
        }
        if (!in_display) continue;
        const auto equal = text.find('=');
        if (equal == std::string_view::npos) continue;
        const auto key = trim(text.substr(0, equal));
        auto* const target = equal_ascii(key, "width") ? &width : equal_ascii(key, "height") ? &height : nullptr;
        if (!target) continue;
        const auto value = trim(text.substr(equal + 1));
        unsigned number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        *target = !value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? number : 1u;
    }
    return !width || !height || !*width || !*height;
}

bool sgl::display_config(HWND window, const std::filesystem::path& path, DisplayConfig& config, std::string& error,
    std::optional<DisplaySize> game_size) {
    error.clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) { error = "Cannot inspect high_res.ini: " + ec.message(); return false; }
    DisplayConfig selected{};
    const auto desktop_size = [&]() {
        MONITORINFOEXW monitor = {};
        monitor.cbSize = sizeof(monitor);
        const HMONITOR handle = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (!window || !IsWindow(window) || !handle || !GetMonitorInfoW(handle, &monitor) ||
            !EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            error = "Cannot determine the window's display resolution";
            return false;
        }
        selected.size = {mode.dmPelsWidth, mode.dmPelsHeight};
        return true;
    };
    const bool any_size = game_size.has_value();
    const bool use_game_size = any_size && is_valid_display_size(*game_size);
    if (!exists) {
        if (use_game_size) selected.size = *game_size;
        else if (!desktop_size()) return false;
    } else {
        std::ifstream file(path, std::ios::binary);
        if (!file) { error = "Cannot read high_res.ini"; return false; }
        bool in_display = false, has_width = false, has_height = false;
        std::string line;
        unsigned line_number = 0;
        while (std::getline(file, line)) {
            ++line_number;
            auto text = trim(line);
            if (text.empty() || text.front() == ';' || text.front() == '#') continue;
            if (text.front() == '[' && text.back() == ']') {
                in_display = equal_ascii(text, "[display]");
                continue;
            }
            if (!in_display) continue;
            const auto equal = text.find('=');
            if (equal == std::string_view::npos) { error = "high_res.ini line " + std::to_string(line_number) + ": expected key=value"; return false; }
            const auto key = trim(text.substr(0, equal));
            const auto value = trim(text.substr(equal + 1));
            // Unknown keys are skipped and a repeated key takes its last value, so the file can be shared with
            // other settings.
            unsigned* number = nullptr;
            bool* present = nullptr;
            if (equal_ascii(key, "width")) { number = &selected.size.width; present = &has_width; }
            else if (equal_ascii(key, "height")) { number = &selected.size.height; present = &has_height; }
            else if (equal_ascii(key, "mode")) {
                if (equal_ascii(value, "windowed")) selected.mode = DisplayMode::Windowed;
                else if (equal_ascii(value, "borderless")) selected.mode = DisplayMode::Borderless;
                else { error = "high_res.ini line " + std::to_string(line_number) + ": mode must be windowed or borderless"; return false; }
                continue;
            } else if (equal_ascii(key, "show_fps")) {
                if (value != "0" && value != "1") { error = "high_res.ini line " + std::to_string(line_number) + ": show_fps must be 0 or 1"; return false; }
                selected.show_fps = value == "1";
                continue;
            } else if (equal_ascii(key, "vsync")) {
                if (value != "0" && value != "1") { error = "high_res.ini line " + std::to_string(line_number) + ": vsync must be 0 or 1"; return false; }
                selected.vsync = value == "1";
                continue;
            } else if (equal_ascii(key, "exact_blend")) {
                if (value != "0" && value != "1") { error = "high_res.ini line " + std::to_string(line_number) + ": exact_blend must be 0 or 1"; return false; }
                selected.exact_blend = value == "1";
                continue;
            } else if (equal_ascii(key, "max_fps")) {
                unsigned fps = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), fps);
                if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    (fps != 0 && (fps < 10 || fps > 1000))) {
                    error = "high_res.ini line " + std::to_string(line_number) + ": max_fps must be 0 or 10 to 1000";
                    return false;
                }
                selected.max_fps = fps;
                continue;
            } else continue;
            *present = true;
            if (value.empty()) {
                error = "high_res.ini line " + std::to_string(line_number) + ": expected a positive integer";
                return false;
            }
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), *number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                error = "high_res.ini line " + std::to_string(line_number) + ": expected a positive integer";
                return false;
            }
        }
        if (file.bad()) { error = "Cannot read high_res.ini"; return false; }
        if (use_game_size) {
            selected.size = *game_size;
        } else if (any_size) {
            if (!desktop_size()) return false;
        } else if (!has_width || !has_height) {
            error = "high_res.ini requires [display] width and height";
            return false;
        }
    }
    if (any_size ? !is_valid_display_size(selected.size) : !is_supported_display_size(selected.size)) {
        error = "Unsupported display resolution " + std::to_string(selected.size.width) + "x" + std::to_string(selected.size.height);
        return false;
    }
    config = selected;
    return true;
}

bool sgl::save_display_config(const std::filesystem::path& path, DisplaySize size, DisplayMode mode, std::string& error) {
    error.clear();
    if (path.empty() || !is_supported_display_size(size) ||
        (mode != DisplayMode::Windowed && mode != DisplayMode::Borderless)) {
        error = "Cannot save unsupported display configuration";
        return false;
    }

    std::filesystem::path temporary;
    HANDLE output = INVALID_HANDLE_VALUE;
    try {
        std::vector<std::string> lines;
        std::error_code filesystem_error;
        if (std::filesystem::exists(path, filesystem_error)) {
            if (filesystem_error) {
                error = "Cannot inspect high_res.ini: " + filesystem_error.message();
                return false;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                error = "Cannot read high_res.ini before saving";
                return false;
            }
            std::string line;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            }
            if (input.bad()) {
                error = "Cannot read high_res.ini before saving";
                return false;
            }
        } else if (filesystem_error) {
            error = "Cannot inspect high_res.ini: " + filesystem_error.message();
            return false;
        }

        const auto width_line = "width=" + std::to_string(size.width);
        const auto height_line = "height=" + std::to_string(size.height);
        const auto mode_line = std::string("mode=") + (mode == DisplayMode::Windowed ? "windowed" : "borderless");
        std::size_t section_end = lines.size();
        bool found_display = false;
        bool found_width = false;
        bool found_height = false;
        bool found_mode = false;
        bool in_display = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto text = trim(lines[i]);
            if (!text.empty() && text.front() == '[' && text.back() == ']') {
                if (in_display) {
                    section_end = i;
                    break;
                }
                in_display = equal_ascii(text, "[display]");
                if (in_display) {
                    found_display = true;
                }
                continue;
            }
            if (!in_display) continue;
            const auto equal = text.find('=');
            if (equal == std::string_view::npos) continue;
            const auto key = trim(text.substr(0, equal));
            if (equal_ascii(key, "width")) {
                lines[i] = width_line;
                found_width = true;
            } else if (equal_ascii(key, "height")) {
                lines[i] = height_line;
                found_height = true;
            } else if (equal_ascii(key, "mode")) {
                lines[i] = mode_line;
                found_mode = true;
            }
        }
        if (found_display) {
            auto position = lines.begin() + static_cast<std::ptrdiff_t>(section_end);
            if (!found_width) position = lines.insert(position, width_line) + 1;
            if (!found_height) position = lines.insert(position, height_line) + 1;
            if (!found_mode) lines.insert(position, mode_line);
        } else {
            if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
            lines.emplace_back("[display]");
            lines.push_back(width_line);
            lines.push_back(height_line);
            lines.push_back(mode_line);
        }

        std::string contents;
        for (const auto& line : lines) {
            contents += line;
            contents += "\r\n";
        }

        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            temporary = path;
            temporary += ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(attempt);
            output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (output != INVALID_HANDLE_VALUE) break;
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
                error = "Cannot create temporary high_res.ini: " +
                    std::system_category().message(static_cast<int>(code));
                return false;
            }
        }
        if (output == INVALID_HANDLE_VALUE) {
            error = "Cannot create a unique temporary high_res.ini";
            return false;
        }

        std::size_t offset = 0;
        while (offset < contents.size()) {
            const auto remaining = contents.size() - offset;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(output, contents.data() + offset, chunk, &written, nullptr) || written != chunk) {
                const DWORD code = GetLastError();
                CloseHandle(output);
                output = INVALID_HANDLE_VALUE;
                DeleteFileW(temporary.c_str());
                error = "Cannot write temporary high_res.ini: " + std::system_category().message(static_cast<int>(code));
                return false;
            }
            offset += written;
        }
        if (!FlushFileBuffers(output)) {
            const DWORD code = GetLastError();
            CloseHandle(output);
            output = INVALID_HANDLE_VALUE;
            DeleteFileW(temporary.c_str());
            error = "Cannot flush temporary high_res.ini: " + std::system_category().message(static_cast<int>(code));
            return false;
        }
        CloseHandle(output);
        output = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = GetLastError();
            DeleteFileW(temporary.c_str());
            error = "Cannot atomically replace high_res.ini: " + std::system_category().message(static_cast<int>(code));
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        try { error = std::string("Cannot save high_res.ini: ") + exception.what(); }
        catch (...) { }
        return false;
    } catch (...) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        error = "Cannot save high_res.ini";
        return false;
    }
}
