#include "surface_renderer.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
HRESULT as_bgra(const sgl::DecodedImage& image, std::vector<std::uint8_t>& converted,
    const std::uint8_t*& pixels, UINT& pitch) noexcept {
    pixels = image.pixels.data();
    pitch = image.pitch;
    if (image.format != sgl::ImageFormat::Bgr24) return S_OK;
    if (image.width > UINT_MAX / 4 ||
        image.pitch < static_cast<std::uint64_t>(image.width) * 3 ||
        static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size()) return E_INVALIDARG;
    const std::uint64_t converted_size = static_cast<std::uint64_t>(image.width) * image.height * 4;
    if (converted_size > SIZE_MAX) return E_OUTOFMEMORY;
    try {
        converted.resize(static_cast<std::size_t>(converted_size));
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (const std::length_error&) {
        return E_OUTOFMEMORY;
    }
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto* source_row = image.pixels.data() + static_cast<std::size_t>(y) * image.pitch;
        auto* target_row = converted.data() + static_cast<std::size_t>(y) * image.width * 4;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto* source_pixel = source_row + static_cast<std::size_t>(x) * 3;
            auto* target_pixel = target_row + static_cast<std::size_t>(x) * 4;
            target_pixel[0] = source_pixel[0];
            target_pixel[1] = source_pixel[1];
            target_pixel[2] = source_pixel[2];
            target_pixel[3] = 0xff;
        }
    }
    pixels = converted.data();
    pitch = image.width * 4;
    return S_OK;
}
}

namespace sgl {

HRESULT SurfaceRenderer::initialize() noexcept {
    if (device_) return S_OK;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(result)) return result;
    result = compositor_.initialize(device.Get());
    if (FAILED(result)) return result;
    device_ = device;
    context_ = context;
    return S_OK;
}

HRESULT SurfaceRenderer::surface(void* id, const DecodedImage& image, Surface*& result) noexcept {
    const auto found = surfaces_.find(id);
    if (found != surfaces_.end()) {
        const auto expected_format = image.format == ImageFormat::Indexed8 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (found->second.description.Width != image.width || found->second.description.Height != image.height ||
            found->second.description.Format != expected_format) return E_INVALIDARG;
        result = &found->second;
        return S_OK;
    }
    Surface next;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = image.format == ImageFormat::Indexed8 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (image.format != ImageFormat::Indexed8) desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &next.texture);
    if (FAILED(hr)) return hr;
    next.description = desc;
    hr = device_->CreateShaderResourceView(next.texture.Get(), nullptr, &next.view);
    if (FAILED(hr)) return hr;
    if (image.format == ImageFormat::Indexed8) {
        desc.Width = 256;
        desc.Height = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = device_->CreateTexture2D(&desc, nullptr, &next.palette);
        if (FAILED(hr)) return hr;
        hr = device_->CreateShaderResourceView(next.palette.Get(), nullptr, &next.palette_view);
    } else {
        hr = device_->CreateRenderTargetView(next.texture.Get(), nullptr, &next.target);
        if (FAILED(hr)) return hr;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device_->CreateTexture2D(&desc, nullptr, &next.staging);
    }
    if (FAILED(hr)) return hr;
    try {
        result = &surfaces_.emplace(id, std::move(next)).first->second;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    return S_OK;
}

namespace {
long long performance_counter() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
}

HRESULT SurfaceRenderer::upload_cpu_image(Surface& surface, const DecodedImage& image) noexcept {
    // Queued draws may read or write this texture; they were submitted before the upload.
    if (surface.cpu_needs_upload || (surface.palette && surface.palette_needs_upload && !image.palette.empty()))
        flush_pending();
    if (surface.cpu_needs_upload) {
        const long long started = performance_counter();
        std::vector<std::uint8_t> converted;
        const std::uint8_t* pixels = nullptr;
        UINT pitch = 0;
        const HRESULT hr = as_bgra(image, converted, pixels, pitch);
        if (FAILED(hr)) return hr;
        context_->UpdateSubresource(surface.texture.Get(), 0, nullptr, pixels, pitch, 0);
        ++counters_.uploads;
        counters_.transfer_bytes += static_cast<unsigned long long>(pitch) * image.height;
        counters_.upload_ticks += performance_counter() - started;
        surface.cpu_needs_upload = false;
        surface.cpu_needs_readback = false;
        surface.gpu_dirty = {};
    }
    if (surface.palette && surface.palette_needs_upload && !image.palette.empty()) {
        if (image.palette.size() != 1024) return E_INVALIDARG;
        context_->UpdateSubresource(surface.palette.Get(), 0, nullptr, image.palette.data(), 1024, 0);
        surface.palette_needs_upload = false;
    }
    return S_OK;
}

