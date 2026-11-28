#pragma once

#include "compositor.hpp"
#include "image_decoder.hpp"
#include "presentation.hpp"
#include <unordered_map>
#include <vector>

namespace sgl {

class SurfaceRenderer {
public:
    HRESULT initialize() noexcept;
    HRESULT draw(void* source_id, const DecodedImage& source, void* target_id,
        DecodedImage& target, Rect source_rect, Rect target_rect, RECT clip,
        std::optional<std::uint32_t> key, BlendMode blend, const char** failure_stage = nullptr,
        std::uint32_t modulation_color = 0xffffffff, Sampling sampling = Sampling::SglFixedPoint) noexcept;
    HRESULT draw_masked(void* source_id, const DecodedImage& source, void* mask_id,
        const DecodedImage& mask, void* target_id, DecodedImage& target, Rect source_rect,
        Rect target_rect, RECT clip, std::uint32_t modulation_color = 0xffffffff,
        const char** failure_stage = nullptr) noexcept;
    // Opaque copy of a region of a surface onto the same surface, as from its pixels before the call (regions may
    // overlap), without a CPU readback.
    HRESULT copy_within(void* id, DecodedImage& image, Rect source_rect, Rect target_rect, RECT clip,
        const char** failure_stage = nullptr) noexcept;
    HRESULT synchronize_to_cpu(void* id, DecodedImage& image) noexcept;
    // Partial transfers for CPU work confined to a known region of a GPU-current BGRA surface: the readback copies
    // the region's GPU-written pixels to the CPU; the upload copies the region's CPU pixels to the GPU. Neither
    // changes which copy is authoritative for the rest of the surface.
    HRESULT synchronize_region_to_cpu(void* id, DecodedImage& image, RECT region) noexcept;
    HRESULT upload_region(void* id, const DecodedImage& image, RECT region) noexcept;
    // Applies the city water effect (Compositor::WaterDraw) to distinct tiles of a BGRA surface on the GPU. Like any
    // draw, this leaves the CPU pixels of the tiles stale.
    HRESULT draw_water(void* id, const DecodedImage& image, const RECT* tiles, UINT tile_count, RECT area,
        const std::uint8_t* heights, UINT height_pitch, UINT limit_width, UINT limit_height) noexcept;
    void mark_cpu_modified(void* id) noexcept;
    // True when the GPU copy of a surface is newer than its CPU pixels, so CPU access would need a readback.
    bool gpu_holds_newer_pixels(void* id) const noexcept;
    void mark_palette_modified(void* id) noexcept;
    void forget(void* id) noexcept;
    HRESULT open_screen(HWND window, const std::filesystem::path& config, std::string& error);
    HRESULT set_screen_size(DisplaySize size) noexcept;
    void close_screen() noexcept;
    void shutdown() noexcept;
    DisplaySize screen_size() const noexcept;
    DisplaySize output_size() const noexcept;
    DisplayMode display_mode() const noexcept;
    std::optional<unsigned> max_fps() const noexcept;
    HRESULT present(void* id, const DecodedImage& image) noexcept;

private:
    struct Surface {
        D3D11_TEXTURE2D_DESC description = {};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> background;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> background_view;
        // Source pixels of copy_within. Separate from background: the background copies of queued draws are recorded
        // when the queue is flushed and would overwrite a snapshot that a queued self-copy still reads.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshot;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> snapshot_view;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> palette;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> palette_view;
        bool cpu_needs_upload = true;
        bool cpu_needs_readback = false;
        bool palette_needs_upload = true;
        // Bounds of the GPU writes not yet copied to the CPU pixels; readback copies only this region.
        RECT gpu_dirty{};
    };
    // A queued draw. Draws to one target are queued in layers: a draw goes one layer above the highest queued draw it
    // may overlap, so the draws of a layer write disjoint pixels. Overlapping draws keep their order, and every pixel
    // ends up as if the draws had been recorded one at a time.
    struct PendingDraw {
        Compositor::PreparedDraw draw;
        ID3D11Texture2D* texture = nullptr;
        ID3D11Texture2D* background = nullptr;
        D3D11_BOX copy{};
        RECT written{};
        std::uint32_t layer = 0;
    };
    // Records the queued draws layer by layer: first the background copies of the layer's blended draws, then its draws,
    // grouped into as few calls as possible. A copy taken at the start of a layer equals one taken right before its
    // draw because every earlier draw that writes the copied pixels lies in a lower layer. Must run before anything
    // else reads or writes the queued resources.
    void flush_pending() noexcept;
    // Places a draw that writes the given pixels of the queued target in its layer.
    HRESULT assign_layer(const Surface& target, const RECT& written, std::uint32_t& layer) noexcept;
    HRESULT surface(void* id, const DecodedImage& image, Surface*& result) noexcept;
    HRESULT upload_cpu_image(Surface& surface, const DecodedImage& image) noexcept;
    // Creates, once, a shader-readable texture of the surface's size and format for copies of its pixels.
    HRESULT create_copy_texture(const Surface& surface, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view) noexcept;
    HRESULT draw_impl(void* source_id, const DecodedImage& source, void* mask_id,
        const DecodedImage* mask, void* target_id, DecodedImage& target, Rect source_rect,
        Rect target_rect, RECT clip, std::optional<std::uint32_t> key, BlendMode blend,
        const char** failure_stage, std::uint32_t modulation_color, Sampling sampling, bool self_copy = false) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Compositor compositor_;
    Presentation presentation_;
    std::unordered_map<void*, Surface> surfaces_;
    FrameCounters counters_{};
    ID3D11Resource* bound_target_ = nullptr;
    std::vector<PendingDraw> pending_;
    // The queued draws handed to the compositor; reserved with pending_, so filling it does not allocate.
    std::vector<const Compositor::PreparedDraw*> pending_draws_;
    std::vector<std::uint32_t> pending_order_;
    std::vector<std::uint32_t> layer_starts_;
    ID3D11Texture2D* pending_target_ = nullptr;
    // A 16x16-pixel cell of the queued target: one more than the highest layer of a queued draw touching it, and the
    // first queued draws touching it (count above capacity: more than listed).
    struct LayerCell {
        static constexpr std::uint16_t capacity = 6;
        std::uint32_t highest = 0;
        std::uint16_t count = 0;
        std::uint16_t draws[capacity] = {};
    };
    // The cells of the queued target and the cells in use.
    std::vector<LayerCell> layer_grid_;
    std::vector<std::uint32_t> layer_cells_;
    UINT layer_grid_columns_ = 0;
    UINT layer_grid_rows_ = 0;
};

}
