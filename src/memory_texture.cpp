#include "archive_reader.hpp"
#include "archive_state.hpp"
#include "color_key.hpp"
#include "image_decoder.hpp"
#include "game_resolution.hpp"
#include "game_window_binding.hpp"
#include <intrin.h>
#include "unimplemented.hpp"
#include "mouse_input_transform.hpp"
#include "screen_transform.hpp"
#include "surface_renderer.hpp"

#include <array>
#include <algorithm>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <memory>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
struct Texture {
    std::array<std::uint8_t, 0x48> header{};
    sgl::DecodedImage image;
    std::string name;
    std::vector<std::uint8_t> encoded;
    std::vector<std::uint8_t> locked_palette;
    unsigned locks = 0;
    unsigned external_locks = 0;
    bool external_write_pending = false;
    bool pending_write = false;
    bool initial_color_key_pending = false;
    bool externally_modified = false;
    std::uint32_t key = 0;
    bool keyed = false;
};
struct LockOutput {
    void* pixels;
    void* palette;
    std::uint32_t width, height, pitch;
};
enum class LockIntent { CpuPixels, GpuResource };
// The game calls the draw and clip exports tens of thousands of times per frame from one thread. Under WOW64 an SRW
// lock adds lock-ownership bookkeeping to every acquire and release that costs more than the exports themselves, so the
// exports use a plain spin lock: one interlocked exchange uncontended, yielding and then sleeping when another thread
// holds it for longer (texture loading).
class TexturesMutex {
public:
    void lock() noexcept {
        for (unsigned attempt = 0; locked_.exchange(true, std::memory_order_acquire); ++attempt) {
            while (locked_.load(std::memory_order_relaxed)) {
                if (attempt < 64) YieldProcessor();
                else if (attempt < 128) SwitchToThread();
                else Sleep(1);
                ++attempt;
            }
        }
    }
    void unlock() noexcept { locked_.store(false, std::memory_order_release); }
private:
    std::atomic<bool> locked_{false};
};
TexturesMutex textures_mutex;

std::unordered_map<void*, std::unique_ptr<Texture>> textures;
// Bumped whenever a texture is removed, so cached handle lookups never outlive their texture.
std::uint64_t textures_generation = 0;
struct TextureLookup {
    void* handle = nullptr;
    std::uint64_t generation = ~std::uint64_t{0};
    Texture* texture = nullptr;
};
Texture* find_texture_cached(TextureLookup& cache, void* handle) {
    if (cache.handle == handle && cache.generation == textures_generation && cache.texture) return cache.texture;
    const auto found = textures.find(handle);
    cache = {handle, textures_generation, found == textures.end() ? nullptr : found->second.get()};
    return cache.texture;
}
// Lookups of the selected render source and destination, filled when they are selected.
TextureLookup render_source_lookup;
TextureLookup render_destination_lookup;
void* render_source = nullptr;
void* render_destination = nullptr;
std::array<std::int32_t, 4> clip_rectangle{};
// The mode the game set, and the mode the draws use: the original treats negative modes like mode 2 and draws
// nothing in modes above 3.
std::int32_t requested_clipping_mode = 0;
std::int32_t clipping_mode = 0;
// Incremented whenever the clipping mode, clip rectangle, active clipper or clipper contents change.
std::uint64_t clip_state_version = 0;
// Changes whenever the active clipper or its rectangles change; keys the cached clipper bounds.
std::uint64_t clipper_version = 0;

void report_screen_init_failure(const char* stage, const char* detail) noexcept {
    OutputDebugStringA("Patrician III D3D11: ");
    OutputDebugStringA(stage);
    OutputDebugStringA(": ");
    OutputDebugStringA(detail);
    OutputDebugStringA("\n");
}

struct Clipper {
    std::vector<RECT> rectangles;
    // Rectangles added since the last change that can uncover pixels: the region still covers each of them.
    std::unordered_set<std::uint64_t> added;
};
std::unordered_map<void*, std::unique_ptr<Clipper>> clippers;
std::unique_ptr<Clipper> default_clipper;
Clipper* active_clipper = nullptr;
Clipper* get_active_clipper() {
    if (!default_clipper) {
        try { default_clipper = std::make_unique<Clipper>(); }
        catch (const std::bad_alloc&) { sgl::unimplemented("clipper allocation"); }
    }
    if (!active_clipper) active_clipper = default_clipper.get();
    return active_clipper;
}

__forceinline bool nonempty(const RECT& rectangle) {
    return rectangle.left < rectangle.right && rectangle.top < rectangle.bottom;
}

__forceinline bool intersects(const RECT& first, const RECT& second) {
    return nonempty(first) && nonempty(second) && first.left < second.right &&
        first.right > second.left && first.top < second.bottom && first.bottom > second.top;
}

void append_rectangle(std::vector<RECT>& rectangles, const RECT& rectangle) {
    if (nonempty(rectangle)) rectangles.push_back(rectangle);
}

void subtract_rectangle(const RECT& source, const RECT& cut, std::vector<RECT>& output) {
    if (!intersects(source, cut)) {
        append_rectangle(output, source);
        return;
    }

    const LONG left = std::max(source.left, cut.left);
    const LONG top = std::max(source.top, cut.top);
    const LONG right = std::min(source.right, cut.right);
    const LONG bottom = std::min(source.bottom, cut.bottom);
    append_rectangle(output, {source.left, source.top, source.right, top});
    append_rectangle(output, {source.left, bottom, source.right, source.bottom});
    append_rectangle(output, {source.left, top, left, bottom});
    append_rectangle(output, {right, top, source.right, bottom});
}

// Scratch lists of change_clip_rectangles, kept to reuse their allocations; guarded by the texture lock.
std::vector<RECT> clip_pieces;
std::vector<RECT> clip_remaining;

// Adds (RGN_OR) or removes (RGN_DIFF) a non-empty rectangle. A union appends the parts of the rectangle outside every
// existing rectangle, cut against them in list order; a difference replaces each rectangle by its parts outside the
// cut. The game adds the same dirty rectangles thousands of times per frame on the Hanse map, so a rectangle inside an
// existing one leaves the list unchanged, and existing rectangles that miss it are skipped: every piece lies inside the
// rectangle, so cutting by them would keep the pieces as they are. The result equals the plain cut in list order.
// Returns whether the list changed.
bool change_clip_rectangles(std::vector<RECT>& rectangles, const RECT& rectangle, int operation) {
    if (operation == RGN_OR) {
        for (const auto& existing : rectangles)
            if (existing.left <= rectangle.left && existing.top <= rectangle.top &&
                existing.right >= rectangle.right && existing.bottom >= rectangle.bottom) return false;
        clip_pieces.assign(1, rectangle);
        for (const auto& existing : rectangles) {
            if (!intersects(existing, rectangle)) continue;
            clip_remaining.clear();
            for (const auto& piece : clip_pieces) subtract_rectangle(piece, existing, clip_remaining);
            clip_pieces.swap(clip_remaining);
            if (clip_pieces.empty()) return false;
        }
        rectangles.insert(rectangles.end(), clip_pieces.begin(), clip_pieces.end());
        return true;
    }
    if (operation == RGN_DIFF) {
        clip_remaining.clear();
        for (const auto& existing : rectangles) subtract_rectangle(existing, rectangle, clip_remaining);
        rectangles.swap(clip_remaining);
        return true;
    }
    sgl::unimplemented("clip rectangle operation");
}

std::uint32_t constant_color = 0;
std::uint8_t active_blend_mode = 1;
const std::uint8_t* active_font = nullptr;
int text_mode = 0;
struct Font {
    std::array<std::uint8_t, 0x128> header{};
    std::vector<std::uint8_t> widths;
    std::vector<std::uint8_t> kerning;
};
std::unordered_map<void*, std::unique_ptr<Font>> fonts;
sgl::SurfaceRenderer surface_renderer;
struct Screen {
    void* offscreen = nullptr;
    void* display = nullptr;
};
std::unique_ptr<Screen> screen;
std::filesystem::path display_config_path;
struct ViewState {
    int logical_width = 640;
    int logical_height = 480;
    float near_plane = 0.1f;
    float far_plane = 100.0f;
    int zoom_x = 256;
    int zoom_y = 256;
    float constant_z = 0.0f;
};
ViewState view_state;
bool fast_forward_overlay = false;
HMODULE aim_library = nullptr;
HMODULE archive_library = nullptr;
bool hardware_initialized = false;

std::pair<unsigned, unsigned> screen_logical_size() noexcept {
    return {static_cast<unsigned>(view_state.logical_width), static_cast<unsigned>(view_state.logical_height)};
}

template<typename T> void set_field(Texture& texture, std::size_t offset, T value) {
    std::memcpy(texture.header.data() + offset, &value, sizeof(value));
}

constexpr std::uint32_t texture_flag_toggle_mask = 0x07018197u;

void apply_color_key(sgl::DecodedImage& image, std::uint32_t key, bool keyed) {
    if (!keyed || image.format != sgl::ImageFormat::Bgra32) return;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        auto* row = image.pixels.data() + static_cast<std::size_t>(y) * image.pitch;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            std::uint32_t pixel;
            std::memcpy(&pixel, row + x * 4, sizeof(pixel));
            pixel &= 0x00ffffff;
            if (pixel != key) pixel |= 0xff000000;
            std::memcpy(row + x * 4, &pixel, sizeof(pixel));
        }
    }
}

void apply_palette_color_key(void* palette, std::size_t size, std::uint32_t key, bool keyed) {
    if (!keyed || !palette || size < 1024) return;
    auto* bytes = static_cast<std::uint8_t*>(palette);
    for (std::size_t offset = 0; offset < 1024; offset += 4) {
        std::uint32_t color;
        std::memcpy(&color, bytes + offset, sizeof(color));
        if ((color & 0x00ffffff) == key) {
            color &= 0x00ffffff;
            std::memcpy(bytes + offset, &color, sizeof(color));
        }
    }
}

HRESULT normalize_loaded_image(sgl::DecodedImage& image) noexcept {
    if (image.format == sgl::ImageFormat::Indexed8) {
        if (!image.width || !image.height || image.width > UINT_MAX - 3u || image.pitch < image.width ||
            (!image.pixels.empty() && static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size())) {
            return E_INVALIDARG;
        }
        const auto pitch = (image.width + 3u) & ~3u;
        const auto size = static_cast<std::uint64_t>(pitch) * image.height;
        if (size > SIZE_MAX) return E_OUTOFMEMORY;
        if (!image.pixels.empty() && image.pitch != pitch) {
            std::vector<std::uint8_t> aligned;
            try {
                aligned.resize(static_cast<std::size_t>(size));
            } catch (const std::bad_alloc&) {
                return E_OUTOFMEMORY;
            } catch (const std::length_error&) {
                return E_OUTOFMEMORY;
            }
            for (std::uint32_t y = 0; y < image.height; ++y) {
                const auto* source = image.pixels.data() + static_cast<std::size_t>(y) * image.pitch;
                auto* target = aligned.data() + static_cast<std::size_t>(y) * pitch;
                std::memcpy(target, source, image.width);
            }
            image.pixels = std::move(aligned);
        }
        image.pitch = pitch;
        return S_OK;
    }
    if (image.format != sgl::ImageFormat::Bgr24) return S_OK;
    if (image.width > UINT_MAX / 4 ||
        image.pitch < static_cast<std::uint64_t>(image.width) * 3 ||
        (!image.pixels.empty() && static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size())) {
        return E_INVALIDARG;
    }
    const auto pitch = image.width * 4;
    const auto size = static_cast<std::uint64_t>(pitch) * image.height;
    if (size > SIZE_MAX) return E_OUTOFMEMORY;
    if (!image.pixels.empty()) {
        std::vector<std::uint8_t> converted;
        try {
            converted.resize(static_cast<std::size_t>(size));
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (const std::length_error&) {
            return E_OUTOFMEMORY;
        }
        for (std::uint32_t y = 0; y < image.height; ++y) {
            const auto* source_row = image.pixels.data() + static_cast<std::size_t>(y) * image.pitch;
            auto* target_row = converted.data() + static_cast<std::size_t>(y) * pitch;
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const auto* source_pixel = source_row + static_cast<std::size_t>(x) * 3;
                auto* target_pixel = target_row + static_cast<std::size_t>(x) * 4;
                std::memcpy(target_pixel, source_pixel, 3);
                target_pixel[3] = 0xff;
            }
        }
        image.pixels = std::move(converted);
    }
    image.pitch = pitch;
    image.format = sgl::ImageFormat::Bgra32;
    return S_OK;
}

std::filesystem::path game_directory();

constexpr std::string_view fast_forward_map_name = "images/Vollansichtskarte1280.bmp";

bool is_fast_forward_map(const char* name) noexcept {
    return name && _stricmp(name, fast_forward_map_name.data()) == 0;
}

bool decode_wide_fast_forward_map(const char* name, sgl::ImageDecoder& decoder,
    sgl::DisplaySize size, bool pixels, sgl::DecodedImage& image) noexcept {
    if (!is_fast_forward_map(name) || size.width <= 1280) return false;
    try {
        const auto directory = game_directory();
        if (directory.empty()) return false;
        const bool wqhd = size.width == 2560;
        const auto asset_path = directory / L"renderer-assets" / L"fast-forward-map" /
            (wqhd ? L"Vollansichtskarte2560.bmp" :
                size.width == 1600 ? L"Vollansichtskarte1600.bmp" : L"Vollansichtskarte1920.bmp");
        std::error_code error;
        const auto asset_size = std::filesystem::file_size(asset_path, error);
        constexpr std::uintmax_t maximum_asset_size = 32u * 1024u * 1024u;
        if (error || asset_size == 0 || asset_size > maximum_asset_size || asset_size > LONG_MAX) return false;

        std::vector<std::uint8_t> encoded(static_cast<std::size_t>(asset_size));
        std::ifstream input(asset_path, std::ios::binary);
        if (!input || !input.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size())) ||
            static_cast<std::size_t>(input.gcount()) != encoded.size()) return false;

        const auto result = pixels ? decoder.decode(name, encoded, image) : decoder.inspect(name, encoded, image);
        const auto expected_format = wqhd ? (pixels ? sgl::ImageFormat::Bgr24 : sgl::ImageFormat::Bgra32) : sgl::ImageFormat::Indexed8;
        const auto source_width = size.width;
        const auto source_height = size.height;
        if (FAILED(result) || image.width != source_width || image.height != source_height ||
            image.format != expected_format ||
            (!image.palette.empty() && image.palette.size() != 1024)) return false;

        const auto pixel_size = sgl::bytes_per_pixel(image.format);
        if (pixels && (image.pitch < static_cast<std::uint64_t>(source_width) * pixel_size ||
            static_cast<std::uint64_t>(image.pitch) * source_height > image.pixels.size())) return false;
        if (!pixels) {
            image.pitch = (size.width * pixel_size + 3u) & ~3u;
        }
        image.width = size.width;
        image.height = size.height;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

void reserve_fast_forward_map_capacity(sgl::DecodedImage& image) {
    if (image.pixels.empty()) return;
    image.pixels.reserve(2560u * 1440u * sgl::bytes_per_pixel(sgl::ImageFormat::Bgra32));
}

void* register_texture(std::unique_ptr<Texture> texture) {
    void* handle = texture->header.data();
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    textures.emplace(handle, std::move(texture));
    return handle;
}

std::filesystem::path game_directory() {
    const HMODULE archiver = GetModuleHandleW(L"archiver.dll");
    if (!archiver) return {};
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(archiver, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    return std::filesystem::path(path.data()).parent_path();
}

}

namespace water_effect {
void install() noexcept;
void remove() noexcept;
}