HRESULT SurfaceRenderer::create_copy_texture(const Surface& surface, ComPtr<ID3D11Texture2D>& texture,
    ComPtr<ID3D11ShaderResourceView>& view) noexcept {
    if (texture) return S_OK;
    D3D11_TEXTURE2D_DESC desc = surface.description;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> created;
    ComPtr<ID3D11ShaderResourceView> created_view;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &created);
    if (FAILED(hr)) return hr;
    hr = device_->CreateShaderResourceView(created.Get(), nullptr, &created_view);
    if (FAILED(hr)) return hr;
    texture = created;
    view = created_view;
    return S_OK;
}

HRESULT SurfaceRenderer::draw_water(void* id, const DecodedImage& image, const RECT* tiles, UINT tile_count, RECT area,
    const std::uint8_t* heights, UINT height_pitch, UINT limit_width, UINT limit_height) noexcept {
    if (!id || !tiles || !tile_count || !heights || !limit_width || !limit_height || image.format != ImageFormat::Bgra32 ||
        image.pixels.empty() || image.pitch < static_cast<std::uint64_t>(image.width) * 4 ||
        static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size() ||
        area.left < 0 || area.top < 0 || area.left >= area.right || area.top >= area.bottom ||
        area.right > static_cast<LONG>(image.width) || area.bottom > static_cast<LONG>(image.height)) return E_INVALIDARG;
    HRESULT hr = initialize();
    if (FAILED(hr)) return hr;
    Surface* dst = nullptr;
    hr = surface(id, image, dst);
    if (FAILED(hr)) return hr;
    if (!dst->target) return E_INVALIDARG;
    hr = create_copy_texture(*dst, dst->background, dst->background_view);
    if (FAILED(hr)) return hr;
    // Queued draws, which may write or read this surface, come before the effect.
    flush_pending();
    hr = upload_cpu_image(*dst, image);
    if (FAILED(hr)) return hr;
    // The effect reads up to 128 pixels away from a tile (see read_margin in the binding), clamped to the limit size.
    constexpr LONG margin = 128;
    const LONG read_right = std::min<LONG>(static_cast<LONG>(std::min(limit_width, image.width)), area.right + margin);
    const LONG read_bottom = std::min<LONG>(static_cast<LONG>(std::min(limit_height, image.height)), area.bottom + margin);
    const LONG read_left = std::min(std::max(area.left - margin, 0L), read_right - 1);
    const LONG read_top = std::min(std::max(area.top - margin, 0L), read_bottom - 1);
    if (read_left < 0 || read_top < 0 || read_left >= read_right || read_top >= read_bottom) return E_INVALIDARG;
    const D3D11_BOX read{static_cast<UINT>(read_left), static_cast<UINT>(read_top), 0,
        static_cast<UINT>(read_right), static_cast<UINT>(read_bottom), 1};
    context_->CopySubresourceRegion(dst->background.Get(), 0, read.left, read.top, 0, dst->texture.Get(), 0, &read);
    Compositor::WaterDraw draw;
    draw.target = dst->target.Get();
    draw.target_width = image.width;
    draw.target_height = image.height;
    draw.source = dst->background_view.Get();
    draw.tiles = tiles;
    draw.tile_count = tile_count;
    draw.area = area;
    draw.heights = heights;
    draw.height_pitch = height_pitch;
    draw.limit_width = std::min(limit_width, image.width);
    draw.limit_height = std::min(limit_height, image.height);
    hr = compositor_.draw_water(context_.Get(), draw);
    if (FAILED(hr)) return hr;
    bound_target_ = dst->texture.Get();
    ++counters_.draws;
    auto& dirty = dst->gpu_dirty;
    dirty = dirty.left >= dirty.right || dirty.top >= dirty.bottom ? area :
        RECT{std::min(dirty.left, area.left), std::min(dirty.top, area.top),
            std::max(dirty.right, area.right), std::max(dirty.bottom, area.bottom)};
    dst->cpu_needs_upload = false;
    dst->cpu_needs_readback = true;
    return S_OK;
}

HRESULT SurfaceRenderer::draw(void* source_id, const DecodedImage& source, void* target_id,
    DecodedImage& target, Rect source_rect, Rect target_rect, RECT clip, std::optional<std::uint32_t> key, BlendMode blend,
    const char** failure_stage, std::uint32_t modulation_color, Sampling sampling) noexcept {
    return draw_impl(source_id, source, nullptr, nullptr, target_id, target, source_rect,
        target_rect, clip, key, blend, failure_stage, modulation_color, sampling);
}

