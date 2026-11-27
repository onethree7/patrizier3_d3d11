#include "presentation.hpp"
#include "screen_transform.hpp"

#include <algorithm>
#include <commctrl.h>

using Microsoft::WRL::ComPtr;

namespace {

HRESULT size_window_to_monitor(HWND window, RECT& output_rect) noexcept {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!monitor) return E_FAIL;
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT previous_rect = {};
    if (!GetMonitorInfoW(monitor, &info)) return HRESULT_FROM_WIN32(GetLastError());
    if (!GetWindowRect(window, &previous_rect)) return HRESULT_FROM_WIN32(GetLastError());

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous_style = GetWindowLongPtrW(window, GWL_STYLE);
    DWORD error = GetLastError();
    if (!previous_style && error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    if (previous_style & WS_CHILD) return E_INVALIDARG;

    const LONG_PTR borderless_style = (previous_style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR replaced_style = SetWindowLongPtrW(window, GWL_STYLE, borderless_style);
    error = GetLastError();
    if (!replaced_style && error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);

    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    if (!SetWindowPos(window, nullptr, info.rcMonitor.left, info.rcMonitor.top, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED)) {
        error = GetLastError();
        SetWindowLongPtrW(window, GWL_STYLE, previous_style);
        SetWindowPos(window, nullptr, previous_rect.left, previous_rect.top,
            previous_rect.right - previous_rect.left, previous_rect.bottom - previous_rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        return HRESULT_FROM_WIN32(error);
    }

    RECT client = {};
    SetLastError(ERROR_SUCCESS);
    const BOOL got_client = GetClientRect(window, &client);
    error = GetLastError();
    if (!got_client || client.right - client.left != width || client.bottom - client.top != height) {
        SetWindowLongPtrW(window, GWL_STYLE, previous_style);
        SetWindowPos(window, nullptr, previous_rect.left, previous_rect.top,
            previous_rect.right - previous_rect.left, previous_rect.bottom - previous_rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
    }
    output_rect = info.rcMonitor;
    return S_OK;
}

HRESULT get_window_value(HWND window, int index, LONG_PTR& value) noexcept {
    SetLastError(ERROR_SUCCESS);
    value = GetWindowLongPtrW(window, index);
    const DWORD error = GetLastError();
    return value || error == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(error);
}

void restore_window(HWND window, LONG_PTR style, LONG_PTR extended_style, const RECT& rect) noexcept {
    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, extended_style);
    SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

// Window frame bits owned by windowed presentation. The game strips caption and frame bits on every mode
// change; only these bits are restored, so system state such as WS_MAXIMIZE or WS_VISIBLE is untouched.
constexpr LONG_PTR windowed_frame_bits = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
    WS_MAXIMIZEBOX | WS_POPUP;
constexpr LONG_PTR windowed_frame_style = WS_OVERLAPPEDWINDOW;
constexpr LONG minimum_windowed_client_width = 320;
constexpr LONG minimum_windowed_client_height = 240;

HRESULT window_frame_size(HWND window, LONG_PTR style, LONG_PTR extended_style, SIZE& frame) noexcept {
    RECT rect{0, 0, 0, 0};
    if (!AdjustWindowRectEx(&rect, static_cast<DWORD>(style), GetMenu(window) != nullptr,
        static_cast<DWORD>(extended_style))) {
        const DWORD error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    frame = {rect.right - rect.left, rect.bottom - rect.top};
    return S_OK;
}

}

namespace sgl {

SIZE fit_windowed_client(UINT render_width, UINT render_height, LONG available_width, LONG available_height) noexcept {
    if (!render_width || !render_height || available_width <= 0 || available_height <= 0) return {0, 0};
    if (render_width <= static_cast<UINT>(available_width) && render_height <= static_cast<UINT>(available_height))
        return {static_cast<LONG>(render_width), static_cast<LONG>(render_height)};
    const auto width = static_cast<long long>(render_width), height = static_cast<long long>(render_height);
    if (static_cast<long long>(available_width) * height <= static_cast<long long>(available_height) * width)
        return {available_width, std::max<LONG>(1, static_cast<LONG>(available_width * height / width))};
    return {std::max<LONG>(1, static_cast<LONG>(available_height * width / height)), available_height};
}

}

namespace {

// Gives the window a normal resizable frame whose client shows the render size, or the largest
// aspect-correct size that fits the monitor work area. keep_position preserves the current top-left
// corner (clamped into the work area); otherwise the window is centered.
HRESULT place_windowed_window(HWND window, UINT render_width, UINT render_height, bool keep_position) noexcept {
    if (render_width > LONG_MAX || render_height > LONG_MAX) return E_INVALIDARG;
    LONG_PTR original_style = 0;
    LONG_PTR extended_style = 0;
    HRESULT hr = get_window_value(window, GWL_STYLE, original_style);
    if (FAILED(hr)) return hr;
    hr = get_window_value(window, GWL_EXSTYLE, extended_style);
    if (FAILED(hr)) return hr;
    if (original_style & WS_CHILD) return E_INVALIDARG;
    RECT prior = {};
    if (!GetWindowRect(window, &prior)) return HRESULT_FROM_WIN32(GetLastError());

    const LONG_PTR style = (original_style & ~windowed_frame_bits & ~static_cast<LONG_PTR>(WS_MAXIMIZE | WS_MINIMIZE)) |
        windowed_frame_style;
    SIZE frame = {};
    hr = window_frame_size(window, style, extended_style, frame);
    if (FAILED(hr)) return hr;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!monitor) return E_FAIL;
    if (!GetMonitorInfoW(monitor, &info)) return HRESULT_FROM_WIN32(GetLastError());
    const LONG work_width = info.rcWork.right - info.rcWork.left;
    const LONG work_height = info.rcWork.bottom - info.rcWork.top;
    const SIZE client = sgl::fit_windowed_client(render_width, render_height, work_width - frame.cx, work_height - frame.cy);
    if (!client.cx || !client.cy) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    const LONG window_width = client.cx + frame.cx;
    const LONG window_height = client.cy + frame.cy;
    LONG left = info.rcWork.left + (work_width - window_width) / 2;
    LONG top = info.rcWork.top + (work_height - window_height) / 2;
    if (keep_position) {
        left = std::clamp(prior.left, info.rcWork.left, info.rcWork.right - window_width);
        top = std::clamp(prior.top, info.rcWork.top, info.rcWork.bottom - window_height);
    }

    const auto rollback = [&]() noexcept {
        restore_window(window, original_style, extended_style, prior);
    };
    if (style != original_style) {
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR replaced = SetWindowLongPtrW(window, GWL_STYLE, style);
        const DWORD error = GetLastError();
        if (!replaced && error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    }
    if (!SetWindowPos(window, HWND_NOTOPMOST, left, top, window_width, window_height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED)) {
        const DWORD error = GetLastError();
        rollback();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    RECT actual = {};
    if (!GetClientRect(window, &actual)) {
        const DWORD error = GetLastError();
        rollback();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    if (actual.right - actual.left != client.cx || actual.bottom - actual.top != client.cy) {
        rollback();
        return E_FAIL;
    }
    return S_OK;
}
}

namespace sgl {

HRESULT Presentation::initialize_configured(ID3D11Device* device, HWND window, const std::filesystem::path& path, std::string& error,
    std::optional<DisplaySize> game_size) {
    DisplayConfig config{};
    if (!display_config(window, path, config, error, game_size)) return E_INVALIDARG;
    const HRESULT hr = initialize(device, window, config.size.width, config.size.height, config.mode);
    if (SUCCEEDED(hr)) {
        max_fps_ = config.max_fps;
        sync_interval_ = config.vsync ? 1 : 0;
        set_show_fps(config.show_fps);
        exact_blend_ = config.exact_blend;
        any_render_size_ = game_size.has_value();
    }
    if (FAILED(hr)) {
        error = hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ?
            "The selected windowed resolution does not fit within the monitor work area; choose a lower resolution or borderless mode." :
            "Cannot initialize D3D11 presentation";
    }
    return hr;
}

Presentation::~Presentation() noexcept { close(); }

LRESULT CALLBACK Presentation::window_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR reference_data) noexcept {
    auto* presentation = reinterpret_cast<Presentation*>(reference_data);
    if (!presentation || window != presentation->window_ || presentation->window_change_in_progress_)
        return DefSubclassProc(window, message, wparam, lparam);

    // Both modes: the game requests HWND_TOPMOST with its fullscreen placement; the window stays a normal window that
    // others can cover.
    if (message == WM_WINDOWPOSCHANGED && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        presentation->window_change_in_progress_ = true;
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        presentation->window_change_in_progress_ = false;
    }

    if (presentation->mode_ == DisplayMode::Windowed) {
        constexpr WPARAM mouse_buttons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
        switch (message) {
        case WM_STYLECHANGING:
            // The game strips the caption and frame on every mode change.
            if (wparam == static_cast<WPARAM>(GWL_STYLE) && lparam) {
                auto* style = reinterpret_cast<STYLESTRUCT*>(lparam);
                style->styleNew = static_cast<DWORD>((static_cast<LONG_PTR>(style->styleNew) & ~windowed_frame_bits) |
                    windowed_frame_style);
            }
            break;
        case WM_GETMINMAXINFO:
            if (lparam) {
                SIZE frame = {};
                if (SUCCEEDED(window_frame_size(window, GetWindowLongPtrW(window, GWL_STYLE),
                    GetWindowLongPtrW(window, GWL_EXSTYLE), frame))) {
                    auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                    limits->ptMinTrackSize.x = minimum_windowed_client_width + frame.cx;
                    limits->ptMinTrackSize.y = minimum_windowed_client_height + frame.cy;
                }
            }
            break;
        case WM_SETCURSOR:
            // The game hides the system cursor and draws its own; show it over the frame and caption.
            if (LOWORD(lparam) == HTCLIENT) {
                for (; presentation->revealed_cursor_count_; --presentation->revealed_cursor_count_) ShowCursor(FALSE);
            } else if (!presentation->revealed_cursor_count_) {
                do ++presentation->revealed_cursor_count_; while (ShowCursor(TRUE) < 0);
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDBLCLK:
            // Capture only while a button is held, so drags may leave the client and the frame stays usable.
            SetCapture(window);
            break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            if (!(wparam & mouse_buttons) && GetCapture() == window) ReleaseCapture();
            break;
        default:
            break;
        }
    } else if (presentation->mode_ == DisplayMode::Borderless && message == WM_ACTIVATEAPP && !wparam) {
        // The game keeps its own capture while it is the foreground window (its menu lists need it); another
        // application taking the foreground gets the mouse back. The game recaptures only once it is in front again
        // (game_window_binding).
        if (GetCapture() == window) ReleaseCapture();
    } else if (presentation->mode_ == DisplayMode::Borderless &&
        message == WM_WINDOWPOSCHANGING && lparam) {
        auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
        if (!(position->flags & SWP_NOMOVE)) {
            position->x = presentation->output_rect_.left;
            position->y = presentation->output_rect_.top;
        }
        if (!(position->flags & SWP_NOSIZE)) {
            position->cx = presentation->output_rect_.right - presentation->output_rect_.left;
            position->cy = presentation->output_rect_.bottom - presentation->output_rect_.top;
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void Presentation::close() noexcept {
    if (window_subclassed_ && window_ && IsWindow(window_)) {
        RemoveWindowSubclass(window_, window_subclass_proc, reinterpret_cast<UINT_PTR>(this));
    }
    if (mode_ == DisplayMode::Windowed && original_window_state_saved_ && window_ && IsWindow(window_))
        restore_window(window_, original_style_, original_extended_style_, original_window_rect_);
    for (; revealed_cursor_count_; --revealed_cursor_count_) ShowCursor(FALSE);
    window_subclassed_ = false;
    original_window_state_saved_ = false;
    window_ = nullptr;
    output_rect_ = {};
    target_.Reset();
    back_buffer_.Reset();
    swap_chain_.Reset();
    device_.Reset();
    render_width_ = render_height_ = output_width_ = output_height_ = 0;
    original_style_ = original_extended_style_ = 0;
    original_window_rect_ = {};
    mode_ = DisplayMode::Borderless;
    window_change_in_progress_ = false;
    max_fps_.reset();
    sync_interval_ = 1;
    show_fps_ = false;
    exact_blend_ = true;
    any_render_size_ = false;
    fps_overlay_.reset();
}

HRESULT Presentation::initialize(ID3D11Device* device, HWND window, UINT width, UINT height, DisplayMode mode) noexcept {
    if (!device || !window || !IsWindow(window) || !width || !height || swap_chain_) return E_INVALIDARG;
    if (mode != DisplayMode::Windowed && mode != DisplayMode::Borderless) return E_INVALIDARG;
    LONG_PTR original_style = 0, original_extended_style = 0;
    HRESULT hr = get_window_value(window, GWL_STYLE, original_style);
    if (FAILED(hr)) return hr;
    hr = get_window_value(window, GWL_EXSTYLE, original_extended_style);
    if (FAILED(hr)) return hr;
    RECT original_window_rect = {};
    if (!GetWindowRect(window, &original_window_rect)) return HRESULT_FROM_WIN32(GetLastError());

    RECT output_rect = {};
    if (mode == DisplayMode::Borderless) {
        hr = size_window_to_monitor(window, output_rect);
    } else {
        hr = place_windowed_window(window, width, height, false);
    }
    if (FAILED(hr)) {
        restore_window(window, original_style, original_extended_style, original_window_rect);
        return hr;
    }
    RECT client = {};
    if (!GetClientRect(window, &client)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        restore_window(window, original_style, original_extended_style, original_window_rect);
        return hr;
    }
    const LONG client_width = client.right - client.left;
    const LONG client_height = client.bottom - client.top;
    if (client_width <= 0 || client_height <= 0) {
        restore_window(window, original_style, original_extended_style, original_window_rect);
        return E_INVALIDARG;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) { restore_window(window, original_style, original_extended_style, original_window_rect); return hr; }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) { restore_window(window, original_style, original_extended_style, original_window_rect); return hr; }
    ComPtr<IDXGIFactory> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { restore_window(window, original_style, original_extended_style, original_window_rect); return hr; }
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = static_cast<UINT>(client_width);
    desc.BufferDesc.Height = static_cast<UINT>(client_height);
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> chain;
    hr = factory->CreateSwapChain(device, &desc, &chain);
    if (FAILED(hr)) { restore_window(window, original_style, original_extended_style, original_window_rect); return hr; }
    ComPtr<ID3D11Texture2D> back_buffer;
    hr = chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) { restore_window(window, original_style, original_extended_style, original_window_rect); return hr; }
    ComPtr<ID3D11RenderTargetView> target;
    hr = device->CreateRenderTargetView(back_buffer.Get(), nullptr, &target);
    if (FAILED(hr)) { restore_window(window, original_style, original_extended_style, original_window_rect); return hr; }
    window_ = window;
    output_rect_ = output_rect;
    if (!SetWindowSubclass(window_, window_subclass_proc, reinterpret_cast<UINT_PTR>(this),
        reinterpret_cast<DWORD_PTR>(this))) {
        window_ = nullptr;
        output_rect_ = {};
        restore_window(window, original_style, original_extended_style, original_window_rect);
        return E_FAIL;
    }
    window_subclassed_ = true;
    // The game may already have made the window topmost (see window_subclass_proc).
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST)
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    device_ = device;
    swap_chain_ = chain;
    back_buffer_ = back_buffer;
    target_ = target;
    render_width_ = width;
    render_height_ = height;
    output_width_ = static_cast<UINT>(client_width);
    output_height_ = static_cast<UINT>(client_height);
    original_style_ = original_style;
    original_extended_style_ = original_extended_style;
    original_window_rect_ = original_window_rect;
    original_window_state_saved_ = true;
    mode_ = mode;
    return S_OK;
}

HRESULT Presentation::set_render_size(UINT width, UINT height) noexcept {
    const DisplaySize size{width, height};
    if (!swap_chain_ || (any_render_size_ ? !is_valid_display_size(size) : !is_supported_display_size(size)))
        return E_INVALIDARG;
    if (mode_ == DisplayMode::Windowed) {
        // A user-maximized or minimized window keeps its state; the frame is scaled into it.
        if (!IsIconic(window_) && !IsZoomed(window_)) {
            window_change_in_progress_ = true;
            const HRESULT placed = place_windowed_window(window_, width, height, true);
            window_change_in_progress_ = false;
            if (FAILED(placed)) return placed;
        }
        const HRESULT synced = sync_windowed_buffers();
        if (FAILED(synced)) return synced;
    }
    render_width_ = width;
    render_height_ = height;
    return S_OK;
}

HRESULT Presentation::create_back_buffer() noexcept {
    if (!device_ || !swap_chain_) return E_UNEXPECTED;
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11RenderTargetView> target;
    hr = device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &target);
    if (FAILED(hr)) return hr;
    back_buffer_ = back_buffer;
    target_ = target;
    return S_OK;
}

// Matches the swap chain to the current client area; the user may move, resize or maximize the window.
HRESULT Presentation::sync_windowed_buffers() noexcept {
    if (mode_ != DisplayMode::Windowed || !window_ || !swap_chain_ || !device_) return E_UNEXPECTED;
    RECT client = {};
    if (!GetClientRect(window_, &client)) return HRESULT_FROM_WIN32(GetLastError());
    const UINT width = static_cast<UINT>(std::max<LONG>(0, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(0, client.bottom - client.top));
    if (!width || !height || (width == output_width_ && height == output_height_)) return S_OK;
    ComPtr<ID3D11DeviceContext> context;
    device_->GetImmediateContext(&context);
    if (!context) return E_FAIL;
    const UINT previous_width = output_width_;
    const UINT previous_height = output_height_;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    target_.Reset();
    back_buffer_.Reset();
    HRESULT hr = swap_chain_->ResizeBuffers(1, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (SUCCEEDED(hr)) hr = create_back_buffer();
    if (FAILED(hr)) {
        target_.Reset();
        back_buffer_.Reset();
        const HRESULT rollback = swap_chain_->ResizeBuffers(1, previous_width, previous_height,
            DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        const HRESULT restored = FAILED(rollback) ? rollback : create_back_buffer();
        return FAILED(restored) ? restored : hr;
    }
    output_width_ = width;
    output_height_ = height;
    return S_OK;
}

HRESULT Presentation::draw(ID3D11DeviceContext* context, Compositor& compositor, ID3D11ShaderResourceView* source,
    Rect source_rect, Rect render_rect) noexcept {
    if (!context || !source || !target_) return E_INVALIDARG;
    ComPtr<ID3D11Device> context_device;
    context->GetDevice(&context_device);
    if (context_device.Get() != device_.Get()) return E_INVALIDARG;
    if (mode_ == DisplayMode::Windowed) {
        if (IsIconic(window_)) return S_FALSE;
        const HRESULT synced = sync_windowed_buffers();
        if (FAILED(synced)) return synced;
    }
    ScreenTransform transform;
    if (!ScreenTransform::create(render_width_, render_height_, output_width_, output_height_, transform))
        return E_INVALIDARG;
    const float clear[] = {0, 0, 0, 1};
    context->ClearRenderTargetView(target_.Get(), clear);
    const HRESULT hr = compositor.copy(context, target_.Get(), source, output_width_, output_height_, source_rect,
        transform.map(render_rect),
        {0, 0, static_cast<LONG>(output_width_), static_cast<LONG>(output_height_)}, Sampling::PresentSharp);
    if (FAILED(hr) || !show_fps_) return hr;
    return fps_overlay_.draw(device_.Get(), context, compositor, target_.Get(), output_width_, output_height_);
}

HRESULT Presentation::present(const FrameCounters& counters) noexcept {
    if (!swap_chain_) return E_UNEXPECTED;
    const HRESULT hr = swap_chain_->Present(sync_interval_, 0);
    if (SUCCEEDED(hr) && show_fps_) fps_overlay_.frame_presented(counters);
    return hr;
}

}
