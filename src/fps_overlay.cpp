#include "fps_overlay.hpp"

#include "build_config.hpp"

#include <algorithm>
#include <cstdio>

namespace sgl {

namespace {

struct Glyph {
    char character;
    std::uint8_t rows[FpsOverlay::glyph_height];
};

// 5x7 glyphs, most significant of the five bits is the left column.
constexpr Glyph glyphs[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}}, {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'f', {0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08}}, {'p', {0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10}},
    {'s', {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e}}, {'m', {0x00, 0x00, 0x1a, 0x15, 0x15, 0x11, 0x11}},
    {'a', {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f}}, {'x', {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11}},
    {'d', {0x01, 0x01, 0x0d, 0x13, 0x11, 0x11, 0x0f}}, {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'b', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1e}}, {'u', {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d}},
};

const Glyph* find_glyph(char character) noexcept {
    for (const auto& glyph : glyphs) if (glyph.character == character) return &glyph;
    return nullptr;
}

long long now() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

}

void FpsOverlay::reset() noexcept {
    texture_.Reset();
    view_.Reset();
    text_ = "-- fps";
    pixels_dirty_ = true;
    previous_ = window_start_ = longest_ = 0;
    frames_ = 0;
    totals_ = {};
}

void FpsOverlay::frame_presented(const FrameCounters& counters) noexcept {
    if (!frequency_) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        frequency_ = frequency.QuadPart;
    }
    const long long current = now();
    if (previous_) {
        longest_ = std::max(longest_, current - previous_);
        ++frames_;
        totals_.draws += counters.draws;
        totals_.readbacks += counters.readbacks;
        totals_.uploads += counters.uploads;
        totals_.transfer_bytes += counters.transfer_bytes;
        totals_.readback_ticks += counters.readback_ticks;
        totals_.upload_ticks += counters.upload_ticks;
    } else {
        window_start_ = current;
    }
    previous_ = current;
    const long long elapsed = current - window_start_;
    if (frames_ && frequency_ && elapsed * 2 >= frequency_) {
        const double seconds = static_cast<double>(elapsed) / static_cast<double>(frequency_);
        update_text(frames_ / seconds, seconds * 1000.0 / frames_, longest_ * 1000.0 / static_cast<double>(frequency_));
        window_start_ = current;
        longest_ = 0;
        frames_ = 0;
        totals_ = {};
    }
}

void FpsOverlay::update_text(double fps, double average_ms, double longest_ms) noexcept {
    char buffer[max_characters + 1] = {};
    if constexpr (build::diagnostics) {
        const double frames = frames_ ? static_cast<double>(frames_) : 1.0;
        const double tick_ms = frequency_ ? 1000.0 / static_cast<double>(frequency_) / frames : 0.0;
        std::snprintf(buffer, sizeof(buffer), "%.0f fps %.1f ms max %.1f dr %.0f rb %.1f %.1fms up %.1f %.1fms mb %.1f", fps,
            average_ms, longest_ms, totals_.draws / frames, totals_.readbacks / frames, totals_.readback_ticks * tick_ms,
            totals_.uploads / frames, totals_.upload_ticks * tick_ms, static_cast<double>(totals_.transfer_bytes) / frames / 1048576.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f fps %.1f ms max %.1f", fps, average_ms, longest_ms);
    }
    text_ = buffer;
    pixels_dirty_ = true;
}

HRESULT FpsOverlay::draw(ID3D11Device* device, ID3D11DeviceContext* context, Compositor& compositor,
    ID3D11RenderTargetView* target, UINT target_width, UINT target_height) noexcept {
    if (!device || !context || !target || !target_width || !target_height) return E_INVALIDARG;
    if (!texture_) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = texture_width;
        desc.Height = texture_height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture_);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texture_.Get(), nullptr, &view_);
        if (FAILED(hr)) { texture_.Reset(); view_.Reset(); return hr; }
        pixels_dirty_ = true;
    }
    if (pixels_dirty_) {
        pixels_.fill(background);
        UINT x = padding;
        for (const char character : text_) {
            if (x + glyph_width > texture_width - padding) break;
            if (const Glyph* glyph = find_glyph(character)) {
                for (UINT row = 0; row < glyph_height; ++row)
                    for (UINT column = 0; column < glyph_width; ++column)
                        if (glyph->rows[row] & (0x10u >> column))
                            pixels_[(padding + row) * texture_width + x + column] = foreground;
            }
            x += glyph_width + 1;
        }
        text_width_ = std::min(texture_width, x - 1 + padding);
        context->UpdateSubresource(texture_.Get(), 0, nullptr, pixels_.data(), texture_width * 4, 0);
        pixels_dirty_ = false;
    }
    const UINT scale = std::max(2u, target_height / 540u);
    const float margin = static_cast<float>(4 * scale);
    const Rect destination = {margin, margin, margin + static_cast<float>(text_width_ * scale),
        margin + static_cast<float>(texture_height * scale)};
    return compositor.copy(context, target, view_.Get(), target_width, target_height,
        {0, 0, static_cast<float>(text_width_), static_cast<float>(texture_height)}, destination,
        {0, 0, static_cast<LONG>(target_width), static_cast<LONG>(target_height)});
}

}