HRESULT SurfaceRenderer::draw_masked(void* source_id, const DecodedImage& source, void* mask_id,
    const DecodedImage& mask, void* target_id, DecodedImage& target, Rect source_rect,
    Rect target_rect, RECT clip, std::uint32_t modulation_color, const char** failure_stage) noexcept {
    return draw_impl(source_id, source, mask_id, &mask, target_id, target, source_rect,
        target_rect, clip, std::nullopt, BlendMode::AlphaMask, failure_stage, modulation_color, Sampling::SglFixedPoint);
}

HRESULT SurfaceRenderer::copy_within(void* id, DecodedImage& image, Rect source_rect, Rect target_rect, RECT clip,
    const char** failure_stage) noexcept {
    return draw_impl(id, image, nullptr, nullptr, id, image, source_rect, target_rect, clip, std::nullopt,
        BlendMode::Opaque, failure_stage, 0xffffffff, Sampling::SglFixedPoint, true);
}

HRESULT SurfaceRenderer::draw_impl(void* source_id, const DecodedImage& source, void* mask_id,
    const DecodedImage* mask, void* target_id, DecodedImage& target, Rect source_rect,
    Rect target_rect, RECT clip, std::optional<std::uint32_t> key, BlendMode blend,
    const char** failure_stage, std::uint32_t modulation_color, Sampling sampling, bool self_copy) noexcept {
    if (failure_stage) *failure_stage = "argument validation";
    if (!source_id || !target_id || (source_id == target_id) != self_copy ||
        (mask && mask_id == target_id) ||
        ((mask == nullptr) != (mask_id == nullptr)) ||
        ((blend == BlendMode::AlphaMask) != (mask != nullptr)) ||
        (target.format != ImageFormat::Bgra32 && target.format != ImageFormat::Bgr24) ||
        source.pixels.empty() || target.pixels.empty() ||
        source.pitch < static_cast<std::uint64_t>(source.width) * bytes_per_pixel(source.format) ||
        target.pitch < static_cast<std::uint64_t>(target.width) * bytes_per_pixel(target.format) ||
        static_cast<std::uint64_t>(source.pitch) * source.height > source.pixels.size() ||
        static_cast<std::uint64_t>(target.pitch) * target.height > target.pixels.size() ||
        (source.format == ImageFormat::Indexed8 && source.palette.size() != 1024) ||
        (mask && (source.format != ImageFormat::Indexed8 || mask->format != ImageFormat::Indexed8 ||
            source.width != mask->width || source.height != mask->height || source.pitch != mask->pitch ||
            (!mask->palette.empty() && mask->palette.size() != 1024) || mask->pixels.empty() ||
            mask->pitch < static_cast<std::uint64_t>(mask->width) ||
            static_cast<std::uint64_t>(mask->pitch) * mask->height > mask->pixels.size())) ||
        (blend != BlendMode::Opaque && key &&
            !(source.format == ImageFormat::Indexed8 && blend == BlendMode::Alpha))) return E_INVALIDARG;
    if (failure_stage) *failure_stage = "D3D11 initialization";
    HRESULT hr = initialize();
    if (FAILED(hr)) return hr;
    Surface* src = nullptr;
    Surface* mask_surface = nullptr;
    Surface* dst = nullptr;
    if (failure_stage) *failure_stage = "source surface creation";
    hr = surface(source_id, source, src);
    if (FAILED(hr)) return hr;
    if (mask) {
        if (failure_stage) *failure_stage = "mask surface creation";
        hr = surface(mask_id, *mask, mask_surface);
        if (FAILED(hr)) return hr;
    }
    if (failure_stage) *failure_stage = "destination surface creation";
    hr = surface(target_id, target, dst);
    if (FAILED(hr)) return hr;
    // Insertion can rehash the map, so reacquire both references.
    src = &surfaces_.at(source_id);
    if (mask) mask_surface = &surfaces_.at(mask_id);
    dst = &surfaces_.at(target_id);
    // Queued draws belong to one target; recording them before switching keeps the GPU order intact.
    if (pending_target_ && pending_target_ != dst->texture.Get()) flush_pending();
    // The last recorded draw leaves its target bound. Unbind it only when this draw reads it; binding a bound render
    // target as a shader resource would silently null the input, while re-binding per draw costs driver time.
    // A self-copy reads its snapshot, not the bound target.
    if (!self_copy && bound_target_ &&
        (bound_target_ == src->texture.Get() || (mask && bound_target_ == mask_surface->texture.Get()))) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        bound_target_ = nullptr;
        compositor_.invalidate_renderer_bindings();
    }
    if (failure_stage) *failure_stage = "source texture upload";
    hr = upload_cpu_image(*src, source);
    if (FAILED(hr)) return hr;
    if (mask) {
        if (failure_stage) *failure_stage = "mask texture upload";
        hr = upload_cpu_image(*mask_surface, *mask);
        if (FAILED(hr)) return hr;
    }
    if (failure_stage) *failure_stage = "destination texture upload";
    hr = upload_cpu_image(*dst, target);
    if (FAILED(hr)) return hr;
    const LONG width = static_cast<LONG>(target.width), height = static_cast<LONG>(target.height);
    const auto bound = [](float value, LONG low, LONG high, bool up) {
        const float rounded = up ? std::ceil(value) : std::floor(value);
        return std::clamp(static_cast<LONG>(std::clamp(rounded, -1.0e9f, 1.0e9f)), low, high);
    };
    // Pixels this draw can write, which for blended draws are also the background pixels it reads.
    const RECT written{std::max(bound(target_rect.left, 0, width, false), std::clamp<LONG>(clip.left, 0, width)),
        std::max(bound(target_rect.top, 0, height, false), std::clamp<LONG>(clip.top, 0, height)),
        std::min(bound(target_rect.right, 0, width, true), std::clamp<LONG>(clip.right, 0, width)),
        std::min(bound(target_rect.bottom, 0, height, true), std::clamp<LONG>(clip.bottom, 0, height))};
    const bool writes = written.left < written.right && written.top < written.bottom;
    if (blend != BlendMode::Opaque) {
        if (failure_stage) *failure_stage = "alpha background allocation";
        hr = create_copy_texture(*dst, dst->background, dst->background_view);
        if (FAILED(hr)) return hr;
    }
    ID3D11ShaderResourceView* source_view = src->view.Get();
    if (self_copy) {
        // The draw reads a snapshot of its source pixels taken after the queued draws, which may write them. The SGL
        // samplers load texels in [floor(left), ceil(right)); one more texel covers float rounding on the fractional path.
        if (failure_stage) *failure_stage = "self-copy snapshot allocation";
        hr = create_copy_texture(*dst, dst->snapshot, dst->snapshot_view);
        if (FAILED(hr)) return hr;
        flush_pending();
        const D3D11_BOX read{static_cast<UINT>(bound(source_rect.left, 0, width, false)),
            static_cast<UINT>(bound(source_rect.top, 0, height, false)), 0,
            static_cast<UINT>(std::min(bound(source_rect.right, 0, width, true) + 1, width)),
            static_cast<UINT>(std::min(bound(source_rect.bottom, 0, height, true) + 1, height)), 1};
        if (read.left < read.right && read.top < read.bottom)
            context_->CopySubresourceRegion(dst->snapshot.Get(), 0, read.left, read.top, 0, dst->texture.Get(), 0, &read);
        source_view = dst->snapshot_view.Get();
    }
    if (failure_stage) *failure_stage = "compositor draw";
    const Compositor::RendererSource renderer_source{source.width, source.height,
        source.format == ImageFormat::Indexed8 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM};
    PendingDraw pending;
    if (mask) {
        hr = compositor_.prepare(context_.Get(), dst->target.Get(), source_view,
            src->palette_view.Get(), mask_surface->view.Get(), target.width, target.height,
            source_rect, target_rect, clip, std::nullopt, Sampling::SglFixedPoint,
            dst->background_view.Get(), modulation_color, BlendMode::AlphaMask, failure_stage, &renderer_source,
            pending.draw);
    } else {
        hr = compositor_.prepare(context_.Get(), dst->target.Get(), source_view,
            source.format == ImageFormat::Indexed8 ? src->palette_view.Get() : nullptr, nullptr,
            target.width, target.height, source_rect, target_rect, clip, key, sampling,
            blend != BlendMode::Opaque ? dst->background_view.Get() : nullptr,
            modulation_color, blend, failure_stage, &renderer_source, pending.draw);
    }
    if (FAILED(hr)) return hr;
    ++counters_.draws;
    if (pending.draw.visible) {
        if (blend != BlendMode::Opaque && writes) {
            pending.background = dst->background.Get();
            pending.copy = {static_cast<UINT>(written.left), static_cast<UINT>(written.top), 0,
                static_cast<UINT>(written.right), static_cast<UINT>(written.bottom), 1};
        }
        pending.texture = dst->texture.Get();
        pending.written = writes ? written : RECT{};
        // Clipping in the pixel shader lets draws of different clips share a call; the quad is then shaded within the
        // run's common scissor, so it is used only where the quad is small or mostly visible.
        const auto quad_area = static_cast<std::uint64_t>(bound(target_rect.right, 0, width, true) -
            bound(target_rect.left, 0, width, false)) * static_cast<std::uint64_t>(bound(target_rect.bottom, 0, height, true) -
            bound(target_rect.top, 0, height, false));
        const auto visible_area = writes ? static_cast<std::uint64_t>(written.right - written.left) *
            static_cast<std::uint64_t>(written.bottom - written.top) : 0;
        pending.draw.clip_in_shader = writes && sampling != Sampling::PresentSharp &&
            quad_area <= std::max<std::uint64_t>(visible_area * 2, 4096);
        if (writes) {
            hr = assign_layer(*dst, written, pending.layer);
            if (FAILED(hr)) return hr;
        }
        try {
            if (pending_.capacity() == 0) {
                pending_.reserve(1024);
                pending_draws_.reserve(1024);
                pending_order_.reserve(1024);
                layer_starts_.reserve(1025);
            }
            pending_.push_back(pending);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        pending_target_ = pending.texture;
        if (pending_.size() >= 1024) flush_pending();
    }
    if (writes) {
        auto& dirty = dst->gpu_dirty;
        if (dirty.left >= dirty.right || dirty.top >= dirty.bottom) dirty = written;
        else dirty = {std::min(dirty.left, written.left), std::min(dirty.top, written.top),
            std::max(dirty.right, written.right), std::max(dirty.bottom, written.bottom)};
    }
    dst->cpu_needs_upload = false;
    dst->cpu_needs_readback = true;
    return S_OK;
}