extern "C" void* __cdecl sgl_LoadMemoryTexture(const char* name, std::uint32_t flags) {
    if (!name || !*name) return nullptr;
    try {
        const auto directory = game_directory();
        if (directory.empty()) sgl::unimplemented("sgl_LoadMemoryTexture archiver");
        sgl::ArchiveReader archive;
        sgl::ImageDecoder decoder;
        if (FAILED(archive.initialize(directory / L"archiver.dll")) || FAILED(decoder.initialize(directory / L"AIM.dll"))) {
            sgl::unimplemented("sgl_LoadMemoryTexture dependencies");
        }
        auto texture = std::make_unique<Texture>();
        void* archive_handle = sgl::active_archive();
        if (FAILED(archive.read(archive_handle, name, texture->encoded))) return nullptr;
        texture->name = name;
        const bool load_pixels = (flags & 0x00010040u) != 0;
        const bool replaced_map = decode_wide_fast_forward_map(name, decoder,
            sgl::current_game_resolution(), load_pixels, texture->image);
        HRESULT result = S_OK;
        if (!replaced_map) {
            texture->image = {};
            auto decoder_input = texture->encoded;
            result = load_pixels ? decoder.decode(name, decoder_input, texture->image)
                : decoder.inspect(name, decoder_input, texture->image);
        }
        if (FAILED(result)) {
            char message[512];
            std::snprintf(message, sizeof(message),
                "SGL texture load failed: AIM %s, resource=%s flags=0x%08lx bytes=%zu layout=%u,%u,%u hr=0x%08lx\n",
                (flags & 0x00010040u) != 0 ? "decode" : "inspect", name,
                static_cast<unsigned long>(flags), texture->encoded.size(), decoder.last_layout()[0],
                decoder.last_layout()[1], decoder.last_layout()[2], static_cast<unsigned long>(result));
            OutputDebugStringA(message);
            if (result == E_NOTIMPL) sgl::unimplemented(message);
            return nullptr;
        }
        if (FAILED(normalize_loaded_image(texture->image))) sgl::unimplemented("sgl_LoadMemoryTexture pixel format conversion");
        if (is_fast_forward_map(name)) {
            if (load_pixels) reserve_fast_forward_map_capacity(texture->image);
            sgl::set_fast_forward_map_source_size({texture->image.width, texture->image.height});
        }
        sgl::adapt_game_chrome(name, texture->image);
        const auto& image = texture->image;
        const auto width = static_cast<std::uint16_t>(image.width);
        const auto height = static_cast<std::uint16_t>(image.height);
        const bool indexed = image.format == sgl::ImageFormat::Indexed8;
        texture->key = sgl::color_key() & 0x00ffffff;
        texture->keyed = (flags & 7u) == 7u;
        if ((flags & 0x00010040u) != 0) {
            set_field(*texture, 0x18, image.pixels.data());
            if (!image.palette.empty()) set_field(*texture, 0x1c, image.palette.data());
        }
        set_field(*texture, 0x08, texture->key);
        set_field(*texture, 0x20, width);
        set_field(*texture, 0x22, height);
        set_field(*texture, 0x24, static_cast<std::uint8_t>(sgl::bytes_per_pixel(image.format)));
        set_field(*texture, 0x25, static_cast<std::uint8_t>(sgl::bytes_per_pixel(image.format)));
        set_field(*texture, 0x28, image.pitch);
        const auto texture_flags = flags | (indexed ? 0x20u : 0u);
        set_field(*texture, 0x34, texture_flags);
        if (texture->header[0x37] == 0 && (texture_flags & 1u) != 0)
            set_field(*texture, 0x37, std::uint8_t{1});
        return register_texture(std::move(texture));
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (const std::filesystem::filesystem_error&) {
        return nullptr;
    }
}

static std::unique_ptr<Texture> make_memory_texture(std::uint16_t width, std::uint16_t height, std::uint32_t flags) {
    if (!width || !height) sgl::unimplemented("sgl_CreateMemoryTexture dimensions");
    try {
        auto texture = std::make_unique<Texture>();
        auto& image = texture->image;
        const bool indexed = (flags & 0x20) != 0;
        const std::uint32_t bytes_per_pixel = sgl::bytes_per_pixel(indexed ? sgl::ImageFormat::Indexed8 : sgl::ImageFormat::Bgra32);
        image.width = width;
        image.height = height;
        image.pitch = (static_cast<std::uint32_t>(width) * bytes_per_pixel + 3) & ~3u;
        image.format = indexed ? sgl::ImageFormat::Indexed8 : sgl::ImageFormat::Bgra32;
        const std::uint64_t size = static_cast<std::uint64_t>(image.pitch) * height;
        if (size > SIZE_MAX) return nullptr;
        image.pixels.resize(static_cast<std::size_t>(size));
        texture->key = sgl::color_key() & 0x00ffffff;
        texture->keyed = (flags & 7) == 7;
        texture->initial_color_key_pending = texture->keyed;
        if (indexed) {
            image.palette.resize(1024);
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t color = 0xff000000 | (i * 0x010101);
                if (texture->keyed && (color & 0xffffff) == texture->key) color &= 0xffffff;
                std::memcpy(image.palette.data() + i * 4, &color, sizeof(color));
            }
        }
        set_field(*texture, 0x08, texture->key);
        set_field(*texture, 0x18, image.pixels.data());
        set_field(*texture, 0x1c, indexed ? image.palette.data() : nullptr);
        set_field(*texture, 0x20, width);
        set_field(*texture, 0x22, height);
        set_field(*texture, 0x24, static_cast<std::uint8_t>(bytes_per_pixel));
        set_field(*texture, 0x25, static_cast<std::uint8_t>(bytes_per_pixel));
        set_field(*texture, 0x28, image.pitch);
        set_field(*texture, 0x34, flags | 0x40u);
        if ((flags & 0xff000000u) == 0 && (flags & 1u) != 0) {
            set_field(*texture, 0x37, std::uint8_t{1});
        }
        return texture;
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (const std::length_error&) {
        return nullptr;
    }
}

extern "C" void* __cdecl sgl_CreateMemoryTexture(std::uint16_t width, std::uint16_t height, std::uint32_t flags) {
    auto texture = make_memory_texture(width, height, flags);
    return texture ? register_texture(std::move(texture)) : nullptr;
}

static int lock_texture(Texture& texture, LockOutput* output, int flags, bool external_access = false,
    LockIntent intent = LockIntent::CpuPixels, bool defer_synchronization = false) {
    auto& image = texture.image;
    if (texture.initial_color_key_pending) {
        if (texture.keyed && image.format == sgl::ImageFormat::Bgra32) {
            if (FAILED(surface_renderer.synchronize_to_cpu(texture.header.data(), image))) return 0;
        }
        apply_color_key(image, texture.key, texture.keyed);
        if (texture.keyed && image.format == sgl::ImageFormat::Bgra32)
            surface_renderer.mark_cpu_modified(texture.header.data());
        texture.initial_color_key_pending = false;
    }
    if (image.pixels.empty()) {
        try {
            const auto directory = game_directory();
            if (directory.empty()) sgl::unimplemented("sgl_LockTexture archiver");
            sgl::ImageDecoder decoder;
            if (FAILED(decoder.initialize(directory / L"AIM.dll"))) sgl::unimplemented("sgl_LockTexture dependencies");
            sgl::DecodedImage decoded;
            const bool replaced_map = decode_wide_fast_forward_map(texture.name.c_str(), decoder,
                sgl::current_game_resolution(), true, decoded);
            HRESULT result = S_OK;
            if (!replaced_map) {
                decoded = {};
                auto decoder_input = texture.encoded;
                result = decoder.decode(texture.name.c_str(), decoder_input, decoded);
            }
            if (result == E_NOTIMPL) {
                char message[512];
                std::snprintf(message, sizeof(message), "sgl_LockTexture format: resource=%s layout=%u,%u,%u",
                    texture.name.c_str(), decoder.last_layout()[0], decoder.last_layout()[1], decoder.last_layout()[2]);
                sgl::unimplemented(message);
            }
            if (FAILED(result)) return 0;
            if (FAILED(normalize_loaded_image(decoded))) sgl::unimplemented("sgl_LockTexture pixel format conversion");
            if (is_fast_forward_map(texture.name.c_str())) {
                reserve_fast_forward_map_capacity(decoded);
                sgl::set_fast_forward_map_source_size({decoded.width, decoded.height});
            }
            sgl::adapt_game_chrome(texture.name.c_str(), decoded);
            if (decoded.width != image.width || decoded.height != image.height || decoded.pitch != image.pitch || decoded.format != image.format) {
                char message[512];
                std::snprintf(message, sizeof(message),
                    "sgl_LockTexture metadata mismatch: resource=%s expected=%ux%u pitch=%u format=%u; decoded=%ux%u pitch=%u format=%u; bytes=%zu pixels=%zu palette=%zu",
                    texture.name.c_str(), image.width, image.height, image.pitch, static_cast<unsigned>(image.format),
                    decoded.width, decoded.height, decoded.pitch, static_cast<unsigned>(decoded.format),
                    texture.encoded.size(), decoded.pixels.size(), decoded.palette.size());
                sgl::unimplemented(message);
            }
            image = std::move(decoded);
            if (image.format == sgl::ImageFormat::Indexed8)
                apply_palette_color_key(image.palette.data(), image.palette.size(), texture.key, texture.keyed);
            apply_color_key(image, texture.key, texture.keyed);
        } catch (const std::bad_alloc&) { return 0; }
        catch (const std::filesystem::filesystem_error&) { return 0; }
    }
    if (!defer_synchronization && (external_access || (intent == LockIntent::CpuPixels && (flags & 1)))) {
        if (FAILED(surface_renderer.synchronize_to_cpu(texture.header.data(), image))) return 0;
    }
    if (output && !image.palette.empty() && texture.locked_palette.empty()) {
        try { texture.locked_palette = image.palette; }
        catch (const std::bad_alloc&) { return 0; }
    }
    set_field(texture, 0x18, image.pixels.data());
    set_field(texture, 0x1c, image.palette.empty() ? nullptr : image.palette.data());
    texture.pending_write = texture.pending_write || (flags & 2) != 0;
    ++texture.locks;
    if (output) {
        *output = {image.pixels.data(), texture.locked_palette.empty() ? nullptr : texture.locked_palette.data(),
            image.width, image.height, image.pitch};
    }
    return 1;
}

static void unlock_texture(Texture& texture) {
    --texture.locks;
    if (!texture.locks && texture.pending_write) {
        if (texture.keyed && texture.image.format == sgl::ImageFormat::Bgra32 &&
            FAILED(surface_renderer.synchronize_to_cpu(texture.header.data(), texture.image)))
            sgl::unimplemented("sgl_UnlockTexture D3D11 synchronization");
        apply_color_key(texture.image, texture.key, texture.keyed);
        if (texture.keyed && texture.image.format == sgl::ImageFormat::Bgra32)
            surface_renderer.mark_cpu_modified(texture.header.data());
        texture.pending_write = false;
    }
}

static void* screen_alias(void* handle) {
    const auto value = reinterpret_cast<std::intptr_t>(handle);
    if (value >= 0) return handle;
    if (!screen) sgl::unimplemented("screen handle without screen");
    if (value == -1 || value == -4) return screen->offscreen;
    if (value == -3) return screen->display;
    sgl::unimplemented("screen handle");
}

static bool resize_screen_texture(Texture& texture, sgl::DisplaySize size,
    sgl::DecodedImage& replacement) noexcept {
    const auto& current = texture.image;
    if (texture.external_locks || texture.locks != (render_destination == texture.header.data() ? 1u : 0u) ||
        current.format != sgl::ImageFormat::Bgra32 || current.pixels.empty() ||
        current.pitch < static_cast<std::uint64_t>(current.width) * 4 ||
        static_cast<std::uint64_t>(current.pitch) * current.height > current.pixels.size()) return false;
    if (FAILED(surface_renderer.synchronize_to_cpu(texture.header.data(), texture.image))) return false;
    const std::uint64_t pitch = static_cast<std::uint64_t>(size.width) * 4;
    const std::uint64_t bytes = pitch * size.height;
    if (pitch > UINT_MAX || bytes > SIZE_MAX) return false;
    try {
        replacement.width = size.width;
        replacement.height = size.height;
        replacement.pitch = static_cast<std::uint32_t>(pitch);
        replacement.format = current.format;
        replacement.pixels.assign(static_cast<std::size_t>(bytes), 0);
        // Framebuffer pixels use the old layout coordinates and must be redrawn after a mode change.
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

struct ResolutionTextureReplacement {
    Texture* texture;
    sgl::DecodedImage image;
    bool preserve_palette;
};

bool is_game_chrome(const std::string& name) noexcept {
    return _stricmp(name.c_str(), "images/HauptscreenE1280.bmp") == 0 ||
        _stricmp(name.c_str(), "images/HauptscreenA1280.bmp") == 0;
}

bool stage_resolution_textures(sgl::DisplaySize size,
    std::vector<ResolutionTextureReplacement>& replacements) noexcept {
    try {
        const auto has_replacements = std::any_of(textures.begin(), textures.end(), [](const auto& entry) {
            return is_game_chrome(entry.second->name) || is_fast_forward_map(entry.second->name.c_str());
        });
        if (!has_replacements) return true;
        const auto directory = game_directory();
        if (directory.empty()) return false;
        sgl::ImageDecoder decoder;
        if (FAILED(decoder.initialize(directory / L"AIM.dll"))) return false;
        replacements.reserve(textures.size());
        for (auto& entry : textures) {
            auto& texture = *entry.second;
            const bool map_texture = is_fast_forward_map(texture.name.c_str());
            const bool chrome_texture = is_game_chrome(texture.name);
            if (!map_texture && !chrome_texture) continue;
            if (texture.external_locks || texture.locks || texture.pending_write || texture.externally_modified)
                return false;
            sgl::DecodedImage image;
            const bool pixels = !texture.image.pixels.empty();
            bool replaced_map = map_texture && decode_wide_fast_forward_map(
                texture.name.c_str(), decoder, size, pixels, image);
            HRESULT decoded = S_OK;
            if (!replaced_map) {
                image = {};
                auto encoded = texture.encoded;
                decoded = pixels ? decoder.decode(texture.name.c_str(), encoded, image)
                    : decoder.inspect(texture.name.c_str(), encoded, image);
            }
            if (FAILED(decoded) || FAILED(normalize_loaded_image(image))) return false;
            if (chrome_texture) sgl::adapt_game_chrome(texture.name.c_str(), image, size);
            if (image.width > UINT16_MAX || image.height > UINT16_MAX) return false;
            if (!map_texture && (image.format != texture.image.format ||
                image.palette.size() != texture.image.palette.size())) return false;
            if (pixels) {
                const auto required = static_cast<std::uint64_t>(image.pitch) * image.height;
                if (image.pixels.empty() || required > image.pixels.size() ||
                    required > texture.image.pixels.capacity()) return false;
                if (image.format == sgl::ImageFormat::Indexed8)
                    apply_palette_color_key(image.palette.data(), image.palette.size(), texture.key, texture.keyed);
                apply_color_key(image, texture.key, texture.keyed);
            } else if (!image.pixels.empty()) {
                return false;
            }
            if (chrome_texture) {
                if (image.palette.size() != texture.image.palette.size() ||
                    (!texture.locked_palette.empty() && texture.locked_palette.size() != image.palette.size()))
                    return false;
                if (!texture.image.palette.empty()) image.palette = texture.image.palette;
            } else if (!image.palette.empty() && image.palette.size() != 1024) {
                return false;
            }
            replacements.push_back({&texture, std::move(image), chrome_texture});
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

void commit_resolution_textures(std::vector<ResolutionTextureReplacement>& replacements) noexcept {
    for (auto& replacement : replacements) {
        auto& texture = *replacement.texture;
        auto& target = texture.image;
        auto& source = replacement.image;
        if (!target.pixels.empty()) {
            target.pixels.resize(source.pixels.size());
            std::memcpy(target.pixels.data(), source.pixels.data(), source.pixels.size());
        }
        if (source.palette.empty()) {
            target.palette.clear();
            texture.locked_palette.clear();
        } else {
            target.palette.resize(source.palette.size());
            std::memcpy(target.palette.data(), source.palette.data(), source.palette.size());
            if (!texture.locked_palette.empty()) {
                texture.locked_palette.resize(source.palette.size());
                std::memcpy(texture.locked_palette.data(), source.palette.data(), source.palette.size());
            }
        }
        target.width = source.width;
        target.height = source.height;
        target.pitch = source.pitch;
        target.format = source.format;
        if (!replacement.preserve_palette && is_fast_forward_map(texture.name.c_str()))
            sgl::set_fast_forward_map_source_size({target.width, target.height});
        surface_renderer.forget(texture.header.data());
        set_field(texture, 0x18, target.pixels.empty() ? nullptr : target.pixels.data());
        set_field(texture, 0x1c, target.palette.empty() ? nullptr : target.palette.data());
        set_field(texture, 0x20, static_cast<std::uint16_t>(target.width));
        set_field(texture, 0x22, static_cast<std::uint16_t>(target.height));
        const auto bytes_per_pixel = static_cast<std::uint8_t>(sgl::bytes_per_pixel(target.format));
        set_field(texture, 0x24, bytes_per_pixel);
        set_field(texture, 0x25, bytes_per_pixel);
        set_field(texture, 0x28, target.pitch);
        if (!replacement.preserve_palette) {
            std::uint32_t flags = 0;
            std::memcpy(&flags, texture.header.data() + 0x34, sizeof(flags));
            flags = (flags & ~0x20u) | (target.format == sgl::ImageFormat::Indexed8 ? 0x20u : 0u);
            set_field(texture, 0x34, flags);
        }
    }
}

static bool resize_screen(sgl::DisplaySize size) noexcept {
    if (!screen || !sgl::is_supported_display_size(size)) return false;
    auto offscreen = textures.find(screen->offscreen);
    auto display = textures.find(screen->display);
    if (offscreen == textures.end() || display == textures.end()) return false;
    if (offscreen->second->image.width == size.width && offscreen->second->image.height == size.height &&
        display->second->image.width == size.width && display->second->image.height == size.height) {
        return surface_renderer.set_screen_size(size) == S_OK && sgl::update_game_resolution(size);
    }

    sgl::DecodedImage next_offscreen;
    sgl::DecodedImage next_display;
    std::vector<ResolutionTextureReplacement> next_chrome;
    if (!resize_screen_texture(*offscreen->second, size, next_offscreen) ||
        !resize_screen_texture(*display->second, size, next_display) ||
        !stage_resolution_textures(size, next_chrome)) return false;
    if (FAILED(surface_renderer.set_screen_size(size))) return false;

    commit_resolution_textures(next_chrome);
    surface_renderer.forget(screen->offscreen);
    surface_renderer.forget(screen->display);
    offscreen->second->image = std::move(next_offscreen);
    display->second->image = std::move(next_display);
    for (auto* texture : {offscreen->second.get(), display->second.get()}) {
        set_field(*texture, 0x18, texture->image.pixels.data());
        set_field(*texture, 0x20, static_cast<std::uint16_t>(size.width));
        set_field(*texture, 0x22, static_cast<std::uint16_t>(size.height));
        set_field(*texture, 0x28, texture->image.pitch);
    }
    return sgl::update_game_resolution(size);
}

sgl::DisplayApplyResult sgl::apply_options_display_resolution(std::string& error) {
    error.clear();
    const auto size = pending_game_resolution();
    try {
        {
            std::lock_guard<TexturesMutex> lock(textures_mutex);
            if (!screen) {
                error = "Cannot change resolution because the game screen is not active.";
                return DisplayApplyResult::Failed;
            }
            if (!resize_screen(size)) {
                error = "The renderer could not prepare the selected resolution; the current size was kept.";
                return DisplayApplyResult::Failed;
            }
        }
        if (!save_display_config(display_config_path, size, surface_renderer.display_mode(), error)) {
            try {
                error = "The resolution changed for this session, but high_res.ini could not be saved: " + error;
            } catch (...) {
                error.clear();
            }
            return DisplayApplyResult::AppliedNotSaved;
        }
        return DisplayApplyResult::Applied;
    } catch (const std::exception& exception) {
        try { error = std::string("The resolution could not be changed: ") + exception.what(); }
        catch (...) { error.clear(); }
        return DisplayApplyResult::Failed;
    } catch (...) {
        error.clear();
        return DisplayApplyResult::Failed;
    }
}

static void* external_handle(void* handle) {
    if (screen && handle == screen->offscreen) return reinterpret_cast<void*>(-4);
    if (screen && handle == screen->display) return reinterpret_cast<void*>(-3);
    return handle;
}

static bool is_screen_texture(void* handle) {
    return screen && (handle == screen->offscreen || handle == screen->display);
}

static RECT texture_bounds(void* handle, const sgl::DecodedImage& image) {
    const auto dimension = [](std::uint32_t value) {
        return static_cast<LONG>(std::min<std::uint32_t>(value, static_cast<std::uint32_t>(LONG_MAX)));
    };
    const auto [logical_width, logical_height] = screen_logical_size();
    const LONG width = is_screen_texture(handle) ? dimension(logical_width) : dimension(image.width);
    const LONG height = is_screen_texture(handle) ? dimension(logical_height) : dimension(image.height);
    return {0, 0, std::max<LONG>(0, width), std::max<LONG>(0, height)};
}

static __forceinline bool intersect_with(RECT& rectangle, const RECT& bounds) {
    rectangle.left = std::max(rectangle.left, bounds.left);
    rectangle.top = std::max(rectangle.top, bounds.top);
    rectangle.right = std::min(rectangle.right, bounds.right);
    rectangle.bottom = std::min(rectangle.bottom, bounds.bottom);
    return nonempty(rectangle);
}

static bool make_screen_transform(void* handle, const sgl::DecodedImage& image,
    sgl::ScreenTransform& transform) {
    if (!is_screen_texture(handle)) return false;
    const auto [logical_width, logical_height] = screen_logical_size();
    if (!sgl::ScreenTransform::create(logical_width, logical_height, image.width, image.height, transform)) {
        sgl::unimplemented("invalid SGL screen transform");
    }
    return true;
}

// Fills clips in place so a caller can reuse its storage. With within, only clips overlapping it are kept.
static void collect_draw_clips(const sgl::DecodedImage& target, std::vector<RECT>& clips, const RECT* within = nullptr) {
    clips.clear();
    try {
        const RECT bounds = texture_bounds(render_destination, target);
        if (clipping_mode == 0) {
            if (nonempty(bounds)) clips.push_back(bounds);
            return;
        }
        if (clipping_mode == 1) {
            RECT clip{clip_rectangle[0], clip_rectangle[1], clip_rectangle[2], clip_rectangle[3]};
            if (intersect_with(clip, bounds)) clips.push_back(clip);
            return;
        }
        // Like the original, clipping modes other than 0 to 3 leave nothing visible.
        if (clipping_mode != 2 && clipping_mode != 3) return;

        auto* clipper = get_active_clipper();
        const RECT clip_bounds{clip_rectangle[0], clip_rectangle[1], clip_rectangle[2], clip_rectangle[3]};
        if (within) {
            for (const RECT& region : clipper->rectangles) {
                if (region.left >= within->right || region.right <= within->left ||
                    region.top >= within->bottom || region.bottom <= within->top) continue;
                RECT clip = region;
                if (clipping_mode == 3 && !intersect_with(clip, clip_bounds)) continue;
                if (intersect_with(clip, bounds)) clips.push_back(clip);
            }
            return;
        }
        clips.reserve(clipper->rectangles.size());
        for (RECT clip : clipper->rectangles) {
            if (clipping_mode == 3 && !intersect_with(clip, clip_bounds)) continue;
            if (intersect_with(clip, bounds)) clips.push_back(clip);
        }
    } catch (const std::bad_alloc&) {
        sgl::unimplemented("sgl_DrawBitmapRect region allocation");
    } catch (const std::length_error&) {
        sgl::unimplemented("sgl_DrawBitmapRect region allocation");
    }
}

static std::vector<RECT> draw_clip_rectangles(const sgl::DecodedImage& target) {
    std::vector<RECT> clips;
    collect_draw_clips(target, clips);
    return clips;
}

// Draw paths query the clip list once per sprite, and the game changes the clip rectangle between most sprites while
// the clipper rectangles stay fixed for a whole redraw pass. The clipper rectangles are therefore clipped to the
// destination and mapped once per clipper state; a draw then only intersects them with its clip rectangle. Screen
// mapping rounds every edge with a monotonic function, so mapping the parts and intersecting the results equals
// mapping the logical intersection, as long as that intersection is non-empty.
struct ClipperDrawCache {
    Clipper* clipper = nullptr;
    std::uint64_t version = ~std::uint64_t{0};
    void* destination = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int logical_width = 0;
    int logical_height = 0;
    bool screen = false;
    std::vector<RECT> logical;
    std::vector<RECT> mapped;
};
static ClipperDrawCache clipper_draw_cache;

static bool map_draw_clip(const RECT& clip, bool map_to_screen, const sgl::ScreenTransform& transform, RECT& result) {
    if (!map_to_screen) {
        result = clip;
        return nonempty(result);
    }
    return transform.map_clip(clip, result);
}

// within is the draw's bounding box in the destination's logical coordinates; clipper rectangles outside it are
// skipped. The returned list is valid until the next call.
static const std::vector<RECT>& cached_draw_clips(const sgl::DecodedImage& target, bool map_to_screen,
    const sgl::ScreenTransform& transform, const RECT* within = nullptr) {
    static std::vector<RECT> clips;
    clips.clear();
    try {
        const RECT bounds = texture_bounds(render_destination, target);
        RECT mapped{};
        if (clipping_mode == 0 || clipping_mode == 1) {
            RECT clip = bounds;
            if (clipping_mode == 1 && !intersect_with(clip, RECT{clip_rectangle[0], clip_rectangle[1],
                clip_rectangle[2], clip_rectangle[3]})) return clips;
            if (nonempty(clip) && map_draw_clip(clip, map_to_screen, transform, mapped)) clips.push_back(mapped);
            return clips;
        }
        if (clipping_mode != 2 && clipping_mode != 3) return clips;
        auto& cache = clipper_draw_cache;
        auto* clipper = get_active_clipper();
        if (cache.clipper != clipper || cache.version != clipper_version || cache.destination != render_destination ||
            cache.width != target.width || cache.height != target.height || cache.screen != map_to_screen ||
            cache.logical_width != view_state.logical_width || cache.logical_height != view_state.logical_height) {
            cache.logical.clear();
            cache.mapped.clear();
            for (RECT clip : clipper->rectangles) {
                if (!intersect_with(clip, bounds)) continue;
                cache.logical.push_back(clip);
                cache.mapped.push_back(map_draw_clip(clip, map_to_screen, transform, mapped) ? mapped : RECT{});
            }
            cache.clipper = clipper;
            cache.version = clipper_version;
            cache.destination = render_destination;
            cache.width = target.width;
            cache.height = target.height;
            cache.screen = map_to_screen;
            cache.logical_width = view_state.logical_width;
            cache.logical_height = view_state.logical_height;
        }
        RECT limit = bounds;
        RECT mapped_limit{};
        if (clipping_mode == 3) {
            if (!intersect_with(limit, RECT{clip_rectangle[0], clip_rectangle[1], clip_rectangle[2], clip_rectangle[3]}))
                return clips;
            if (!map_draw_clip(limit, map_to_screen, transform, mapped_limit)) return clips;
        }
        for (std::size_t index = 0; index < cache.logical.size(); ++index) {
            const RECT& region = cache.logical[index];
            if (within && (region.left >= within->right || region.right <= within->left ||
                region.top >= within->bottom || region.bottom <= within->top)) continue;
            if (clipping_mode == 2) {
                if (nonempty(cache.mapped[index])) clips.push_back(cache.mapped[index]);
                continue;
            }
            RECT clip = region;
            if (!intersect_with(clip, limit)) continue;
            RECT result = cache.mapped[index];
            if (intersect_with(result, mapped_limit)) clips.push_back(result);
        }
    } catch (const std::bad_alloc&) {
        sgl::unimplemented("sgl_DrawBitmapRect region allocation");
    } catch (const std::length_error&) {
        sgl::unimplemented("sgl_DrawBitmapRect region allocation");
    }
    return clips;
}

// Bounding box of the active clipper's rectangles, cached until the clipper or its rectangles change.
static const RECT& active_clipper_bounds() {
    static Clipper* cached_clipper = nullptr;
    static std::uint64_t cached_version = ~std::uint64_t{0};
    static RECT bounds{};
    auto* clipper = get_active_clipper();
    if (clipper == cached_clipper && clipper_version == cached_version) return bounds;
    bounds = {LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    for (const RECT& region : clipper->rectangles) {
        if (!nonempty(region)) continue;
        bounds = {std::min(bounds.left, region.left), std::min(bounds.top, region.top),
            std::max(bounds.right, region.right), std::max(bounds.bottom, region.bottom)};
    }
    if (!nonempty(bounds)) bounds = {};
    cached_clipper = clipper;
    cached_version = clipper_version;
    return bounds;
}

// Logical pixels by which draw bounds are widened before testing them against clips: covers the outward rounding of
// mapped clips, including screens where one logical pixel maps to less than one output pixel.
static LONG draw_clip_margin(const sgl::DecodedImage& target) {
    if (!is_screen_texture(render_destination)) return 1;
    const auto [logical_width, logical_height] = screen_logical_size();
    if (!target.width || !target.height) return 1 << 20;
    return static_cast<LONG>(2 + std::max(logical_width / target.width, logical_height / target.height));
}

// True when a draw to the current destination cannot touch any clip of the current state. The game issues most of
// its sprite draws outside the visible clip region, so this test runs before any per-draw setup. It is conservative:
// the visible area is the bounding box of the clips, and the destination is widened by more than the rounding of
// mapped clips, matching the logical-bounds filter of cached_draw_clips.
static bool draw_outside_clip(const Texture& target, int dx, int dy, int dw, int dh) {
    if (clipping_mode < 0 || clipping_mode > 3) return false;
    // Destination bounds and margin change only with the destination and the screen size.
    static struct {
        void* destination = nullptr;
        std::uint32_t width = 0, height = 0;
        int logical_width = -1, logical_height = -1;
        bool screen = false;
        RECT bounds{};
        LONG margin = 1;
    } destination_cache;
    auto& cached = destination_cache;
    const bool screen_destination = is_screen_texture(render_destination);
    if (cached.destination != render_destination || cached.screen != screen_destination || cached.width != target.image.width ||
        cached.height != target.image.height || cached.logical_width != view_state.logical_width ||
        cached.logical_height != view_state.logical_height) {
        cached.destination = render_destination;
        cached.screen = screen_destination;
        cached.width = target.image.width;
        cached.height = target.image.height;
        cached.logical_width = view_state.logical_width;
        cached.logical_height = view_state.logical_height;
        cached.bounds = texture_bounds(render_destination, target.image);
        cached.margin = draw_clip_margin(target.image);
    }
    RECT visible = cached.bounds;
    if (clipping_mode == 1 || clipping_mode == 3) {
        const RECT clip{clip_rectangle[0], clip_rectangle[1], clip_rectangle[2], clip_rectangle[3]};
        if (!intersect_with(visible, clip)) return true;
    }
    if ((clipping_mode == 2 || clipping_mode == 3) && !intersect_with(visible, active_clipper_bounds())) return true;
    if (!nonempty(visible)) return true;
    const long long margin = cached.margin;
    return static_cast<long long>(dx) + dw + margin <= visible.left || static_cast<long long>(dx) - margin >= visible.right ||
        static_cast<long long>(dy) + dh + margin <= visible.top || static_cast<long long>(dy) - margin >= visible.bottom;
}

static HMODULE load_game_library(const wchar_t* name) {
    std::array<wchar_t, 32768> path{};
    const HMODULE loaded = GetModuleHandleW(name);
    const DWORD length = GetModuleFileNameW(loaded, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return nullptr;
    const auto full_path = loaded ? std::filesystem::path(path.data()) : std::filesystem::path(path.data()).parent_path() / name;
    return LoadLibraryExW(full_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static bool city_executable_verified = false;

static void verify_city_scroll_executable() noexcept {
    static constexpr std::uintptr_t address = 0x592FCF;
    static constexpr std::uint8_t original[] = {
        0x6A, 0xFD, 0xE8, 0xEA, 0x89, 0xF2, 0xFF, 0x6A, 0xFD, 0xE8, 0xD3, 0x89, 0xF2, 0xFF, 0x8B, 0x44,
        0x24, 0x18, 0x8B, 0x4C, 0x24, 0x1C, 0x8B, 0x54, 0x24, 0x20, 0x6A, 0x04, 0x57, 0x53, 0x55, 0x50,
        0x51, 0x52, 0xE8, 0x5A, 0x83, 0xF2, 0xFF};
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return;
    auto* code = reinterpret_cast<const std::uint8_t*>(address);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(code, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize < address + sizeof(original)) return;
    city_executable_verified = std::memcmp(code, original, sizeof(original)) == 0;
}

// City Map bottom band (R2). Three City Map passes in Patrician3.exe walk tile rows, 11 pixels apart, down to
// the bottom of the view and then through a band below it. From each band cell they also step further down to
// cells whose sprites reach up into the view: 0x590F30 renders the ground and terrain objects into the map
// buffer, 0x591440 draws buildings and walls, and 0x590100 is the third pass with the same structure. Each
// band ends at view bottom + [view+0x514] = 21 pixels, which holds two rows (one of each parity) unless the
// view height is 1 modulo 11. The 2560x1440 City Map view is 1398 = 11 * 127 + 1 pixels high, so one row
// parity below the bottom edge was skipped: every other column of mountains, buildings and walls entering at
// the bottom was never drawn, and scrolling carried those gaps up through the picture. Each band end, at loop
// entry and at the loop's back edge, is widened to 22 pixels. That always covers exactly two rows, so no row
// is visited twice at any view height. The replacements keep every register value of the original sequence
// except the band end, which is one larger.
static void patch_city_bottom_band() noexcept {
    struct Site {
        std::uintptr_t address;
        std::size_t size;
        std::uint8_t original[17];
        std::uint8_t replacement[17];
    };
    static constexpr Site sites[] = {
        // 0x590100 pass, entry: mov eax,[esi+18]; mov ebp,[esi+514]; mov ebx,[esi+30]; add eax,ebp; add eax,ebx
        // -> mov ebp,[esi+514]; mov ebx,[esi+30]; lea eax,[ebp+ebx+1]; add eax,[esi+18]
        {0x590233, 16,
            {0x8B, 0x46, 0x18, 0x8B, 0xAE, 0x14, 0x05, 0x00, 0x00, 0x8B, 0x5E, 0x30, 0x03, 0xC5, 0x03, 0xC3},
            {0x8B, 0xAE, 0x14, 0x05, 0x00, 0x00, 0x8B, 0x5E, 0x30, 0x8D, 0x44, 0x1D, 0x01, 0x03, 0x46, 0x18}},
        // 0x590100 pass, back edge: mov ecx,[esi+18]; lea eax,[eax+ebx*4]; mov ebx,[esi+30]; add ecx,ebp;
        // xor dl,1; add ecx,ebx -> lea eax,[eax+ebx*4]; mov ebx,[esi+30]; xor dl,1; lea ecx,[ebp+ebx+1];
        // add ecx,[esi+18]
        {0x5904D1, 16,
            {0x8B, 0x4E, 0x18, 0x8D, 0x04, 0x98, 0x8B, 0x5E, 0x30, 0x03, 0xCD, 0x80, 0xF2, 0x01, 0x03, 0xCB},
            {0x8D, 0x04, 0x98, 0x8B, 0x5E, 0x30, 0x80, 0xF2, 0x01, 0x8D, 0x4C, 0x1D, 0x01, 0x03, 0x4E, 0x18}},
        // Ground pass, entry: mov edx,[esi+30]; mov ebp,[esi+514]; mov edi,[esi+18]; add edx,ebp; add edx,edi
        // -> mov ebp,[esi+514]; mov edi,[esi+18]; lea edx,[ebp+edi+1]; add edx,[esi+30]
        {0x591039, 16,
            {0x8B, 0x56, 0x30, 0x8B, 0xAE, 0x14, 0x05, 0x00, 0x00, 0x8B, 0x7E, 0x18, 0x03, 0xD5, 0x03, 0xD7},
            {0x8B, 0xAE, 0x14, 0x05, 0x00, 0x00, 0x8B, 0x7E, 0x18, 0x8D, 0x54, 0x3D, 0x01, 0x03, 0x56, 0x30}},
        // Ground pass, back edge: mov edx,[esi+30]; lea ebx,[ebx+ecx*4]; mov cl,[esp+13]; add edx,ebp; xor cl,1;
        // add edx,edi -> lea ebx,[ebx+ecx*4]; mov cl,[esp+13]; xor cl,1; lea edx,[ebp+edi+1]; add edx,[esi+30]
        {0x5912BD, 17,
            {0x8B, 0x56, 0x30, 0x8D, 0x1C, 0x8B, 0x8A, 0x4C, 0x24, 0x13, 0x03, 0xD5, 0x80, 0xF1, 0x01, 0x03, 0xD7},
            {0x8D, 0x1C, 0x8B, 0x8A, 0x4C, 0x24, 0x13, 0x80, 0xF1, 0x01, 0x8D, 0x54, 0x3D, 0x01, 0x03, 0x56, 0x30}},
        // Building pass, entry: mov ecx,[esi+30]; mov eax,[esi+514]; mov ebx,[esi+18]; add ecx,eax; add ecx,ebx
        // -> mov eax,[esi+514]; mov ebx,[esi+18]; lea ecx,[eax+ebx+1]; add ecx,[esi+30]
        {0x591611, 16,
            {0x8B, 0x4E, 0x30, 0x8B, 0x86, 0x14, 0x05, 0x00, 0x00, 0x8B, 0x5E, 0x18, 0x03, 0xC8, 0x03, 0xCB},
            {0x8B, 0x86, 0x14, 0x05, 0x00, 0x00, 0x8B, 0x5E, 0x18, 0x8D, 0x4C, 0x18, 0x01, 0x03, 0x4E, 0x30}},
        // Building pass, back edge: mov eax,[esi+30]; xor dl,1; add eax,ecx; mov [esp+14],edi; add eax,ebx
        // -> lea eax,[ecx+ebx+1]; add eax,[esi+30]; xor dl,1; mov [esp+14],edi
        {0x591940, 14,
            {0x8B, 0x46, 0x30, 0x80, 0xF2, 0x01, 0x03, 0xC1, 0x89, 0x7C, 0x24, 0x14, 0x03, 0xC3},
            {0x8D, 0x44, 0x19, 0x01, 0x03, 0x46, 0x30, 0x80, 0xF2, 0x01, 0x89, 0x7C, 0x24, 0x14}},
    };
    static bool attempted = false;
    if (attempted || !city_executable_verified) return;
    attempted = true;
    for (const Site& site : sites) {
        if (std::memcmp(reinterpret_cast<const void*>(site.address), site.original, site.size) != 0) {
            OutputDebugStringA("Patrician III D3D11: City Map bottom band patch skipped (unexpected executable)\n");
            return;
        }
    }
    for (const Site& site : sites) {
        auto* const code = reinterpret_cast<std::uint8_t*>(site.address);
        DWORD protection = 0;
        if (!VirtualProtect(code, site.size, PAGE_EXECUTE_READWRITE, &protection)) return;
        std::memcpy(code, site.replacement, site.size);
        DWORD ignored = 0;
        VirtualProtect(code, site.size, protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), code, site.size);
    }
}

extern "C" int __cdecl sgl_InitHardware() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (hardware_initialized) return 1;
    try {
        HMODULE aim = load_game_library(L"AIM.dll");
        if (!aim) return 0;
        HMODULE archive = nullptr;
        try { archive = load_game_library(L"archiver.dll"); }
        catch (...) { FreeLibrary(aim); throw; }
        if (!archive) { FreeLibrary(aim); return 0; }
        const auto aim_vendor = reinterpret_cast<int (__cdecl*)(int, int)>(GetProcAddress(aim, "?AIM_SETVENDOR@@YAHHH@Z"));
        const auto archive_vendor = reinterpret_cast<int (__cdecl*)(int, int)>(GetProcAddress(archive, "arc_SetVendor"));
        if (!aim_vendor || !archive_vendor || !aim_vendor(0, 0) || !archive_vendor(0, 0) || FAILED(surface_renderer.initialize())) {
            FreeLibrary(archive);
            FreeLibrary(aim);
            return 0;
        }
        aim_library = aim;
        archive_library = archive;
        sgl::initialize_color_key();
        hardware_initialized = true;
        return 1;
    } catch (const std::bad_alloc&) { return 0; }
    catch (const std::filesystem::filesystem_error&) { return 0; }
}

static void destroy_screen();

extern "C" void* __cdecl sgl_InitScreen(HWND window, int fullscreen, const char*, const std::uint32_t* settings) {
    verify_city_scroll_executable();
    patch_city_bottom_band();
    if (!window || !IsWindow(window) || (fullscreen != 0 && fullscreen != 1) ||
        (settings && (!settings[0] || !settings[1] || settings[0] > INT_MAX || settings[1] > INT_MAX))) {
        report_screen_init_failure("invalid SGL screen arguments", "window, fullscreen flag, or dimensions rejected");
        return nullptr;
    }
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (screen) sgl::unimplemented("sgl_InitScreen multiple screens");
    const int previous_logical_width = view_state.logical_width;
    const int previous_logical_height = view_state.logical_height;
    const int logical_width = settings ? static_cast<int>(settings[0]) : 640;
    const int logical_height = settings ? static_cast<int>(settings[1]) : 480;
    try {
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size()) {
            report_screen_init_failure("locating game executable", "GetModuleFileNameW failed or truncated the path");
            return nullptr;
        }
        std::string error;
        const auto config_path = std::filesystem::path(path.data()).parent_path() / L"high_res.ini";
        const HRESULT hr = surface_renderer.open_screen(window, config_path, error);
        if (FAILED(hr)) {
            report_screen_init_failure("opening D3D11 screen", error.c_str());
            return nullptr;
        }
        display_config_path = config_path;
        const auto size = surface_renderer.screen_size();
        auto offscreen = make_memory_texture(static_cast<std::uint16_t>(size.width), static_cast<std::uint16_t>(size.height), 0);
        auto display = make_memory_texture(static_cast<std::uint16_t>(size.width), static_cast<std::uint16_t>(size.height), 0);
        if (!offscreen || !display) {
            report_screen_init_failure("allocating screen buffers", "could not create both render-size buffers");
            surface_renderer.close_screen();
            return nullptr;
        }
        auto next = std::make_unique<Screen>();
        next->offscreen = offscreen->header.data();
        next->display = display->header.data();
        textures.emplace(next->offscreen, std::move(offscreen));
        try { textures.emplace(next->display, std::move(display)); }
        catch (...) { textures.erase(next->offscreen); ++textures_generation; surface_renderer.close_screen(); throw; }
        if (render_destination) unlock_texture(*textures.at(render_destination));
        render_source = nullptr;
        render_destination = next->offscreen;
        lock_texture(*textures.at(render_destination), nullptr, 2);
        constant_color = 0xffffffff;
        screen = std::move(next);
        view_state.logical_width = logical_width;
        view_state.logical_height = logical_height;
        if (!sgl::install_mouse_input_transform(window, size.width, size.height,
            static_cast<unsigned>(view_state.logical_width), static_cast<unsigned>(view_state.logical_height))) {
            char message[128];
            std::snprintf(message, sizeof(message), "Cannot install SGL mouse coordinate transform: error=%lu\n",
                static_cast<unsigned long>(GetLastError()));
            report_screen_init_failure("installing mouse transform", message);
            destroy_screen();
            view_state.logical_width = previous_logical_width;
            view_state.logical_height = previous_logical_height;
            return nullptr;
        }
        if (!sgl::install_game_resolution(size, error)) {
            report_screen_init_failure("installing game resolution", error.c_str());
            destroy_screen();
            view_state.logical_width = previous_logical_width;
            view_state.logical_height = previous_logical_height;
            return nullptr;
        }
        if (!sgl::set_game_frame_limit(surface_renderer.max_fps()))
            OutputDebugStringA("Frame limit: the game's frame limiter does not match the verified executable\n");
        water_effect::install();
        // Not fatal: without it the window still works, but the game can move it, keep it topmost and hold capture.
        if (!sgl::install_game_window_binding(window,
                surface_renderer.display_mode() == sgl::DisplayMode::Borderless))
            OutputDebugStringA("Cannot bind the game's window placement, capture and cursor calls\n");
        return screen.get();
    } catch (const std::bad_alloc&) {
        report_screen_init_failure("initializing SGL screen", "memory allocation failed");
        surface_renderer.close_screen();
        return nullptr;
    } catch (const std::filesystem::filesystem_error&) {
        report_screen_init_failure("initializing SGL screen", "filesystem operation failed");
        surface_renderer.close_screen();
        return nullptr;
    }
}

extern "C" float __cdecl sgl_SetConstantZVal(float value) {
    if (!std::isfinite(value)) sgl::unimplemented("sgl_SetConstantZVal parameter");
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const float previous = view_state.constant_z;
    view_state.constant_z = value;
    return previous;
}

extern "C" void __cdecl sgl_SetScreenDimensions(int, int, int width, int height,
    float near_plane, float far_plane) {
    if (width <= 0 || height <= 0 || !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
        near_plane <= 0.0f || far_plane <= near_plane) sgl::unimplemented("sgl_SetScreenDimensions parameters");
    {
        std::lock_guard<TexturesMutex> lock(textures_mutex);
        view_state.logical_width = width;
        view_state.logical_height = height;
        view_state.near_plane = near_plane;
        view_state.far_plane = far_plane;
    }
    const auto render_size = surface_renderer.screen_size();
    sgl::update_mouse_input_transform(static_cast<unsigned>(width), static_cast<unsigned>(height),
        render_size.width, render_size.height);
    sgl_SetConstantZVal(near_plane);
}

extern "C" int __cdecl sgl_SetZoomFactor(int x, int y) {
    if (x <= 0 || y <= 0) sgl::unimplemented("sgl_SetZoomFactor parameters");
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    view_state.zoom_x = x;
    view_state.zoom_y = y;
    return 1;
}

static void destroy_screen() {
    if (!screen) return;
    sgl::remove_game_window_binding();
    sgl::remove_mouse_input_transform();
    if (render_destination) {
        const auto entry = textures.find(render_destination);
        if (entry == textures.end() || entry->second->locks != 1) sgl::unimplemented("sgl_DestroyActiveScreen locked destination");
        unlock_texture(*entry->second);
    }
    render_source = render_destination = nullptr;
    surface_renderer.forget(screen->offscreen);
    surface_renderer.forget(screen->display);
    textures.erase(screen->offscreen);
    textures.erase(screen->display);
    ++textures_generation;
    surface_renderer.close_screen();
    screen.reset();
}

extern "C" void __cdecl sgl_DestroyActiveScreen() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    destroy_screen();
}

extern "C" int __cdecl sgl_SwitchScreenMode(const std::uint32_t* requested_mode) {
    // Legacy SGL callers use 16 as a mode tag; presentation and surfaces stay 32-bit.
    if (!requested_mode || (requested_mode[2] != 16 && requested_mode[2] != 32)) return 0;
    auto width = requested_mode[0], height = requested_mode[1];
    const bool logical_layout = (width == 640 && height == 480) || (width == 800 && height == 600);
    // Game views always use the resolution chosen in the options or high_res.ini. Some code paths, such as saving
    // with the P3Modding modloader, request the game's own resolution (1280x1024) instead; that would silently
    // switch the game to it.
    if (!logical_layout) {
        const auto chosen = sgl::current_game_resolution();
        if (sgl::is_supported_display_size(chosen)) {
            width = chosen.width;
            height = chosen.height;
        }
    }
    const sgl::DisplaySize requested_size{width, height};
    if (!logical_layout && !sgl::is_supported_display_size(requested_size)) return 0;
    {
        std::lock_guard<TexturesMutex> lock(textures_mutex);
        if (!screen) return 0;
        if (!logical_layout && !resize_screen(requested_size)) return 0;
        view_state.logical_width = static_cast<int>(width);
        view_state.logical_height = static_cast<int>(height);
    }
    const auto render_size = surface_renderer.screen_size();
    sgl::update_mouse_input_transform(width, height, render_size.width, render_size.height);
    return 1;
}

extern "C" void __cdecl sgl_CleanUp() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    destroy_screen();
}

extern "C" void __cdecl sgl_ShotDown() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    destroy_screen();
    water_effect::remove();
    sgl::remove_game_resolution();
    surface_renderer.shutdown();
    if (archive_library) FreeLibrary(archive_library);
    if (aim_library) FreeLibrary(aim_library);
    archive_library = aim_library = nullptr;
    hardware_initialized = false;
    display_config_path.clear();
}

extern "C" int __cdecl sgl_SwapBuffer(std::uint32_t flags) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (!screen || flags != 2) sgl::unimplemented("sgl_SwapBuffer state");
    fast_forward_overlay = false;
    sgl::refresh_accelerated_map_layout();
    const auto& output = *textures.at(screen->display);
    if (FAILED(surface_renderer.present(screen->display, output.image))) sgl::unimplemented("sgl_SwapBuffer D3D11");
    const auto render_size = surface_renderer.screen_size();
    sgl::update_mouse_input_transform(static_cast<unsigned>(view_state.logical_width),
        static_cast<unsigned>(view_state.logical_height), render_size.width, render_size.height);
    return 0;
}

// City water effect. Once per frame the city view (0x588960) locks the offscreen surface (handle -4, flags 3) and calls
// FUN_0059e8e0, which redraws its listed 8x8 water tiles in place from displaced pixels of the same surface. A whole-
// surface lock costs a full readback (waiting for the GPU) and a full upload per frame. With this binding the lock
// defers synchronization and the effect runs on the GPU (SurfaceRenderer::draw_water) with the original's in-place
// write order, so the CPU never waits for the GPU. When the GPU pass cannot run, the original runs on the CPU after a
// readback of the tiles plus the largest displacement, and the tiles it wrote are uploaded.
namespace water_effect {
constexpr std::uintptr_t lock_return = 0x5889e7;
constexpr std::uintptr_t call_site = 0x588a25;
constexpr std::uintptr_t function = 0x59e8e0;
// Object layout: width and height at +4 and +8, height bytes at +0x24 in rows of width + 2 bytes, tile list count at
// +0x2c and (x, y) shorts every 8 bytes from +0x34, clamp tables at +0xbbb8 (x, byte offsets) and +0xbbbc (y, row
// offsets) with the pitch they were built for at +0xbbc0, pixel offsets at +0xbbc4 and +0xbbc8.
constexpr std::size_t width_offset = 0x4;
constexpr std::size_t height_offset = 0x8;
constexpr std::size_t heights_offset = 0x24;
constexpr std::size_t tile_count_offset = 0x2c;
constexpr std::size_t tile_list_offset = 0x34;
constexpr std::size_t table_x_offset = 0xbbb8;
constexpr std::size_t table_y_offset = 0xbbbc;
constexpr std::size_t table_pitch_offset = 0xbbc0;
constexpr std::size_t offset_x_offset = 0xbbc4;
constexpr std::size_t offset_y_offset = 0xbbc8;
// The color channels go through a saturation table indexed from -255 to 510.
constexpr std::uintptr_t saturation_table = 0x6e52c7;
// Height differences of -255..255 displace reads by at most 126 pixels horizontally and 127 vertically.
constexpr LONG read_margin = 128;
bool installed = false;
bool lock_deferred = false;
void* deferred_texture = nullptr;
// Clipped tiles of the current call and their grid positions; kept to reuse the allocations.
std::vector<RECT> tiles;
std::vector<std::uint32_t> tile_keys;

using Function = void(__fastcall*)(void*, void*, std::uint8_t*, int, int, const RECT*, int);

template <typename T> T read(const std::uint8_t* object, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}

// The GPU pass computes the clamp tables (FUN_0059dc40) and the saturation table instead of reading them; it runs only
// while the game's tables hold those values for this call.
bool tables_match(const std::uint8_t* object, int width, int height, int pitch) noexcept {
    static bool saturation_matches = false;
    if (!saturation_matches) {
        const auto* table = reinterpret_cast<const std::uint8_t*>(saturation_table);
        for (int value = -255; value <= 510; ++value)
            if (table[value] != std::clamp(value, 0, 255)) return false;
        saturation_matches = true;
    }
    if (width <= 0 || height <= 0 || read<int>(object, table_pitch_offset) != pitch) return false;
    const auto* table_x = read<const std::int32_t*>(object, table_x_offset);
    const auto* table_y = read<const std::int32_t*>(object, table_y_offset);
    // Entries 0..254 hold positions -255..-1; position p is at p + 255, up to width + 254 (height + 254 for rows).
    return table_x && table_y && table_x[0] == 0 && table_x[254] == 0 && table_x[255] == 0 &&
        table_x[255 + width - 1] == (width - 1) * 4 && table_x[255 + width + 254] == (width - 1) * 4 &&
        table_y[0] == 0 && table_y[254] == 0 && table_y[255] == 0 &&
        table_y[255 + height - 1] == (height - 1) * pitch && table_y[255 + height + 254] == (height - 1) * pitch;
}

void __fastcall hook(void* self, void* unused, std::uint8_t* pixels, int offset_x, int offset_y, const RECT* bounds,
    int pitch) {
    RECT written{};
    bool partial = false;
    {
        std::lock_guard<TexturesMutex> lock(textures_mutex);
        const auto entry = lock_deferred ? textures.find(deferred_texture) : textures.end();
        if (entry != textures.end() && self && bounds) {
            const auto* object = static_cast<const std::uint8_t*>(self);
            const int count = read<int>(object, tile_count_offset);
            const int shift_x = read<int>(object, offset_x_offset), shift_y = read<int>(object, offset_y_offset);
            written = {LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
            tiles.clear();
            tile_keys.clear();
            // The GPU pass needs every written pixel once: a tile listed twice is rewritten from its own output.
            bool listed = count <= 1 << 20;
            for (int index = 0; index < count && index < 1 << 20; ++index) {
                const auto tile = read<std::array<std::int16_t, 2>>(object, tile_list_offset + static_cast<std::size_t>(index) * 8);
                const LONG left = std::max<LONG>(tile[0] * 8 - shift_x - offset_x, bounds->left);
                const LONG top = std::max<LONG>(tile[1] * 8 - shift_y - offset_y, bounds->top);
                const LONG right = std::min<LONG>(tile[0] * 8 - shift_x - offset_x + 8, bounds->right);
                const LONG bottom = std::min<LONG>(tile[1] * 8 - shift_y - offset_y + 8, bounds->bottom);
                if (left >= right || top >= bottom) continue;
                written = {std::min(written.left, left), std::min(written.top, top),
                    std::max(written.right, right), std::max(written.bottom, bottom)};
                try {
                    if (listed) {
                        tiles.push_back({left, top, right, bottom});
                        tile_keys.push_back(static_cast<std::uint16_t>(tile[0]) | static_cast<std::uint32_t>(static_cast<std::uint16_t>(tile[1])) << 16);
                    }
                } catch (const std::bad_alloc&) { listed = false; }
            }
            if (listed) {
                std::sort(tile_keys.begin(), tile_keys.end());
                listed = std::adjacent_find(tile_keys.begin(), tile_keys.end()) == tile_keys.end();
            }
            auto& image = entry->second->image;
            const int width = read<int>(object, width_offset), height = read<int>(object, height_offset);
            const auto* heights = read<const std::uint8_t*>(object, heights_offset);
            // h(x, y) is heights[(y + shift_y + offset_y) * (width + 2) + x + shift_x + offset_x].
            const long long height_row = static_cast<long long>(written.top) + shift_y + offset_y;
            const long long height_column = static_cast<long long>(written.left) + shift_x + offset_x;
            if (written.left < written.right && listed && heights && pixels == image.pixels.data() && pitch > 0 &&
                static_cast<std::uint32_t>(pitch) == image.pitch && height_row >= 0 && height_column >= 0 &&
                tables_match(object, width, height, pitch) &&
                SUCCEEDED(surface_renderer.draw_water(deferred_texture, image, tiles.data(), static_cast<UINT>(tiles.size()),
                    written, heights + height_row * (width + 2) + height_column, static_cast<UINT>(width) + 2,
                    static_cast<UINT>(width), static_cast<UINT>(height)))) {
                lock_deferred = false;
                return;
            }
            partial = true;
            if (written.left < written.right) {
                const RECT region{written.left - read_margin, written.top - read_margin,
                    written.right + read_margin, written.bottom + read_margin};
                if (FAILED(surface_renderer.synchronize_region_to_cpu(deferred_texture, image, region)))
                    sgl::unimplemented("water effect: D3D11 region readback");
            }
        }
    }
    reinterpret_cast<Function>(function)(self, unused, pixels, offset_x, offset_y, bounds, pitch);
    if (!partial) return;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto entry = textures.find(deferred_texture);
    if (entry != textures.end() && written.left < written.right &&
        FAILED(surface_renderer.upload_region(deferred_texture, entry->second->image, written)))
        sgl::unimplemented("water effect: D3D11 region upload");
    lock_deferred = false;
}

// Redirects the effect call after checking both call sites against the verified executable.
void install() noexcept {
    if (installed || !sgl::game_binding_active()) return;
    auto* const call = reinterpret_cast<std::uint8_t*>(call_site);
    const auto* const lock_call = reinterpret_cast<const std::uint8_t*>(lock_return - 5);
    std::int32_t relative = 0;
    std::memcpy(&relative, call + 1, sizeof(relative));
    if (call[0] != 0xe8 || call_site + 5 + relative != function || lock_call[0] != 0xe8) return;
    const auto hook_relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&hook) - (call_site + 5));
    DWORD protection = 0, discarded = 0;
    if (!VirtualProtect(call + 1, sizeof(hook_relative), PAGE_EXECUTE_READWRITE, &protection)) return;
    std::memcpy(call + 1, &hook_relative, sizeof(hook_relative));
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    VirtualProtect(call + 1, sizeof(hook_relative), protection, &discarded);
    installed = true;
}

void remove() noexcept {
    if (!installed) return;
    auto* const call = reinterpret_cast<std::uint8_t*>(call_site);
    const auto original = static_cast<std::int32_t>(function - (call_site + 5));
    DWORD protection = 0, discarded = 0;
    if (VirtualProtect(call + 1, sizeof(original), PAGE_EXECUTE_READWRITE, &protection)) {
        std::memcpy(call + 1, &original, sizeof(original));
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        VirtualProtect(call + 1, sizeof(original), protection, &discarded);
    }
    installed = false;
    lock_deferred = false;
    deferred_texture = nullptr;
}
}

extern "C" int __cdecl sgl_LockTexture(void* handle, LockOutput* output, int flags) {
    // The original locks every texture the same way whatever the flags; values other than read (1), write (2) and
    // both (3) are treated as both here, so neither the read-back nor the upload is skipped.
    if (flags < 1 || flags > 3) flags = 3;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const bool water_lock = water_effect::installed && caller == water_effect::lock_return &&
        reinterpret_cast<std::intptr_t>(handle) == -4 && flags == 3;
    handle = screen_alias(handle);
    const auto entry = textures.find(handle);
    if (entry == textures.end()) sgl::unimplemented("sgl_LockTexture object");
    auto& texture = *entry->second;
    if (texture.external_locks == UINT_MAX) sgl::unimplemented("sgl_LockTexture nesting");
    const bool defer = water_lock && texture.external_locks == 0 &&
        texture.image.format == sgl::ImageFormat::Bgra32;
    const int result = lock_texture(texture, output, flags, true, LockIntent::CpuPixels, defer);
    if (result && defer) {
        water_effect::lock_deferred = true;
        water_effect::deferred_texture = handle;
        ++texture.external_locks;
        return result;
    }
    if (result) {
        ++texture.external_locks;
        texture.external_write_pending = texture.external_write_pending || (flags & 2) != 0;
    }
    return result;
}

extern "C" void __cdecl sgl_UnlockTexture(void* handle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    handle = screen_alias(handle);
    const auto entry = textures.find(handle);
    if (entry == textures.end() || !entry->second->locks || !entry->second->external_locks ||
        (handle == render_destination && entry->second->locks == 1)) sgl::unimplemented("sgl_UnlockTexture object");
    auto& texture = *entry->second;
    if (handle == water_effect::deferred_texture && texture.external_locks == 1) {
        // The effect binding already uploaded the tiles it wrote; the rest of the surface is unchanged.
        unlock_texture(texture);
        --texture.external_locks;
        water_effect::deferred_texture = nullptr;
        water_effect::lock_deferred = false;
        return;
    }
    const bool final_external_write = texture.external_locks == 1 && texture.external_write_pending;
    unlock_texture(texture);
    --texture.external_locks;
    if (final_external_write) {
        surface_renderer.mark_cpu_modified(handle);
        texture.external_write_pending = false;
        texture.externally_modified = true;
    }
}

extern "C" int __cdecl sgl_ChangePalette(void* handle, std::uint32_t* palette) {
    if (!handle) return 0;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto entry = textures.find(handle);
    if (entry == textures.end()) sgl::unimplemented("sgl_ChangePalette object");
    auto& texture = *entry->second;
    if (!lock_texture(texture, nullptr, 1)) return 0;
    if (!texture.image.palette.empty()) {
        if (!palette) sgl::unimplemented("sgl_ChangePalette palette");
        if (texture.keyed) apply_palette_color_key(palette, 1024, texture.key, true);
        std::memcpy(texture.image.palette.data(), palette, 1024);
        surface_renderer.mark_palette_modified(handle);
    }
    unlock_texture(texture);
    return 0;
}

extern "C" std::uint32_t __cdecl sgl_ChangeTextureFlags(void* handle, std::uint32_t flags) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto entry = textures.find(handle);
    if (entry == textures.end()) sgl::unimplemented("sgl_ChangeTextureFlags object");

    auto& texture = *entry->second;
    std::uint32_t current = 0;
    std::memcpy(&current, texture.header.data() + 0x34, sizeof(current));
    auto updated = ((current ^ flags) & texture_flag_toggle_mask) ^ current;
    set_field(texture, 0x34, updated);
    if (texture.header[0x37] == 0 && (updated & 1u) != 0) {
        updated |= 0x01000000u;
        set_field(texture, 0x34, updated);
    }
    return current & texture_flag_toggle_mask;
}

extern "C" std::uint32_t __cdecl sgl_GetColorKey(void* handle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    handle = screen_alias(handle);
    const auto entry = textures.find(handle);
    if (entry == textures.end()) sgl::unimplemented("sgl_GetColorKey object");
    const auto& texture = *entry->second;
    return (texture.header[0x34] & 7) ? texture.key : 0;
}

extern "C" void __cdecl sgl_PrefetchTexture(void* handle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (textures.find(handle) == textures.end()) sgl::unimplemented("sgl_PrefetchTexture object");
}

// The game calls this on Ctrl+S. The original writes the screen to grabs/Grab_<n>.bmp; no screenshot is taken here.
extern "C" void __cdecl sgl_GrabScreen(int, const char*) {}

// The game reports archive and texture failures through this and continues. Like the original, the text is shown in
// an error box captioned "SGL".
extern "C" int __cdecl sgl_MessageBox(const char* text) {
    return MessageBoxA(nullptr, text ? text : "", "SGL", MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

extern "C" void* __cdecl sgl_SetRenderSource(void* handle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    handle = screen_alias(handle);
    if (handle) {
        const auto found = textures.find(handle);
        if (found == textures.end()) sgl::unimplemented("sgl_SetRenderSource object");
        render_source_lookup = {handle, textures_generation, found->second.get()};
    }
    void* previous = render_source;
    render_source = handle;
    return external_handle(previous);
}

extern "C" void* __cdecl sgl_SetRenderDest(void* handle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    handle = screen_alias(handle);
    auto next = textures.end();
    if (handle) {
        next = textures.find(handle);
        if (next == textures.end()) sgl::unimplemented("sgl_SetRenderDest object");
        if (next->second->image.format == sgl::ImageFormat::Indexed8) handle = nullptr;
    }
    void* previous = render_destination;
    if (previous == handle) return external_handle(previous);
    if (previous) unlock_texture(*textures.at(previous));
    render_destination = nullptr;
    if (handle && lock_texture(*next->second, nullptr, 2)) render_destination = handle;
    return external_handle(previous);
}

extern "C" int __cdecl sgl_Set2dClippingMode(int mode) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const int previous = requested_clipping_mode;
    const int effective = mode < 0 ? 2 : mode;
    if (clipping_mode != effective) ++clip_state_version;
    requested_clipping_mode = mode;
    clipping_mode = effective;
    return previous;
}

extern "C" int __cdecl sgl_Get2dClippingMode() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    return requested_clipping_mode;
}

extern "C" void* __cdecl sgl_CreateClipper() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    try {
        auto clipper = std::make_unique<Clipper>();
        void* handle = clipper.get();
        clippers.emplace(handle, std::move(clipper));
        return handle;
    } catch (const std::bad_alloc&) { return nullptr; }
}

