#pragma once

#include "compositor.hpp"
#include "display_config.hpp"
#include "fps_overlay.hpp"
#include <dxgi.h>

namespace sgl {

// Client size for a windowed render size: the render size when it fits, otherwise the largest
// aspect-correct size within the available client area. Returns {0, 0} when nothing fits.
SIZE fit_windowed_client(UINT render_width, UINT render_height, LONG available_width, LONG available_height) noexcept;

class Presentation {
public:
    Presentation() = default;
    ~Presentation() noexcept;
    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    HRESULT initialize(ID3D11Device* device, HWND window, UINT width, UINT height,
        DisplayMode mode = DisplayMode::Borderless) noexcept;
    // game_size set: the game owns its layout; any valid render size is accepted (see display_config).
    HRESULT initialize_configured(ID3D11Device* device, HWND window, const std::filesystem::path& path, std::string& error,
        std::optional<DisplaySize> game_size = std::nullopt);
    HRESULT set_render_size(UINT width, UINT height) noexcept;
    void close() noexcept;
    HRESULT draw(ID3D11DeviceContext* context, Compositor& compositor, ID3D11ShaderResourceView* source,
        Rect source_rect, Rect render_rect) noexcept;
    HRESULT present(const FrameCounters& counters = {}) noexcept;
    ID3D11Texture2D* back_buffer() const noexcept { return back_buffer_.Get(); }
    DisplaySize render_size() const noexcept { return {render_width_, render_height_}; }
    DisplaySize output_size() const noexcept { return {output_width_, output_height_}; }
    DisplayMode display_mode() const noexcept { return mode_; }
    std::optional<unsigned> max_fps() const noexcept { return max_fps_; }
    void set_show_fps(bool show) noexcept { show_fps_ = show; fps_overlay_.reset(); }
    bool show_fps() const noexcept { return show_fps_; }
    bool exact_blend() const noexcept { return exact_blend_; }
    const FpsOverlay& fps_overlay() const noexcept { return fps_overlay_; }

private:
    static LRESULT CALLBACK window_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR reference_data) noexcept;
    HRESULT create_back_buffer() noexcept;
    HRESULT sync_windowed_buffers() noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    UINT render_width_ = 0;
    UINT render_height_ = 0;
    UINT output_width_ = 0;
    UINT output_height_ = 0;
    HWND window_ = nullptr;
    RECT output_rect_{};
    LONG_PTR original_style_ = 0;
    LONG_PTR original_extended_style_ = 0;
    RECT original_window_rect_{};
    DisplayMode mode_ = DisplayMode::Borderless;
    bool window_subclassed_ = false;
    bool original_window_state_saved_ = false;
    bool window_change_in_progress_ = false;
    int revealed_cursor_count_ = 0;
    std::optional<unsigned> max_fps_;
    UINT sync_interval_ = 1;
    bool show_fps_ = false;
    bool exact_blend_ = true;
    bool any_render_size_ = false;
    FpsOverlay fps_overlay_;
};

}