void SurfaceRenderer::flush_pending() noexcept {
    if (pending_.empty()) {
        pending_target_ = nullptr;
        return;
    }
    // Order the draws by layer (a counting sort: a layer is below the queue length), and within a layer by pipeline
    // inputs so that draws sharing them are neighbours; draws of one layer write disjoint pixels, so their order is free.
    const auto count = static_cast<std::uint32_t>(pending_.size());
    std::uint32_t layers = 0;
    for (const auto& queued : pending_) layers = std::max(layers, queued.layer + 1);
    layer_starts_.assign(layers + 1, 0);
    for (const auto& queued : pending_) ++layer_starts_[queued.layer + 1];
    for (std::uint32_t layer = 0; layer < layers; ++layer) layer_starts_[layer + 1] += layer_starts_[layer];
    pending_order_.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) pending_order_[layer_starts_[pending_[index].layer]++] = index;
    for (std::uint32_t start = 0; start < count;) {
        const auto layer = pending_[pending_order_[start]].layer;
        std::uint32_t end = start + 1;
        while (end < count && pending_[pending_order_[end]].layer == layer) ++end;
        std::sort(pending_order_.begin() + start, pending_order_.begin() + end, [this](std::uint32_t left, std::uint32_t right) {
            const auto& a = pending_[left].draw;
            const auto& b = pending_[right].draw;
            if (a.pixel_shader != b.pixel_shader) return std::less<>()(a.pixel_shader, b.pixel_shader);
            if (const int order = std::memcmp(a.resources, b.resources, sizeof(a.resources))) return order < 0;
            if (a.clip_in_shader != b.clip_in_shader) return a.clip_in_shader < b.clip_in_shader;
            if (const int order = std::memcmp(&a.clip, &b.clip, sizeof(a.clip))) return order < 0;
            return left < right;
        });
        start = end;
    }
    for (std::uint32_t start = 0; start < count;) {
        const auto layer = pending_[pending_order_[start]].layer;
        std::uint32_t end = start;
        std::uint32_t blended = 0;
        ID3D11Texture2D* background = nullptr;
        D3D11_BOX bounds{UINT_MAX, UINT_MAX, 0, 0, 0, 1};
        for (; end < count && pending_[pending_order_[end]].layer == layer; ++end) {
            const auto& queued = pending_[pending_order_[end]];
            if (!queued.background) continue;
            ++blended;
            background = queued.background;
            bounds = {std::min(bounds.left, queued.copy.left), std::min(bounds.top, queued.copy.top), 0,
                std::max(bounds.right, queued.copy.right), std::max(bounds.bottom, queued.copy.bottom), 1};
        }
        // Many blended draws in one layer share one copy of their bounding box: it holds the same pixels for each of
        // them, because the layer's other draws are recorded after the copy.
        if (blended >= 16) {
            const auto& queued = pending_[pending_order_[start]];
            context_->CopySubresourceRegion(background, 0, bounds.left, bounds.top, 0, queued.texture,
                0, &bounds);
        } else if (blended) {
            for (std::uint32_t index = start; index < end; ++index) {
                const auto& queued = pending_[pending_order_[index]];
                if (queued.background) context_->CopySubresourceRegion(queued.background, 0, queued.copy.left,
                    queued.copy.top, 0, queued.texture, 0, &queued.copy);
            }
        }
        pending_draws_.clear();
        for (std::uint32_t index = start; index < end; ++index) pending_draws_.push_back(&pending_[pending_order_[index]].draw);
        compositor_.record(context_.Get(), pending_draws_.data(), static_cast<UINT>(pending_draws_.size()), true);
        start = end;
    }
    for (const auto cell : layer_cells_) layer_grid_[cell] = {};
    layer_cells_.clear();
    bound_target_ = pending_target_;
    pending_.clear();
    pending_target_ = nullptr;
}