extern "C" void* __cdecl sgl_GetRectClipper() { return sgl_CreateClipper(); }

extern "C" void* __cdecl sgl_SetActiveClipper(void* handle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    auto* previous = get_active_clipper();
    if (!handle) active_clipper = default_clipper.get();
    else if (handle == default_clipper.get()) active_clipper = default_clipper.get();
    else {
        const auto found = clippers.find(handle);
        if (found == clippers.end()) sgl::unimplemented("sgl_SetActiveClipper object");
        active_clipper = found->second.get();
    }
    if (active_clipper != previous) { ++clip_state_version; ++clipper_version; }
    return previous;
}

extern "C" void __cdecl sgl_FreeClipper(void* handle) {
    if (!handle) return;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (handle == default_clipper.get()) return;
    const auto found = clippers.find(handle);
    if (found == clippers.end()) sgl::unimplemented("sgl_FreeClipper object");
    if (active_clipper == handle) active_clipper = default_clipper.get();
    clippers.erase(found);
    ++clip_state_version;
    ++clipper_version;
}

extern "C" void __cdecl sgl_FreeRectClipper(void* handle) { sgl_FreeClipper(handle); }

extern "C" void __cdecl sgl_CopyClipper(void* source_handle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    auto* destination = get_active_clipper();
    if (!source_handle) sgl::unimplemented("sgl_CopyClipper source");

    Clipper* source = nullptr;
    if (source_handle == default_clipper.get()) source = default_clipper.get();
    else {
        const auto found = clippers.find(source_handle);
        if (found == clippers.end()) sgl::unimplemented("sgl_CopyClipper object");
        source = found->second.get();
    }
    if (source == destination) return;

    try {
        auto rectangles = source->rectangles;
        destination->rectangles.swap(rectangles);
        destination->added.clear();
        ++clip_state_version;
        ++clipper_version;
    } catch (const std::bad_alloc&) {
        sgl::unimplemented("sgl_CopyClipper allocation");
    } catch (const std::length_error&) {
        sgl::unimplemented("sgl_CopyClipper allocation");
    }
}

static void change_clip(const RECT* rectangle, int operation) {
    if (!rectangle) sgl::unimplemented("clip rectangle");
    if (rectangle->left >= rectangle->right || rectangle->top >= rectangle->bottom) return;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    auto* clipper = get_active_clipper();
    // The game adds the same few hundred rectangles thousands of times per frame; a union with one already added
    // changes nothing. Keys hold the four edges in 16 bits each, so larger coordinates are not remembered.
    const auto fits = [](LONG value) { return value >= SHRT_MIN && value <= SHRT_MAX; };
    const bool keyed = fits(rectangle->left) && fits(rectangle->top) && fits(rectangle->right) && fits(rectangle->bottom);
    const std::uint64_t key = static_cast<std::uint16_t>(rectangle->left) |
        static_cast<std::uint64_t>(static_cast<std::uint16_t>(rectangle->top)) << 16 |
        static_cast<std::uint64_t>(static_cast<std::uint16_t>(rectangle->right)) << 32 |
        static_cast<std::uint64_t>(static_cast<std::uint16_t>(rectangle->bottom)) << 48;
    if (operation == RGN_OR && keyed && clipper->added.count(key)) return;
    try {
        const bool changed = change_clip_rectangles(clipper->rectangles, *rectangle, operation);
        if (operation == RGN_OR) { if (keyed) clipper->added.insert(key); }
        else clipper->added.clear();
        if (!changed) return;
        ++clip_state_version;
        ++clipper_version;
    } catch (const std::bad_alloc&) {
        sgl::unimplemented("clip rectangle allocation");
    } catch (const std::length_error&) {
        sgl::unimplemented("clip rectangle allocation");
    }
}