HRESULT SurfaceRenderer::assign_layer(const Surface& target, const RECT& written, std::uint32_t& layer) noexcept {
    constexpr LONG cell_size = 16;
    const UINT columns = (target.description.Width + cell_size - 1) / cell_size;
    const UINT rows = (target.description.Height + cell_size - 1) / cell_size;
    if (columns != layer_grid_columns_ || rows != layer_grid_rows_) {
        // Only one target has queued draws, and the grid is cleared with every flush.
        try {
            layer_grid_.assign(static_cast<std::size_t>(columns) * rows, LayerCell{});
            layer_cells_.clear();
            layer_cells_.reserve(layer_grid_.size());
        } catch (const std::bad_alloc&) {
            layer_grid_columns_ = layer_grid_rows_ = 0;
            return E_OUTOFMEMORY;
        }
        layer_grid_columns_ = columns;
        layer_grid_rows_ = rows;
    }
    const auto first_column = static_cast<UINT>(written.left / cell_size);
    const auto last_column = static_cast<UINT>((written.right - 1) / cell_size);
    const auto first_row = static_cast<UINT>(written.top / cell_size);
    const auto last_row = static_cast<UINT>((written.bottom - 1) / cell_size);
    // One above the highest queued draw this one overlaps: exactly for the draws listed in the cells, and through the
    // cell's highest layer where more draws touched a cell than it lists.
    std::uint32_t needed = 0;
    for (UINT row = first_row; row <= last_row; ++row) {
        for (UINT column = first_column; column <= last_column; ++column) {
            const LayerCell& cell = layer_grid_[static_cast<std::size_t>(row) * columns + column];
            if (cell.count > LayerCell::capacity) {
                needed = std::max(needed, cell.highest);
                continue;
            }
            for (std::uint16_t entry = 0; entry < cell.count; ++entry) {
                const PendingDraw& queued = pending_[cell.draws[entry]];
                if (queued.written.left < written.right && written.left < queued.written.right &&
                    queued.written.top < written.bottom && written.top < queued.written.bottom)
                    needed = std::max(needed, queued.layer + 1);
            }
        }
    }
    layer = needed;
    const auto index = static_cast<std::uint16_t>(pending_.size());
    for (UINT row = first_row; row <= last_row; ++row) {
        for (UINT column = first_column; column <= last_column; ++column) {
            const auto cell_index = static_cast<std::uint32_t>(static_cast<std::size_t>(row) * columns + column);
            LayerCell& cell = layer_grid_[cell_index];
            if (!cell.highest) layer_cells_.push_back(cell_index);
            cell.highest = std::max(cell.highest, layer + 1);
            if (cell.count < LayerCell::capacity) cell.draws[cell.count++] = index;
            else cell.count = LayerCell::capacity + 1;
        }
    }
    return S_OK;
}

HRESULT SurfaceRenderer::synchronize_to_cpu(void* id, DecodedImage& image) noexcept {
    if (!id) return E_INVALIDARG;
    const auto found = surfaces_.find(id);
    if (found == surfaces_.end()) return S_OK;
    auto& target = found->second;
    if (!target.cpu_needs_readback || target.cpu_needs_upload) return S_OK;
    if (pending_target_ == target.texture.Get()) flush_pending();
    if (!context_ || !target.staging || image.pixels.empty() ||
        (image.format != ImageFormat::Bgra32 && image.format != ImageFormat::Bgr24) ||
        image.pitch < static_cast<std::uint64_t>(image.width) * bytes_per_pixel(image.format) ||
        static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size()) return E_INVALIDARG;
    const RECT dirty = target.gpu_dirty;
    if (dirty.left >= dirty.right || dirty.top >= dirty.bottom ||
        dirty.right > static_cast<LONG>(image.width) || dirty.bottom > static_cast<LONG>(image.height)) {
        target.cpu_needs_readback = false;
        target.gpu_dirty = {};
        return S_OK;
    }
    const long long started = performance_counter();
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    bound_target_ = nullptr;
    compositor_.invalidate_renderer_bindings();
    ID3D11ShaderResourceView* empty[] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 5, empty);
    const D3D11_BOX region{static_cast<UINT>(dirty.left), static_cast<UINT>(dirty.top), 0,
        static_cast<UINT>(dirty.right), static_cast<UINT>(dirty.bottom), 1};
    context_->CopySubresourceRegion(target.staging.Get(), 0, region.left, region.top, 0, target.texture.Get(), 0, &region);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT hr = context_->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    if (mapped.RowPitch < static_cast<std::uint64_t>(image.width) * 4) {
        context_->Unmap(target.staging.Get(), 0);
        return E_INVALIDARG;
    }
    const auto first = static_cast<std::size_t>(dirty.left), columns = static_cast<std::size_t>(dirty.right - dirty.left);
    for (auto y = static_cast<std::uint32_t>(dirty.top); y < static_cast<std::uint32_t>(dirty.bottom); ++y) {
        const auto* output_row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* target_row = image.pixels.data() + static_cast<std::size_t>(y) * image.pitch;
        if (image.format == ImageFormat::Bgra32) {
            std::memcpy(target_row + first * 4, output_row + first * 4, columns * 4);
        } else {
            for (std::size_t x = first; x < first + columns; ++x) {
                std::memcpy(target_row + x * 3, output_row + x * 4, 3);
            }
        }
    }
    context_->Unmap(target.staging.Get(), 0);
    ++counters_.readbacks;
    counters_.transfer_bytes += static_cast<unsigned long long>(columns) * 4 * static_cast<unsigned long long>(dirty.bottom - dirty.top);
    target.gpu_dirty = {};
    counters_.readback_ticks += performance_counter() - started;
    target.cpu_needs_upload = false;
    target.cpu_needs_readback = false;
    return S_OK;
}