extern "C" void __cdecl sgl_AddClipRect_r(const RECT* rect) { change_clip(rect, RGN_OR); }
extern "C" void __cdecl sgl_SubClipRect_r(const RECT* rect) { change_clip(rect, RGN_DIFF); }
extern "C" void __cdecl sgl_AddClipRect_4i(int l, int t, int r, int b) { const RECT rect{l, t, r, b}; change_clip(&rect, RGN_OR); }
extern "C" void __cdecl sgl_SubClipRect_4i(int l, int t, int r, int b) { const RECT rect{l, t, r, b}; change_clip(&rect, RGN_DIFF); }

extern "C" void __cdecl sgl_ResetClipper() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    get_active_clipper()->rectangles.clear();
    get_active_clipper()->added.clear();
    ++clip_state_version;
    ++clipper_version;
}

extern "C" int __cdecl sgl_IsRectInClippingRegion(const RECT* rect) {
    if (!rect) sgl::unimplemented("sgl_IsRectInClippingRegion rectangle");
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (clipping_mode == 1) return rect->left < clip_rectangle[2] && rect->right > clip_rectangle[0] &&
        rect->top < clip_rectangle[3] && rect->bottom > clip_rectangle[1];
    if (clipping_mode == 2 || clipping_mode == 3) {
        const auto& regions = get_active_clipper()->rectangles;
        return std::any_of(regions.begin(), regions.end(), [&](const RECT& region) {
            return intersects(region, *rect);
        });
    }
    return 1;
}

extern "C" void __cdecl sgl_SetClipRect_4i(int left, int top, int right, int bottom) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const std::array<std::int32_t, 4> next{left, top, right, bottom};
    if (clip_rectangle != next) ++clip_state_version;
    clip_rectangle = next;
}

extern "C" void __cdecl sgl_SetClipRect(int left, int top, int right, int bottom) {
    sgl_SetClipRect_4i(left, top, right, bottom);
}