bool SurfaceRenderer::gpu_holds_newer_pixels(void* id) const noexcept {
    const auto found = surfaces_.find(id);
    return found != surfaces_.end() && found->second.cpu_needs_readback && !found->second.cpu_needs_upload;
}

HRESULT SurfaceRenderer::synchronize_region_to_cpu(void* id, DecodedImage& image, RECT region) noexcept {
    const auto found = surfaces_.find(id);
    if (found == surfaces_.end()) return S_OK;
    auto& target = found->second;
    if (!target.cpu_needs_readback || target.cpu_needs_upload) return S_OK;
    if (pending_target_ == target.texture.Get()) flush_pending();
    if (!context_ || !target.staging || image.format != ImageFormat::Bgra32 || image.pixels.empty() ||
        image.pitch < static_cast<std::uint64_t>(image.width) * 4 ||
        static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size()) return E_INVALIDARG;
    const RECT& dirty = target.gpu_dirty;
    region = {std::max({region.left, dirty.left, 0L}), std::max({region.top, dirty.top, 0L}),
        std::min({region.right, dirty.right, static_cast<LONG>(image.width)}),
        std::min({region.bottom, dirty.bottom, static_cast<LONG>(image.height)})};
    if (region.left >= region.right || region.top >= region.bottom) return S_OK;
    const long long started = performance_counter();
    const D3D11_BOX box{static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0,
        static_cast<UINT>(region.right), static_cast<UINT>(region.bottom), 1};
    context_->CopySubresourceRegion(target.staging.Get(), 0, box.left, box.top, 0, target.texture.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT hr = context_->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    const auto columns = static_cast<std::size_t>(region.right - region.left) * 4;
    for (LONG y = region.top; y < region.bottom; ++y) {
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * image.pitch + static_cast<std::size_t>(region.left) * 4,
            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch +
                static_cast<std::size_t>(region.left) * 4, columns);
    }
    context_->Unmap(target.staging.Get(), 0);
    ++counters_.readbacks;
    counters_.transfer_bytes += static_cast<unsigned long long>(columns) * static_cast<unsigned long long>(region.bottom - region.top);
    counters_.readback_ticks += performance_counter() - started;
    return S_OK;
}