extern "C" void __cdecl sgl_SetClipRect_r(const std::int32_t* rectangle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (std::memcmp(clip_rectangle.data(), rectangle, sizeof(clip_rectangle)) != 0) ++clip_state_version;
    std::memcpy(clip_rectangle.data(), rectangle, sizeof(clip_rectangle));
}

extern "C" void __cdecl sgl_GetClipRect_r(std::int32_t* rectangle) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    std::memcpy(rectangle, clip_rectangle.data(), sizeof(clip_rectangle));
}

extern "C" void __cdecl sgl_GetClipRect_4i(std::int32_t* left, std::int32_t* top,
    std::int32_t* right, std::int32_t* bottom) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    *left = clip_rectangle[0];
    *top = clip_rectangle[1];
    *right = clip_rectangle[2];
    *bottom = clip_rectangle[3];
}

extern "C" std::uint32_t __cdecl sgl_GetConstantColor() {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    return constant_color;
}

extern "C" void __cdecl sgl_SetFont(const void* font) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    active_font = static_cast<const std::uint8_t*>(font);
}

static void* create_font(const unsigned char* characters, void* texture_handle, int glyph_width,
    int glyph_height, int spacing, int space_width, bool kerned, int fixed_width) {
    if (!characters || !texture_handle) return nullptr;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto entry = textures.find(texture_handle);
    if (entry == textures.end() || glyph_width <= 0 || glyph_height <= 0)
        sgl::unimplemented("sgl_MemoryFont_CreateProportional parameters");
    auto& texture = *entry->second;
    if (texture.image.format != sgl::ImageFormat::Bgra32 ||
        static_cast<unsigned>(glyph_height) > texture.image.height)
        sgl::unimplemented("sgl_MemoryFont_CreateProportional image");
    try {
        auto font = std::make_unique<Font>();
        const auto limit = std::min<std::uint32_t>(texture.image.width / static_cast<unsigned>(glyph_width), 256);
        if (!limit) sgl::unimplemented("sgl_MemoryFont_CreateProportional glyph width");
        std::uint32_t count = 0;
        while (count < limit && characters[count]) {
            font->header[0x20 + characters[count]] = static_cast<std::uint8_t>(count);
            ++count;
        }
        std::uint32_t glyph_count = 1;
        for (std::size_t i = 0; i < 256; ++i)
            glyph_count = std::max(glyph_count, static_cast<std::uint32_t>(font->header[0x20 + i]) + 1);
        font->widths.resize(glyph_count);
        if (kerned) font->kerning.resize(glyph_count * glyph_count);
        std::vector<std::uint8_t> edges;
        if (kerned) edges.resize(static_cast<std::size_t>(glyph_count) * glyph_height * 2);
        if (!lock_texture(texture, nullptr, 1)) return nullptr;
        for (std::uint32_t i = 0; i < glyph_count; ++i) {
            if (fixed_width >= 0) {
                font->widths[i] = static_cast<std::uint8_t>(fixed_width);
                continue;
            }
            int rightmost = 0;
            for (int y = 0; y < glyph_height; ++y) {
                const auto* row = texture.image.pixels.data() + static_cast<std::size_t>(y) * texture.image.pitch +
                    static_cast<std::size_t>(i) * glyph_width * 4;
                for (int x = 0; x < glyph_width; ++x) {
                    const auto* pixel = row + x * 4;
                    if (static_cast<std::uint8_t>(pixel[1] + pixel[2] + pixel[3]) != 0)
                        rightmost = std::max(rightmost, x);
                }
            }
            const auto advance = static_cast<std::uint8_t>(spacing + rightmost);
            font->widths[i] = static_cast<std::uint8_t>((advance <= spacing ? space_width : advance) + 1);
        }
        if (kerned) {
            const unsigned char excluded[] = "+-*/\\#~!\"\xa7$%%&/()= ";
            std::array<bool, 256> eligible{};
            eligible.fill(true);
            for (const auto* p = excluded; *p; ++p) eligible[font->header[0x20 + *p]] = false;
            for (std::uint32_t i = 0; i < glyph_count; ++i) {
                const auto width = static_cast<std::uint8_t>(font->widths[i] - 1);
                if (width >= glyph_width) sgl::unimplemented("sgl_MemoryFont_CreateKerning glyph width");
                for (int y = 0; y < glyph_height; ++y) {
                    const auto* row = texture.image.pixels.data() + static_cast<std::size_t>(y) * texture.image.pitch +
                        static_cast<std::size_t>(i) * glyph_width * 4;
                    auto left = width, right = width;
                    for (unsigned x = 0; x <= width; ++x) {
                        const auto* pixel = row + x * 4;
                        if (pixel[0] || pixel[1] || pixel[2] || pixel[3]) {
                            left = std::min(left, static_cast<std::uint8_t>(x));
                            right = std::min(right, static_cast<std::uint8_t>(width - x));
                        }
                    }
                    const auto index = (static_cast<std::size_t>(i) * glyph_height + y) * 2;
                    edges[index] = left; edges[index + 1] = right;
                }
            }
            for (std::uint32_t a = 0; a < glyph_count; ++a) for (std::uint32_t b = 0; b < glyph_count; ++b) {
                const auto width = static_cast<std::uint8_t>(font->widths[a] - 1);
                int advance = width + spacing;
                if (eligible[a] && eligible[b]) {
                    auto overlap = width;
                    for (int y = 0; y < glyph_height; ++y) {
                        const auto left = edges[(static_cast<std::size_t>(b) * glyph_height + y) * 2];
                        const auto right = edges[(static_cast<std::size_t>(a) * glyph_height + y) * 2 + 1];
                        overlap = std::min(overlap, static_cast<std::uint8_t>(left + right));
                    }
                    advance = spacing - overlap + width + 1;
                }
                font->kerning[a * glyph_count + b] = static_cast<std::uint8_t>(advance);
            }
        }
        unlock_texture(texture);
        auto put = [&font](std::size_t offset, const auto& value) { std::memcpy(font->header.data() + offset, &value, sizeof(value)); };
        const auto* widths = font->widths.data();
        const auto* kerning = kerned ? font->kerning.data() : nullptr;
        put(4, glyph_width); put(8, glyph_height); put(0xc, fixed_width); put(0x10, glyph_count);
        put(0x14, widths); put(0x18, kerning); put(0x1c, texture_handle); put(0x120, spacing); put(0x124, space_width);
        void* handle = font->header.data();
        fonts.emplace(handle, std::move(font));
        return handle;
    } catch (const std::bad_alloc&) { return nullptr; }
}

extern "C" void* __cdecl sgl_MemoryFont_CreateProportional(const unsigned char* characters,
    void* texture_handle, int glyph_width, int glyph_height, int spacing, int space_width) {
    return create_font(characters, texture_handle, glyph_width, glyph_height, spacing, space_width, false, -1);
}

extern "C" void* __cdecl sgl_MemoryFont_CreateKerning(const unsigned char* characters,
    void* texture_handle, int glyph_width, int glyph_height, int spacing, int space_width) {
    return create_font(characters, texture_handle, glyph_width, glyph_height, spacing, space_width, true, -1);
}

extern "C" void* __cdecl sgl_MemoryFont_CreateFixed(const unsigned char* characters,
    void* texture_handle, int glyph_width, int glyph_height, int advance) {
    if (advance < 0) sgl::unimplemented("sgl_MemoryFont_CreateFixed advance");
    return create_font(characters, texture_handle, glyph_width, glyph_height, 0, 0, false, advance);
}

extern "C" void __cdecl sgl_FreeFont(void* handle) {
    if (!handle) return;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto entry = fonts.find(handle);
    if (entry == fonts.end()) sgl::unimplemented("sgl_FreeFont object");
    fonts.erase(entry);
}

extern "C" int __cdecl sgl_StrLen(const unsigned char* text) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (!*text || !active_font) return 0;
    std::uint32_t glyph_count;
    const std::uint8_t* widths;
    const std::uint8_t* kerning;
    std::memcpy(&glyph_count, active_font + 0x10, sizeof(glyph_count));
    std::memcpy(&widths, active_font + 0x14, sizeof(widths));
    std::memcpy(&kerning, active_font + 0x18, sizeof(kerning));
    int length = 0;
    do {
        const auto glyph = active_font[0x20 + *text++];
        const auto next = active_font[0x20 + *text];
        length += kerning && next ? kerning[glyph * glyph_count + next] : widths[glyph];
    } while (*text);
    return length;
}

extern "C" std::uint32_t __cdecl sgl_SetConstantColor(std::uint32_t color) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto previous = constant_color;
    constant_color = color;
    return previous;
}

extern "C" std::uint32_t __cdecl sgl_SetBlendMode(std::uint32_t mode) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto previous = active_blend_mode;
    active_blend_mode = static_cast<std::uint8_t>(mode);
    return previous;
}

// Flag 1 clears the whole destination, ignoring the clip and the blend mode; the other flags do nothing. As measured
// against the original, an opaque color replaces the pixels and any other color is blended by its alpha.
extern "C" void __cdecl sgl_ClearBuffer(std::uint32_t flags, std::uint32_t color, float) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (!(flags & 1)) return;
    const auto entry = textures.find(render_destination);
    if (entry == textures.end() || entry->second->image.format == sgl::ImageFormat::Indexed8) return;
    if (screen && render_destination == screen->display) return;
    auto& image = entry->second->image;
    const auto alpha = static_cast<unsigned>(color >> 24);
    if (alpha != 0xff && FAILED(surface_renderer.synchronize_to_cpu(render_destination, image)))
        sgl::unimplemented("sgl_ClearBuffer D3D11 synchronization");
    const auto pixel_size = sgl::bytes_per_pixel(image.format);
    const std::array<unsigned, 3> channels = {color & 0xffu, (color >> 8) & 0xffu, (color >> 16) & 0xffu};
    for (std::uint32_t y = 0; y < image.height; ++y) {
        auto* pixel = image.pixels.data() + static_cast<std::size_t>(y) * image.pitch;
        for (std::uint32_t x = 0; x < image.width; ++x, pixel += pixel_size) {
            for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                pixel[channel] = static_cast<std::uint8_t>(alpha == 0xff ? channels[channel] :
                    ((channels[channel] * alpha) >> 8) + ((pixel[channel] * (255u - alpha)) >> 8));
            }
            if (pixel_size == 4) pixel[3] = 0xff;
        }
    }
    surface_renderer.mark_cpu_modified(render_destination);
}

// Fills within the active clipper's rectangles, or within only_clip (logical, inside the destination) when given.
static void fill_solid_rect(int x, int y, int width, int height, const RECT* only_clip) {
    const auto entry = textures.find(render_destination);
    if (entry == textures.end()) return;
    if (width <= 0 || height <= 0) return;

    auto& image = entry->second->image;
    const auto pixel_size = sgl::bytes_per_pixel(image.format);
    const auto minimum_pitch = static_cast<std::uint64_t>(image.width) * pixel_size;
    if ((image.format != sgl::ImageFormat::Bgra32 && image.format != sgl::ImageFormat::Bgr24) ||
        image.pitch < minimum_pitch ||
        static_cast<std::uint64_t>(image.pitch) * image.height > image.pixels.size()) {
        sgl::unimplemented("sgl_FillSolidRect destination format");
    }

    const double right = static_cast<double>(x) + width;
    const double bottom = static_cast<double>(y) + height;
    sgl::Rect rectangle{static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(right), static_cast<float>(bottom)};
    sgl::ScreenTransform transform;
    const bool destination_is_screen = make_screen_transform(render_destination, image, transform);
    if (destination_is_screen) rectangle = transform.map(rectangle);

    // Pixels whose centers lie inside the mapped rectangle, as the GPU path rasterizes it and as clips are mapped.
    const auto pixel_bound = [](double value, std::uint32_t extent) {
        return static_cast<LONG>(std::clamp(sgl::ScreenTransform::pixel_edge(value), 0.0, static_cast<double>(extent)));
    };
    RECT fill{pixel_bound(rectangle.left, image.width), pixel_bound(rectangle.top, image.height),
        pixel_bound(rectangle.right, image.width), pixel_bound(rectangle.bottom, image.height)};
    if (!nonempty(fill)) return;

    const auto alpha = static_cast<std::uint8_t>(constant_color >> 24);
    // A surface whose current pixels are on the GPU is filled there: the CPU path would first read back the whole
    // surface and then upload it again. The GPU blend modes reproduce the integer formulas of the CPU path below.
    if (surface_renderer.gpu_holds_newer_pixels(render_destination)) {
        // Multiply also scales the destination alpha by the color's alpha.
        const std::array<std::uint8_t, 4> fill_pixel_bytes{static_cast<std::uint8_t>(constant_color),
            static_cast<std::uint8_t>(constant_color >> 8), static_cast<std::uint8_t>(constant_color >> 16),
            active_blend_mode == 2 ? alpha : std::uint8_t{0xff}};
        // One 1x1 source per fill pixel, uploaded once: rewriting a shared source uploaded it before every fill, and
        // each upload ends the renderer's draw queue (about 1,300 fills per frame on a busy Hanse map).
        static std::unordered_map<std::uint32_t, std::unique_ptr<sgl::DecodedImage>> fill_sources;
        std::uint32_t fill_key = 0;
        std::memcpy(&fill_key, fill_pixel_bytes.data(), sizeof(fill_key));
        // Fills mostly repeat the previous color, so the last source is kept at hand.
        static std::uint32_t last_fill_key = 0;
        static sgl::DecodedImage* last_fill_source = nullptr;
        if (!last_fill_source || last_fill_key != fill_key) {
            auto found_source = fill_sources.find(fill_key);
            if (found_source == fill_sources.end()) {
                if (fill_sources.size() >= 1024) {
                    for (auto& cached : fill_sources) surface_renderer.forget(cached.second.get());
                    fill_sources.clear();
                }
                try {
                    auto source = std::make_unique<sgl::DecodedImage>();
                    source->width = source->height = 1;
                    source->pitch = 4;
                    source->format = sgl::ImageFormat::Bgra32;
                    source->pixels.assign(fill_pixel_bytes.begin(), fill_pixel_bytes.end());
                    found_source = fill_sources.emplace(fill_key, std::move(source)).first;
                } catch (const std::bad_alloc&) { sgl::unimplemented("sgl_FillSolidRect source allocation"); }
            }
            last_fill_key = fill_key;
            last_fill_source = found_source->second.get();
        }
        sgl::DecodedImage* const fill_source = last_fill_source;
        const auto blend = active_blend_mode == 2 ? sgl::BlendMode::Multiply :
            active_blend_mode == 3 ? sgl::BlendMode::Additive :
            alpha == 0xff ? sgl::BlendMode::Opaque : sgl::BlendMode::FillAlpha;
        const std::uint32_t modulation = blend == sgl::BlendMode::FillAlpha
            ? (static_cast<std::uint32_t>(alpha) << 24) | 0x00ffffffu : 0xffffffffu;
        const sgl::Rect target_rect{static_cast<float>(fill.left), static_cast<float>(fill.top),
            static_cast<float>(fill.right), static_cast<float>(fill.bottom)};
        sgl::ScreenTransform clip_transform;
        const bool clip_mapped = make_screen_transform(render_destination, image, clip_transform);
        const auto fill_clip = [&](const RECT& clip) {
            RECT region{std::max(fill.left, clip.left), std::max(fill.top, clip.top),
                std::min(fill.right, clip.right), std::min(fill.bottom, clip.bottom)};
            if (!intersect_with(region, {0, 0, static_cast<LONG>(image.width), static_cast<LONG>(image.height)})) return;
            const char* failure_stage = nullptr;
            if (FAILED(surface_renderer.draw(fill_source, *fill_source, render_destination, image, {0, 0, 1, 1},
                target_rect, region, std::nullopt, blend, &failure_stage, modulation)))
                sgl::unimplemented(failure_stage ? failure_stage : "sgl_FillSolidRect D3D11 fill");
        };
        if (only_clip) {
            RECT mapped{};
            if (map_draw_clip(*only_clip, clip_mapped, clip_transform, mapped)) fill_clip(mapped);
        } else {
            for (const RECT& clip : cached_draw_clips(image, clip_mapped, clip_transform)) fill_clip(clip);
        }
        return;
    }
    if (FAILED(surface_renderer.synchronize_to_cpu(render_destination, image)))
        sgl::unimplemented("sgl_FillSolidRect D3D11 synchronization");

    bool modified = false;
    const std::array<std::uint8_t, 3> color = {
        static_cast<std::uint8_t>(constant_color),
        static_cast<std::uint8_t>(constant_color >> 8),
        static_cast<std::uint8_t>(constant_color >> 16)
    };
    const auto inverse_alpha = static_cast<std::uint8_t>(255 - alpha);
    // Blend mode 2 multiplies the color channels and the alpha; 3 adds the color, ignoring its alpha; the other
    // modes blend the color by its alpha.
    const auto fill_pixel = [&](std::uint8_t* pixel) {
        if (active_blend_mode == 2) {
            for (std::size_t channel = 0; channel < color.size(); ++channel)
                pixel[channel] = static_cast<std::uint8_t>((static_cast<unsigned>(pixel[channel]) * color[channel]) >> 8);
            if (pixel_size == 4) pixel[3] = static_cast<std::uint8_t>((static_cast<unsigned>(pixel[3]) * alpha) >> 8);
            return;
        } else if (active_blend_mode == 3) {
            for (std::size_t channel = 0; channel < color.size(); ++channel)
                pixel[channel] = static_cast<std::uint8_t>(std::min<unsigned>(255, pixel[channel] + color[channel]));
        } else if (alpha == 0xff) {
            std::memcpy(pixel, color.data(), color.size());
        } else {
            for (std::size_t channel = 0; channel < color.size(); ++channel) {
                const auto source = (static_cast<unsigned>(color[channel]) * alpha) >> 8;
                const auto destination = (static_cast<unsigned>(pixel[channel]) * inverse_alpha) >> 8;
                pixel[channel] = static_cast<std::uint8_t>(source + destination);
            }
        }
        if (pixel_size == 4) pixel[3] = 0xff;
    };
    const auto fill_clip = [&](RECT clip) {
        if (destination_is_screen) {
            RECT mapped_clip{};
            if (!transform.map_clip(clip, mapped_clip)) return;
            clip = mapped_clip;
        }
        RECT region{std::max(fill.left, clip.left), std::max(fill.top, clip.top),
            std::min(fill.right, clip.right), std::min(fill.bottom, clip.bottom)};
        if (!intersect_with(region, {0, 0, static_cast<LONG>(image.width), static_cast<LONG>(image.height)})) return;
        modified = true;

        for (LONG row = region.top; row < region.bottom; ++row) {
            auto* destination = image.pixels.data() + static_cast<std::size_t>(row) * image.pitch +
                static_cast<std::size_t>(region.left) * pixel_size;
            for (LONG column = region.left; column < region.right; ++column) {
                fill_pixel(destination);
                destination += pixel_size;
            }
        }
    };
    if (only_clip) fill_clip(*only_clip);
    else for (const RECT& clip : draw_clip_rectangles(image)) fill_clip(clip);
    if (modified) surface_renderer.mark_cpu_modified(render_destination);
}

extern "C" void __cdecl sgl_FillSolidRect(int x, int y, int width, int height) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    fill_solid_rect(x, y, width, height, nullptr);
}

// The original calls the render destination's line method (vtable slot 0x58). For surfaces in memory, the offscreen
// included, it rounds the end points and draws the line between them, both included, in the constant color, clipped
// by the active clipper. Patrician III draws its diagonal map lines (routes) with it through 0x4bd680 and Patrician II
// through its counterpart; horizontal and vertical ones go to sgl_FillSolidRect directly, as in the map editor
// (0x43a0c0). The rasterizer (0x91154, set up by 0x91490) walks the line from its left end point along the major axis
// in 32-bit fixed point: the minor-axis step is floor(minor * 2^32 / major) and the accumulator starts at one half.
// An exact half therefore rounds up where the step is exact and down where it was truncated. The line is drawn once per
// clip rectangle; where it enters a rectangle across the minor axis, the setup finds the first pixel with a strict
// comparison, so a pixel that reaches the entry row (column) at an exact half is left out of that rectangle. Each run
// of the line's pixels in one row (one column for steep lines) is filled as one rectangle.
extern "C" void __cdecl sgl_DrawLine(float x1, float y1, float x2, float y2) {
    constexpr float limit = 1 << 24;
    for (const float value : {x1, y1, x2, y2})
        if (!std::isfinite(value) || value < -limit || value > limit) return;
    long start_x = std::lrint(x1), start_y = std::lrint(y1), end_x = std::lrint(x2), end_y = std::lrint(y2);
    if (end_x < start_x) {
        std::swap(start_x, end_x);
        std::swap(start_y, end_y);
    }
    const long dx = end_x - start_x, dy = end_y - start_y;
    const bool steep = std::labs(dy) > dx;
    const long major = steep ? std::labs(dy) : dx, minor = steep ? dx : std::labs(dy);
    const long major_step = steep && dy < 0 ? -1 : 1, minor_step = !steep && dy < 0 ? -1 : 1;
    const long major_start = steep ? start_y : start_x, minor_start = steep ? start_x : start_y;
    const std::uint64_t fraction_step = major ? (static_cast<std::uint64_t>(minor) << 32) / major : 0;

    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto destination = textures.find(render_destination);
    if (destination == textures.end()) return;
    std::vector<RECT> clips;
    collect_draw_clips(destination->second->image, clips);
    for (const RECT& clip : clips) {
        const long major_low = steep ? clip.top : clip.left, major_high = (steep ? clip.bottom : clip.right) - 1;
        const long minor_low = steep ? clip.left : clip.top, minor_high = (steep ? clip.right : clip.bottom) - 1;
        // Steps whose major coordinate lies in the rectangle.
        const long first = std::max(0L, major_step > 0 ? major_low - major_start : major_start - major_high);
        const long last = std::min(major, major_step > 0 ? major_high - major_start : major_start - major_low);
        // Distance from the start to the entry row (column) when the start lies before it on the minor axis.
        const long entry = minor_step > 0 ? minor_low - minor_start : minor_start - minor_high;
        RECT run{};
        bool open = false;
        const auto flush = [&] {
            if (open) fill_solid_rect(run.left, run.top, run.right - run.left + 1, run.bottom - run.top + 1, &clip);
            open = false;
        };
        for (long step = first; step <= last; ++step) {
            const std::uint64_t position = static_cast<std::uint64_t>(step) * fraction_step + (1ull << 31);
            const auto offset = static_cast<long>(position >> 32);
            const long across = minor_start + offset * minor_step;
            if (across < minor_low || across > minor_high ||
                (offset == entry && step > first && static_cast<std::uint32_t>(position) == 0)) {
                flush();
                continue;
            }
            const long along = major_start + step * major_step;
            const long x = steep ? across : along, y = steep ? along : across;
            if (open && (steep ? x == run.left : y == run.top)) {
                run = {std::min(run.left, x), std::min(run.top, y), std::max(run.right, x), std::max(run.bottom, y)};
            } else {
                flush();
                run = {x, y, x, y};
                open = true;
            }
        }
        flush();
    }
}

// The main menu background, drawn only while no game runs.
static bool is_main_menu_background(const std::string& name) noexcept {
    constexpr std::string_view main_menu = "images/options/hauptmenu.jpg";
    if (name.size() != main_menu.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char character = name[i] == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        if (character != main_menu[i]) return false;
    }
    return true;
}

// The 800x600 menus fill only the middle of the display; the bars beside (or above and below) it keep the last
// full-size frame. The in-game menus are meant to show the game there, but after a game ended the main menu and the
// menus opened from it kept its pictures. The main menu therefore clears the bars to black, as they are after startup.
static void clear_display_border() {
    if (!screen) return;
    const auto entry = textures.find(screen->display);
    if (entry == textures.end()) return;
    auto& image = entry->second->image;
    sgl::ScreenTransform transform;
    if (!make_screen_transform(screen->display, image, transform)) return;
    std::array<RECT, 4> bars{};
    const std::size_t count = transform.border_rectangles(bars);
    static sgl::DecodedImage black = [] {
        sgl::DecodedImage pixel;
        pixel.width = pixel.height = 1;
        pixel.pitch = 4;
        pixel.format = sgl::ImageFormat::Bgra32;
        pixel.pixels = {0, 0, 0, 0xff};
        return pixel;
    }();
    for (std::size_t i = 0; i < count; ++i) {
        const RECT& bar = bars[i];
        const sgl::Rect target_rect{static_cast<float>(bar.left), static_cast<float>(bar.top),
            static_cast<float>(bar.right), static_cast<float>(bar.bottom)};
        const char* failure_stage = nullptr;
        if (FAILED(surface_renderer.draw(&black, black, screen->display, image, {0, 0, 1, 1}, target_rect, bar,
            std::nullopt, sgl::BlendMode::Opaque, &failure_stage)))
            sgl::unimplemented(failure_stage ? failure_stage : "display border clear");
    }
}

static void draw_bitmap(int sx, int sy, int dx, int dy, int width, int height, int dw, int dh,
    bool smooth_menu_text = false) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    Texture* const source_texture = find_texture_cached(render_source_lookup, render_source);
    Texture* const target_texture = find_texture_cached(render_destination_lookup, render_destination);
    // Without a source or destination (an indexed destination is never selected) the original draws nothing.
    if (!source_texture || !target_texture) return;
    // The original draws nothing in clipping modes other than 0 to 3.
    if (clipping_mode < 0 || clipping_mode > 3) return;
    // Like the original, an empty or negative size draws nothing. A negative source origin makes the original read
    // memory before the pixels, which has no defined result; the part inside the image is drawn.
    if (width <= 0 || height <= 0 || dw <= 0 || dh <= 0) return;
    if (sx < 0) {
        const auto cut = static_cast<int>(static_cast<std::int64_t>(-static_cast<std::int64_t>(sx)) * dw / width);
        if (-static_cast<std::int64_t>(sx) >= width) return;
        width += sx;
        dx += cut;
        dw -= cut;
        sx = 0;
    }
    if (sy < 0) {
        const auto cut = static_cast<int>(static_cast<std::int64_t>(-static_cast<std::int64_t>(sy)) * dh / height);
        if (-static_cast<std::int64_t>(sy) >= height) return;
        height += sy;
        dy += cut;
        dh -= cut;
        sy = 0;
    }
    if (dw <= 0 || dh <= 0) return;
    // Full-surface copies (the fast-forward map and full-screen backgrounds) ignore the clip region, and the
    // fast-forward overlay maps destinations differently; every other draw outside the clips draws nothing.
    if (!fast_forward_overlay && !(sx == 0 && sy == 0 && width == dw && height == dh && width >= 800) &&
        draw_outside_clip(*target_texture, dx, dy, dw, dh)) return;
    // The name comparison comes last: almost every draw fails the integer conditions first.
    const bool full_map = sx == 0 && sy == 0 && width == dw && height == dh &&
        ((width == 1280 && height == 1024) ||
            (width == static_cast<int>(source_texture->image.width) &&
                height == static_cast<int>(source_texture->image.height))) &&
        source_texture->image.width == target_texture->image.width &&
        source_texture->image.height == target_texture->image.height && target_texture->image.width > 1280 &&
        is_screen_texture(render_destination) && is_fast_forward_map(source_texture->name.c_str());
    // A source rectangle beyond the image makes the original read memory after the pixels, which has no defined
    // result. The part inside the image is drawn, with the destination shortened in proportion.
    if (!full_map && (static_cast<std::uint64_t>(sx) + width > source_texture->image.width ||
        static_cast<std::uint64_t>(sy) + height > source_texture->image.height)) {
        const auto image_width = static_cast<std::int64_t>(source_texture->image.width);
        const auto image_height = static_cast<std::int64_t>(source_texture->image.height);
        if (sx >= image_width || sy >= image_height) return;
        const auto inside_width = static_cast<int>(std::min<std::int64_t>(width, image_width - sx));
        const auto inside_height = static_cast<int>(std::min<std::int64_t>(height, image_height - sy));
        dw = static_cast<int>(static_cast<std::int64_t>(dw) * inside_width / width);
        dh = static_cast<int>(static_cast<std::int64_t>(dh) * inside_height / height);
        width = inside_width;
        height = inside_height;
        if (dw == 0 || dh == 0) return;
    }
    auto& src = *source_texture;
    auto& dst = *target_texture;
    const auto flags = src.header[0x34];
    const auto blend = src.header[0x37];
    const bool indexed = src.image.format == sgl::ImageFormat::Indexed8;
    const auto color_flags = flags & 7;
    // The original library's behavior, measured for every source format, color flags value, blend byte and constant
    // color (tests/blend_matrix_oracle.cpp): blend byte 1 alpha-blends with source alpha times the constant color when
    // the color flags are 3 or the color is not opaque white, and otherwise copies the source without color-keyed
    // pixels; 2 multiplies and 3 adds, both scaled by the constant color when its RGB is not white; 5 is a doubled
    // multiply for BGRA sources. Every other blend byte, including all values above 7, copies the source unchanged,
    // ignoring the constant color and the color key.
    const bool white_rgb = (constant_color & 0x00ffffffu) == 0x00ffffffu;
    const bool alpha_blend = blend == 1 && (color_flags == 3 || constant_color != 0xffffffffu);
    const bool keyed_copy = blend == 1 && !alpha_blend;
    const bool multiply = blend == 2 && white_rgb;
    const bool multiply_modulated = blend == 2 && !white_rgb;
    const bool multiply2 = !indexed && blend == 5;
    const bool additive = blend == 3 && white_rgb;
    const bool additive_modulated = blend == 3 && !white_rgb;
    const bool stretched = width != dw || height != dh;
    const auto blend_mode = multiply_modulated ? sgl::BlendMode::MultiplyModulated :
        multiply ? sgl::BlendMode::Multiply :
        multiply2 ? sgl::BlendMode::Multiply2 :
        additive_modulated ? sgl::BlendMode::AdditiveModulated :
        additive ? sgl::BlendMode::Additive :
        alpha_blend ? sgl::BlendMode::Alpha : sgl::BlendMode::Opaque;
    // Texture flag 0x10 makes the original filter enlarged BGRA sources bilinearly for copies without color key and
    // for the multiplying modes without a constant color; the other modes sample the nearest pixel.
    const bool filtered_mode = (blend_mode == sgl::BlendMode::Opaque && !(keyed_copy && src.keyed)) ||
        blend_mode == sgl::BlendMode::Multiply || blend_mode == sgl::BlendMode::Multiply2;
    auto sampling = sgl::Sampling::SglFixedPoint;
    if (smooth_menu_text && !indexed && !src.keyed && blend_mode == sgl::BlendMode::Alpha)
        sampling = sgl::Sampling::PresentSharp;
    else if (!indexed && filtered_mode && stretched && (flags & 0x10) != 0 && dw >= width && dh >= height)
        sampling = sgl::Sampling::SglBilinear;

    if (blend_mode == sgl::BlendMode::Opaque && !src.keyed && is_screen_texture(render_destination) &&
        sx == 0 && sy == 0 && dx == 0 && dy == 0 && width == dw && height == dh &&
        width == view_state.logical_width && height == view_state.logical_height &&
        is_main_menu_background(src.name))
        clear_display_border();

    if (full_map) fast_forward_overlay = true;
    else if (fast_forward_overlay && _stricmp(src.name.c_str(), "images/Banner_klein.tga") == 0) fast_forward_overlay = false;

    sgl::ScreenTransform destination_transform;
    const bool destination_mapped = make_screen_transform(render_destination, dst.image, destination_transform);
    static std::vector<RECT> full_clip(1);
    full_clip[0] = {0, 0, static_cast<LONG>(dst.image.width), static_cast<LONG>(dst.image.height)};
    // Logical destination bounds, widened for the rounding of mapped clips.
    const LONG margin = draw_clip_margin(dst.image);
    const RECT logical_bounds{dx - margin, dy - margin, dx + dw + margin, dy + dh + margin};
    const auto& clips = full_map ? full_clip :
        cached_draw_clips(dst.image, destination_mapped, destination_transform,
            fast_forward_overlay ? nullptr : &logical_bounds);
    if (clips.empty()) return;
    sgl::Rect src_rect = full_map
        ? sgl::Rect{0.0f, 0.0f, static_cast<float>(src.image.width), static_cast<float>(src.image.height)}
        : sgl::Rect{static_cast<float>(sx), static_cast<float>(sy),
            static_cast<float>(sx) + width, static_cast<float>(sy) + height};
    const sgl::Rect dst_rect{static_cast<float>(dx), static_cast<float>(dy),
        static_cast<float>(dx) + dw, static_cast<float>(dy) + dh};
    sgl::ScreenTransform transform;
    if (!full_map && make_screen_transform(render_source, src.image, transform))
        src_rect = transform.map(src_rect);
    auto mapped_destination = dst_rect;
    const bool destination_is_screen = make_screen_transform(render_destination, dst.image, transform);
    if (full_map) {
        mapped_destination = {0.0f, 0.0f, static_cast<float>(dst.image.width), static_cast<float>(dst.image.height)};
    } else if (destination_is_screen) {
        mapped_destination = transform.map(dst_rect);
    }
    if (fast_forward_overlay && !full_map && destination_is_screen &&
        !sgl::refresh_accelerated_map_layout()) {
        const float x = (static_cast<float>(dx) + dw * 0.5f) * dst.image.width / 1280.0f - dw * 0.5f;
        const float y = (static_cast<float>(dy) + dh * 0.5f) * dst.image.height / 1024.0f - dh * 0.5f;
        mapped_destination = {x, y, x + dw, y + dh};
    }
    if (!lock_texture(src, nullptr, 1, false, LockIntent::GpuResource))
        sgl::unimplemented("sgl_DrawBitmapRect source lock");
    const auto key = src.keyed && (keyed_copy || (indexed && blend_mode == sgl::BlendMode::Alpha))
        ? std::optional<std::uint32_t>(src.key) : std::nullopt;
    const std::uint32_t modulation = blend_mode == sgl::BlendMode::Opaque ? 0xffffffffu : constant_color;
    for (const RECT& clip : clips) {
        if (mapped_destination.left >= static_cast<float>(clip.right) ||
            mapped_destination.top >= static_cast<float>(clip.bottom) ||
            mapped_destination.right <= static_cast<float>(clip.left) ||
            mapped_destination.bottom <= static_cast<float>(clip.top)) continue;
        const char* failure_stage = nullptr;
        const HRESULT result = surface_renderer.draw(src.header.data(), src.image, render_destination,
            dst.image, src_rect, mapped_destination, clip, key, blend_mode, &failure_stage,
            modulation, sampling);
        if (FAILED(result)) {
            unlock_texture(src);
            char message[256] = {};
            std::snprintf(message, sizeof(message),
                "sgl_DrawBitmapRect %s hr=0x%08lx src=%.2f,%.2f-%.2f,%.2f dst=%.2f,%.2f-%.2f,%.2f",
                failure_stage ? failure_stage : "D3D11", static_cast<unsigned long>(result),
                src_rect.left, src_rect.top, src_rect.right, src_rect.bottom,
                mapped_destination.left, mapped_destination.top, mapped_destination.right, mapped_destination.bottom);
            sgl::unimplemented(message);
        }
    }
    unlock_texture(src);
}