HRESULT SurfaceRenderer::upload_region(void* id, const DecodedImage& image, RECT region) noexcept {
    const auto found = surfaces_.find(id);
    if (found == surfaces_.end() || found->second.cpu_needs_upload) return S_OK;
    flush_pending();
    if (!context_ || image.format != ImageFormat::Bgra32 || image.pixels.empty() ||
        static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size()) return E_INVALIDARG;
    region = {std::max(region.left, 0L), std::max(region.top, 0L), std::min(region.right, static_cast<LONG>(image.width)),
        std::min(region.bottom, static_cast<LONG>(image.height))};
    if (region.left >= region.right || region.top >= region.bottom) return S_OK;
    const long long started = performance_counter();
    const D3D11_BOX box{static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0,
        static_cast<UINT>(region.right), static_cast<UINT>(region.bottom), 1};
    context_->UpdateSubresource(found->second.texture.Get(), 0, &box,
        image.pixels.data() + static_cast<std::size_t>(region.top) * image.pitch + static_cast<std::size_t>(region.left) * 4,
        image.pitch, 0);
    ++counters_.uploads;
    counters_.transfer_bytes += static_cast<unsigned long long>(region.right - region.left) * 4 *
        static_cast<unsigned long long>(region.bottom - region.top);
    counters_.upload_ticks += performance_counter() - started;
    return S_OK;
}

void SurfaceRenderer::mark_cpu_modified(void* id) noexcept {
    const auto found = surfaces_.find(id);
    if (found == surfaces_.end()) return;
    found->second.cpu_needs_upload = true;
    found->second.cpu_needs_readback = false;
    found->second.gpu_dirty = {};
}

void SurfaceRenderer::mark_palette_modified(void* id) noexcept {
    const auto found = surfaces_.find(id);
    if (found == surfaces_.end()) return;
    found->second.palette_needs_upload = true;
}

void SurfaceRenderer::forget(void* id) noexcept {
    const auto found = surfaces_.find(id);
    if (found == surfaces_.end()) return;
    // Queued draws reference the views of this surface without holding them.
    flush_pending();
    if (context_ && bound_target_ == found->second.texture.Get()) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        bound_target_ = nullptr;
        compositor_.invalidate_renderer_bindings();
    }
    surfaces_.erase(found);
}

HRESULT SurfaceRenderer::open_screen(HWND window, const std::filesystem::path& config, std::string& error) {
    HRESULT hr = initialize();
    if (FAILED(hr)) { error = "Cannot initialize D3D11 device"; return hr; }
    return presentation_.initialize_configured(device_.Get(), window, config, error);
}

HRESULT SurfaceRenderer::set_screen_size(DisplaySize size) noexcept {
    return presentation_.set_render_size(size.width, size.height);
}

void SurfaceRenderer::close_screen() noexcept { presentation_.close(); }

void SurfaceRenderer::shutdown() noexcept {
    pending_.clear();
    pending_target_ = nullptr;
    if (context_) context_->ClearState();
    bound_target_ = nullptr;
    compositor_.invalidate_renderer_bindings();
    presentation_.close();
    surfaces_.clear();
    compositor_ = {};
    context_.Reset();
    device_.Reset();
}

DisplaySize SurfaceRenderer::screen_size() const noexcept {
    return presentation_.render_size();
}

DisplaySize SurfaceRenderer::output_size() const noexcept {
    return presentation_.output_size();
}

DisplayMode SurfaceRenderer::display_mode() const noexcept {
    return presentation_.display_mode();
}

std::optional<unsigned> SurfaceRenderer::max_fps() const noexcept {
    return presentation_.max_fps();
}

HRESULT SurfaceRenderer::present(void* id, const DecodedImage& image) noexcept {
    if (!id || image.format != ImageFormat::Bgra32 || image.pixels.empty() || !presentation_.back_buffer()) return E_INVALIDARG;
    Surface* source = nullptr;
    HRESULT hr = surface(id, image, source);
    if (FAILED(hr)) return hr;
    flush_pending();
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    bound_target_ = nullptr;
    compositor_.invalidate_renderer_bindings();
    ID3D11ShaderResourceView* empty[] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 5, empty);
    hr = upload_cpu_image(*source, image);
    if (FAILED(hr)) return hr;
    hr = presentation_.draw(context_.Get(), compositor_, source->view.Get(),
        {0, 0, static_cast<float>(image.width), static_cast<float>(image.height)},
        {0, 0, static_cast<float>(image.width), static_cast<float>(image.height)});
    if (FAILED(hr)) return hr;
    const FrameCounters counters = counters_;
    counters_ = {};
    return presentation_.present(counters);
}

}