extern "C" void __cdecl sgl_DrawBitmapRect(int sx, int sy, int dx, int dy, int width, int height) {
    draw_bitmap(sx, sy, dx, dy, width, height, width, height);
}

extern "C" void __cdecl sgl_StretchBitmapRect(int sx, int sy, int dx, int dy,
    int width, int height, int dw, int dh) {
    draw_bitmap(sx, sy, dx, dy, width, height, dw, dh);
}

extern "C" int __cdecl sgl_DrawBitmapRectWithAlphaMask(std::uint32_t mask_handle,
    int sx, int sy, int dx, int dy, int width, int height) {
    // The original resolves 0 and the negative handles to a screen surface, which never matches the indexed source in
    // size and pitch, and then draws nothing. The city fog draws with mask 0 when its sprite has no fogmap.
    if (static_cast<std::int32_t>(mask_handle) <= 0) return 1;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    const auto source = textures.find(render_source);
    const auto target = textures.find(render_destination);
    const auto mask_id = reinterpret_cast<void*>(static_cast<std::uintptr_t>(mask_handle));
    const auto mask = textures.find(mask_id);
    if (source == textures.end() || target == textures.end()) return 1;
    if (mask == textures.end()) sgl::unimplemented("sgl_DrawBitmapRectWithAlphaMask mask handle");
    if (mask_id == render_destination) sgl::unimplemented("sgl_DrawBitmapRectWithAlphaMask mask aliases destination");
    if (clipping_mode < 0 || clipping_mode > 3) return 1;
    if (width <= 0 || height <= 0) return 1;
    if (sx < 0) { if (-static_cast<std::int64_t>(sx) >= width) return 1; width += sx; dx -= sx; sx = 0; }
    if (sy < 0) { if (-static_cast<std::int64_t>(sy) >= height) return 1; height += sy; dy -= sy; sy = 0; }

    auto& src = *source->second;
    auto& alpha = *mask->second;
    auto& dst = *target->second;
    if (src.image.format != sgl::ImageFormat::Indexed8 || alpha.image.format != sgl::ImageFormat::Indexed8 ||
        src.image.width != alpha.image.width || src.image.height != alpha.image.height ||
        src.image.pitch != alpha.image.pitch ||
        dst.image.format == sgl::ImageFormat::Indexed8) {
        char message[256] = {};
        std::snprintf(message, sizeof(message),
            "sgl_DrawBitmapRectWithAlphaMask texture contract: src=%u %ux%u p%u pal%zu; mask=%u %ux%u p%u pal%zu; dst=%u; rect=%d,%d %dx%d",
            static_cast<unsigned>(src.image.format), src.image.width, src.image.height, src.image.pitch,
            src.image.palette.size(), static_cast<unsigned>(alpha.image.format), alpha.image.width,
            alpha.image.height, alpha.image.pitch, alpha.image.palette.size(),
            static_cast<unsigned>(dst.image.format), sx, sy, width, height);
        sgl::unimplemented(message);
    }
    // As with sgl_DrawBitmapRect, only the part of the source rectangle inside the image is drawn.
    if (static_cast<std::uint64_t>(sx) >= src.image.width || static_cast<std::uint64_t>(sy) >= src.image.height) return 1;
    width = static_cast<int>(std::min<std::uint64_t>(width, src.image.width - static_cast<std::uint64_t>(sx)));
    height = static_cast<int>(std::min<std::uint64_t>(height, src.image.height - static_cast<std::uint64_t>(sy)));

    sgl::ScreenTransform destination_transform;
    const bool destination_mapped = make_screen_transform(render_destination, dst.image, destination_transform);
    const auto& clips = cached_draw_clips(dst.image, destination_mapped, destination_transform);
    if (clips.empty()) return 1;
    sgl::Rect source_rect{static_cast<float>(sx), static_cast<float>(sy),
        static_cast<float>(sx) + width, static_cast<float>(sy) + height};
    const sgl::Rect target_rect{static_cast<float>(dx), static_cast<float>(dy),
        static_cast<float>(dx) + width, static_cast<float>(dy) + height};
    sgl::ScreenTransform transform;
    if (make_screen_transform(render_source, src.image, transform)) source_rect = transform.map(source_rect);
    auto mapped_target = target_rect;
    const bool destination_is_screen = make_screen_transform(render_destination, dst.image, transform);
    if (destination_is_screen) mapped_target = transform.map(target_rect);

    if (!lock_texture(src, nullptr, 1, false, LockIntent::GpuResource))
        sgl::unimplemented("sgl_DrawBitmapRectWithAlphaMask source lock");
    const bool mask_is_source = mask_id == render_source;
    if (!mask_is_source && !lock_texture(alpha, nullptr, 1, false, LockIntent::GpuResource)) {
        unlock_texture(src);
        sgl::unimplemented("sgl_DrawBitmapRectWithAlphaMask mask lock");
    }
    for (const RECT& clip : clips) {
        if (mapped_target.left >= static_cast<float>(clip.right) ||
            mapped_target.top >= static_cast<float>(clip.bottom) ||
            mapped_target.right <= static_cast<float>(clip.left) ||
            mapped_target.bottom <= static_cast<float>(clip.top)) continue;
        const char* failure_stage = nullptr;
        const HRESULT result = surface_renderer.draw_masked(render_source, src.image, mask_id,
            alpha.image, render_destination, dst.image, source_rect, mapped_target, clip,
            constant_color, &failure_stage);
        if (FAILED(result)) {
            if (!mask_is_source) unlock_texture(alpha);
            unlock_texture(src);
            char message[256] = {};
            std::snprintf(message, sizeof(message),
                "sgl_DrawBitmapRectWithAlphaMask %s hr=0x%08lx src=%.2f,%.2f-%.2f,%.2f dst=%.2f,%.2f-%.2f,%.2f",
                failure_stage ? failure_stage : "D3D11", static_cast<unsigned long>(result),
                source_rect.left, source_rect.top, source_rect.right, source_rect.bottom,
                mapped_target.left, mapped_target.top, mapped_target.right, mapped_target.bottom);
            sgl::unimplemented(message);
        }
    }
    if (!mask_is_source) unlock_texture(alpha);
    unlock_texture(src);
    return 1;
}

extern "C" void __cdecl sgl_SetTextMode(int mode) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    text_mode = mode;
}

extern "C" void __cdecl sgl_DrawText(int x, int y, const unsigned char* text) {
    if (!*text) return;
    const std::uint8_t* font;
    int mode, old_clipping;
    std::array<std::int32_t, 4> old_clip;
    // Reused between calls: the Hanse map draws about a thousand labels per frame under a large clipper.
    thread_local std::vector<RECT> regions;
    regions.clear();
    {
        std::lock_guard<TexturesMutex> lock(textures_mutex);
        font = active_font;
        mode = text_mode;
        // Text uses the clipping mode as set: as measured against the original, modes other than 1 to 3 (negative
        // ones included) clip text to the destination only.
        old_clipping = requested_clipping_mode;
        old_clip = clip_rectangle;
        if (old_clipping == 2 || old_clipping == 3) {
            try {
                regions = get_active_clipper()->rectangles;
            } catch (const std::bad_alloc&) { sgl::unimplemented("sgl_DrawText region allocation"); }
            catch (const std::length_error&) { sgl::unimplemented("sgl_DrawText region allocation"); }
        }
    }
    if (!font) return;
    const int length = sgl_StrLen(text);
    int glyph_width, glyph_height;
    std::uint32_t glyph_count;
    const std::uint8_t *widths, *kerning;
    void* texture;
    std::memcpy(&glyph_width, font + 4, sizeof(glyph_width));
    std::memcpy(&glyph_height, font + 8, sizeof(glyph_height));
    std::memcpy(&glyph_count, font + 0x10, sizeof(glyph_count));
    std::memcpy(&widths, font + 0x14, sizeof(widths));
    std::memcpy(&kerning, font + 0x18, sizeof(kerning));
    std::memcpy(&texture, font + 0x1c, sizeof(texture));
    if (mode == 2) x -= length;
    else if (mode == 3) x -= length / 2;
    std::array<std::int32_t, 4> bounds{x, y, x + length, y + glyph_height};
    if (old_clipping == 1 || old_clipping == 3) {
        bounds[0] = std::max(bounds[0], old_clip[0]); bounds[1] = std::max(bounds[1], old_clip[1]);
        bounds[2] = std::min(bounds[2], old_clip[2]); bounds[3] = std::min(bounds[3], old_clip[3]);
    }
    if (bounds[0] >= bounds[2] || bounds[1] >= bounds[3]) return;
    if (old_clipping != 2 && old_clipping != 3)
        regions.push_back({bounds[0], bounds[1], bounds[2], bounds[3]});
    if (regions.empty()) return;
    auto* previous = sgl_SetRenderSource(texture);
    sgl_Set2dClippingMode(1);
    for (const auto& region : regions) {
        const std::array<std::int32_t, 4> clip{std::max(bounds[0], static_cast<std::int32_t>(region.left)),
            std::max(bounds[1], static_cast<std::int32_t>(region.top)),
            std::min(bounds[2], static_cast<std::int32_t>(region.right)),
            std::min(bounds[3], static_cast<std::int32_t>(region.bottom))};
        if (clip[0] >= clip[2] || clip[1] >= clip[3]) continue;
        sgl_SetClipRect_r(clip.data());
        int position = x;
        const auto* current = text;
        while (*current) {
            const auto glyph = font[0x20 + *current++];
            const auto next = font[0x20 + *current];
            const auto width = widths[glyph];
            const auto advance = kerning && next ? kerning[glyph * glyph_count + next] : width;
            if (width) draw_bitmap(glyph * glyph_width, 0, position, y, width, glyph_height, width, glyph_height,
                view_state.logical_width == 800 && view_state.logical_height == 600 &&
                    is_screen_texture(render_destination));
            position += advance;
        }
    }
    sgl_SetClipRect_r(old_clip.data());
    sgl_Set2dClippingMode(old_clipping);
    sgl_SetRenderSource(previous);
}

extern "C" void __cdecl sgl_DrawText_Rect(int x, int y, const std::int32_t* rectangle,
    const unsigned char* text) {
    if (!text || !*text || !rectangle) return;

    const int previous_mode = sgl_Get2dClippingMode();
    std::array<std::int32_t, 4> previous_clip;
    sgl_GetClipRect_r(previous_clip.data());
    std::array<std::int32_t, 4> draw_clip;
    std::copy_n(rectangle, draw_clip.size(), draw_clip.begin());

    if (previous_mode == 1 || previous_mode == 3) {
        draw_clip[0] = std::max(draw_clip[0], previous_clip[0]);
        draw_clip[1] = std::max(draw_clip[1], previous_clip[1]);
        draw_clip[2] = std::min(draw_clip[2], previous_clip[2]);
        draw_clip[3] = std::min(draw_clip[3], previous_clip[3]);
        sgl_SetClipRect_r(draw_clip.data());
    } else if (previous_mode == 2) {
        sgl_Set2dClippingMode(3);
        sgl_SetClipRect_r(draw_clip.data());
    } else {
        sgl_Set2dClippingMode(1);
        sgl_SetClipRect_r(draw_clip.data());
    }

    sgl_DrawText(x, y, text);
    sgl_SetClipRect_r(previous_clip.data());
    sgl_Set2dClippingMode(previous_mode);
}

static void blit_bitmap(int sx, int sy, int dx, int dy, int width, int height, std::uint32_t flags) {
    const auto source = textures.find(render_source);
    const auto target = textures.find(render_destination);
    // As measured against the original: without a source or destination (an indexed destination is never
    // selected) nothing is copied, the flags make no difference, and an indexed source is copied through its
    // palette. A negative source origin makes the original read memory before the pixels; the part inside the
    // image is copied.
    if (source == textures.end() || target == textures.end()) return;
    static_cast<void>(flags);
    if (target->second->image.format == sgl::ImageFormat::Indexed8) return;
    if (sx < 0) { dx -= sx; width += sx; sx = 0; }
    if (sy < 0) { dy -= sy; height += sy; sy = 0; }
    if (width <= 0 || height <= 0) return;
    auto& src = *source->second;
    auto& dst = *target->second;
    const bool source_is_screen = is_screen_texture(render_source);
    const bool target_is_screen = is_screen_texture(render_destination);
    const auto [logical_width, logical_height] = screen_logical_size();
    const std::int64_t source_width = source_is_screen ? logical_width : src.image.width;
    const std::int64_t source_height = source_is_screen ? logical_height : src.image.height;
    const std::int64_t target_width = target_is_screen ? logical_width : dst.image.width;
    const std::int64_t target_height = target_is_screen ? logical_height : dst.image.height;
    const std::int64_t left = std::max<std::int64_t>(0, -static_cast<std::int64_t>(dx));
    const std::int64_t top = std::max<std::int64_t>(0, -static_cast<std::int64_t>(dy));
    const std::int64_t right = std::min({static_cast<std::int64_t>(width), source_width - sx, target_width - dx});
    const std::int64_t bottom = std::min({static_cast<std::int64_t>(height), source_height - sy, target_height - dy});
    if (left >= right || top >= bottom) return;
    if (render_source == render_destination && sx == dx && sy == dy) return;
    if (!lock_texture(src, nullptr, 1, false, LockIntent::GpuResource)) sgl::unimplemented("sgl_Blt source lock");
    sgl::Rect source_rect{static_cast<float>(sx + left), static_cast<float>(sy + top),
        static_cast<float>(sx + right), static_cast<float>(sy + bottom)};
    sgl::Rect target_rect{static_cast<float>(dx + left), static_cast<float>(dy + top),
        static_cast<float>(dx + right), static_cast<float>(dy + bottom)};
    sgl::ScreenTransform transform;
    if (make_screen_transform(render_source, src.image, transform)) source_rect = transform.map(source_rect);
    if (make_screen_transform(render_destination, dst.image, transform)) target_rect = transform.map(target_rect);
    const RECT bounds{0, 0, static_cast<LONG>(dst.image.width), static_cast<LONG>(dst.image.height)};
    const char* failure_stage = nullptr;
    // Source and destination may overlap in a copy onto the same surface; copy_within reads a GPU snapshot.
    const bool self_copy = render_source == render_destination;
    const HRESULT result = self_copy ?
        surface_renderer.copy_within(render_destination, dst.image, source_rect, target_rect, bounds, &failure_stage) :
        surface_renderer.draw(render_source, src.image, render_destination, dst.image, source_rect, target_rect, bounds,
            std::nullopt, sgl::BlendMode::Opaque, &failure_stage);
    unlock_texture(src);
    if (FAILED(result)) {
        char draw_message[160]{};
        std::snprintf(draw_message, sizeof(draw_message), "sgl_Blt%s D3D11: %s failed (HRESULT 0x%08lX)",
            self_copy ? " self-copy" : "", failure_stage ? failure_stage : "unknown stage",
            static_cast<unsigned long>(result));
        sgl::unimplemented(draw_message);
    }
}

extern "C" void __cdecl sgl_Blt(int sx, int sy, int dx, int dy, int width, int height, std::uint32_t flags) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    blit_bitmap(sx, sy, dx, dy, width, height, flags);
}

extern "C" void __cdecl sgl_Blt_RC(std::uint32_t flags) {
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    // The flags go to the same copy as sgl_Blt, which ignores them.
    const auto source = textures.find(render_source);
    const auto target = textures.find(render_destination);
    if (source == textures.end() || target == textures.end()) sgl::unimplemented("sgl_Blt_RC state");
    const RECT source_bounds = texture_bounds(render_source, source->second->image);
    const RECT target_bounds = texture_bounds(render_destination, target->second->image);
    const RECT copy_bounds{std::max(source_bounds.left, target_bounds.left),
        std::max(source_bounds.top, target_bounds.top), std::min(source_bounds.right, target_bounds.right),
        std::min(source_bounds.bottom, target_bounds.bottom)};
    const RECT clip_bounds{clip_rectangle[0], clip_rectangle[1], clip_rectangle[2], clip_rectangle[3]};
    for (RECT rect : get_active_clipper()->rectangles) {
        if (clipping_mode == 3 && !intersect_with(rect, clip_bounds)) continue;
        if (!intersect_with(rect, copy_bounds)) continue;
        blit_bitmap(rect.left, rect.top, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags);
    }
}

extern "C" void __cdecl sgl_FreeMemoryTexture(void* handle) {
    if (!handle) return;
    std::lock_guard<TexturesMutex> lock(textures_mutex);
    if (is_screen_texture(handle)) return;
    const auto entry = textures.find(handle);
    if (entry == textures.end()) sgl::unimplemented("sgl_FreeMemoryTexture object");
    if (render_source == handle) render_source = nullptr;
    if (render_destination == handle) render_destination = nullptr;
    surface_renderer.forget(handle);
    textures.erase(entry);
    ++textures_generation;
}

extern "C" void __cdecl sgl_FreeTexture(void* handle) {
    sgl_FreeMemoryTexture(handle);
}
