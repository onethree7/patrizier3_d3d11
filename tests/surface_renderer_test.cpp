#include "surface_renderer.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    constexpr std::uint32_t width = 3;
    constexpr std::uint32_t height = 2;
    constexpr std::uint32_t pitch = 12;
    std::vector<std::uint8_t> source_pixels(pitch * height, 0xe7);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(y) * pitch + x * 3;
            source_pixels[offset] = static_cast<std::uint8_t>(10 + x + y * 3);
            source_pixels[offset + 1] = static_cast<std::uint8_t>(40 + x + y * 3);
            source_pixels[offset + 2] = static_cast<std::uint8_t>(70 + x + y * 3);
        }
    }
    sgl::DecodedImage source{width, height, pitch, sgl::ImageFormat::Bgr24, source_pixels, {}};
    sgl::DecodedImage bgra{width, height, width * 4, sgl::ImageFormat::Bgra32,
        std::vector<std::uint8_t>(width * height * 4, 0x44), {}};
    sgl::DecodedImage bgr_target{width, height, pitch, sgl::ImageFormat::Bgr24,
        std::vector<std::uint8_t>(pitch * height, 0xa5), {}};

    int source_id = 0, bgra_id = 0, bgr_target_id = 0;
    sgl::SurfaceRenderer renderer;
    const RECT clip{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const sgl::Rect bounds{0, 0, static_cast<float>(width), static_cast<float>(height)};
    HRESULT result = renderer.draw(&source_id, source, &bgra_id, bgra, bounds, bounds, clip,
        std::nullopt, sgl::BlendMode::Opaque);
    if (FAILED(result)) return 1;
    if (bgra.pixels.front() != 0x44) {
        std::fprintf(stderr, "GPU draw unexpectedly changed the CPU copy before synchronization.\n");
        return 6;
    }
    result = renderer.synchronize_to_cpu(&bgra_id, bgra);
    if (FAILED(result)) return 7;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto source_offset = static_cast<std::size_t>(y) * pitch + x * 3;
            const auto target_offset = (static_cast<std::size_t>(y) * width + x) * 4;
            if (std::memcmp(source.pixels.data() + source_offset, bgra.pixels.data() + target_offset, 3) != 0 ||
                bgra.pixels[target_offset + 3] != 0xff) {
                std::fprintf(stderr, "BGR-to-BGRA mismatch at %u,%u.\n", x, y);
                return 2;
            }
        }
    }

    result = renderer.draw(&source_id, source, &bgr_target_id, bgr_target, bounds, bounds, clip,
        std::nullopt, sgl::BlendMode::Opaque);
    if (FAILED(result)) return 3;
    result = renderer.synchronize_to_cpu(&bgr_target_id, bgr_target);
    if (FAILED(result)) return 8;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto row = static_cast<std::size_t>(y) * pitch;
        if (std::memcmp(source.pixels.data() + row, bgr_target.pixels.data() + row, width * 3) != 0) return 4;
        for (std::uint32_t x = width * 3; x < pitch; ++x) {
            if (bgr_target.pixels[row + x] != 0xa5) return 5;
        }
    }

    const sgl::DecodedImage changed_source{width + 1, height, pitch, sgl::ImageFormat::Bgr24, source_pixels, {}};
    result = renderer.draw(&source_id, changed_source, &bgra_id, bgra, bounds, bounds, clip,
        std::nullopt, sgl::BlendMode::Opaque);
    if (result != E_INVALIDARG) {
        std::fprintf(stderr, "A renderer surface was reused with different source dimensions.\n");
        return 15;
    }

    const auto preserved_offset = static_cast<std::size_t>(pitch) + 4;
    const std::uint8_t cpu_pixel[] = {0x21, 0x43, 0x65, 0xff};
    std::memcpy(bgra.pixels.data() + preserved_offset, cpu_pixel, sizeof(cpu_pixel));
    renderer.mark_cpu_modified(&bgra_id);
    const sgl::Rect first_pixel{0, 0, 1, 1};
    result = renderer.draw(&source_id, source, &bgra_id, bgra, first_pixel, first_pixel, clip,
        std::nullopt, sgl::BlendMode::Opaque);
    if (FAILED(result)) return 9;
    result = renderer.synchronize_to_cpu(&bgra_id, bgra);
    if (FAILED(result)) return 10;
    if (std::memcmp(bgra.pixels.data() + preserved_offset, cpu_pixel, sizeof(cpu_pixel)) != 0) {
        std::fprintf(stderr, "CPU modification was lost when the GPU surface was updated.\n");
        return 11;
    }

    int alpha_source_id = 0;
    const std::vector<std::uint8_t> alpha_pixels = {200, 100, 50, 128};
    const sgl::DecodedImage alpha_source{1, 1, 4, sgl::ImageFormat::Bgra32, alpha_pixels, {}};
    const sgl::Rect alpha_source_rect{0, 0, 1, 1};
    const auto alpha_destination_rect = alpha_source_rect;
    result = renderer.draw(&alpha_source_id, alpha_source, &bgra_id, bgra,
        alpha_source_rect, alpha_destination_rect, clip, std::nullopt, sgl::BlendMode::Alpha);
    if (FAILED(result)) return 12;
    result = renderer.synchronize_to_cpu(&bgra_id, bgra);
    if (FAILED(result)) return 13;
    const std::uint8_t expected_blend[] = {103, 69, 59, 191};
    if (std::memcmp(bgra.pixels.data(), expected_blend, sizeof(expected_blend)) != 0 ||
        std::memcmp(bgra.pixels.data() + preserved_offset, cpu_pixel, sizeof(cpu_pixel)) != 0) {
        std::fprintf(stderr, "A clipped alpha draw changed pixels outside its destination rectangle.\n");
        return 14;
    }

    // Queued blended draws must match drawing one at a time: a draw that reads pixels an earlier queued draw writes
    // ends the batch, while draws that do not overlap share one batch.
    const auto run_sequence = [](bool synchronize_each, std::vector<std::uint8_t>& output) -> HRESULT {
        constexpr std::uint32_t sequence_width = 8;
        sgl::DecodedImage target{sequence_width, 1, sequence_width * 4, sgl::ImageFormat::Bgra32, {}, {}};
        for (std::uint32_t x = 0; x < sequence_width; ++x) {
            const std::uint8_t pixel[] = {static_cast<std::uint8_t>(20 + x * 25), static_cast<std::uint8_t>(200 - x * 20),
                static_cast<std::uint8_t>(90 + x * 7), 0xff};
            target.pixels.insert(target.pixels.end(), pixel, pixel + 4);
        }
        const sgl::DecodedImage opaque{1, 1, 4, sgl::ImageFormat::Bgra32, {12, 230, 140, 0xff}, {}};
        const sgl::DecodedImage translucent{2, 1, 8, sgl::ImageFormat::Bgra32, {200, 100, 50, 128, 30, 250, 170, 77}, {}};
        int opaque_id = 0, translucent_id = 0, target_id = 0;
        sgl::SurfaceRenderer sequence_renderer;
        const RECT full{0, 0, static_cast<LONG>(sequence_width), 1};
        struct Step { const sgl::DecodedImage* source; int* id; float left; float width; sgl::BlendMode blend; };
        const Step steps[] = {
            {&opaque, &opaque_id, 2, 1, sgl::BlendMode::Opaque},
            {&translucent, &translucent_id, 2, 1, sgl::BlendMode::Alpha},
            {&translucent, &translucent_id, 5, 1, sgl::BlendMode::Alpha},
            {&translucent, &translucent_id, 0, 2, sgl::BlendMode::Alpha},
            {&translucent, &translucent_id, 5, 2, sgl::BlendMode::Multiply2},
            {&opaque, &opaque_id, 7, 1, sgl::BlendMode::Opaque},
            {&translucent, &translucent_id, 3, 1, sgl::BlendMode::Alpha},
        };
        for (const Step& step : steps) {
            const sgl::Rect source_rect{0, 0, step.width, 1};
            const sgl::Rect destination{step.left, 0, step.left + step.width, 1};
            HRESULT hr = sequence_renderer.draw(step.id, *step.source, &target_id, target, source_rect, destination, full,
                std::nullopt, step.blend);
            if (FAILED(hr)) return hr;
            if (synchronize_each) {
                hr = sequence_renderer.synchronize_to_cpu(&target_id, target);
                if (FAILED(hr)) return hr;
            }
        }
        const HRESULT hr = sequence_renderer.synchronize_to_cpu(&target_id, target);
        output = target.pixels;
        return hr;
    };
    std::vector<std::uint8_t> one_at_a_time, batched;
    if (FAILED(run_sequence(true, one_at_a_time)) || FAILED(run_sequence(false, batched))) return 16;
    if (one_at_a_time != batched) {
        std::fprintf(stderr, "Batched blended draws differ from drawing them one at a time.\n");
        return 17;
    }

    // Many draws of all blend modes, sizes and clips: queued, they are reordered into layers, share background copies
    // and draw calls; the result must equal drawing them one at a time.
    const auto run_random = [](bool synchronize_each, std::vector<std::uint8_t>& output) -> HRESULT {
        constexpr std::uint32_t random_width = 192, random_height = 96;
        std::uint32_t seed = 4242;
        const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        sgl::DecodedImage target{random_width, random_height, random_width * 4, sgl::ImageFormat::Bgra32, {}, {}};
        target.pixels.resize(static_cast<std::size_t>(target.pitch) * random_height);
        for (auto& byte : target.pixels) byte = static_cast<std::uint8_t>(next());
        sgl::DecodedImage sprite{12, 10, 48, sgl::ImageFormat::Bgra32, {}, {}};
        sprite.pixels.resize(48 * 10);
        for (auto& byte : sprite.pixels) byte = static_cast<std::uint8_t>(next());
        int sprite_id = 0, target_id = 0;
        sgl::SurfaceRenderer random_renderer;
        const sgl::BlendMode modes[] = {sgl::BlendMode::Opaque, sgl::BlendMode::Alpha, sgl::BlendMode::Alpha,
            sgl::BlendMode::Multiply, sgl::BlendMode::Additive, sgl::BlendMode::FillAlpha, sgl::BlendMode::Multiply2};
        for (int step = 0; step < 600; ++step) {
            const auto mode = modes[next() % std::size(modes)];
            const bool large = next() % 10 == 0;
            const float width = static_cast<float>(large ? 40 + next() % 120 : 4 + next() % 9);
            const float height = static_cast<float>(large ? 20 + next() % 60 : 3 + next() % 8);
            const float left = static_cast<float>(static_cast<int>(next() % (random_width + 8)) - 4);
            const float top = static_cast<float>(static_cast<int>(next() % (random_height + 8)) - 4);
            const LONG clip_left = static_cast<LONG>(next() % random_width), clip_top = static_cast<LONG>(next() % random_height);
            const RECT clip = next() % 3 == 0 ? RECT{0, 0, static_cast<LONG>(random_width), static_cast<LONG>(random_height)} :
                RECT{clip_left, clip_top, clip_left + 1 + static_cast<LONG>(next() % 40), clip_top + 1 + static_cast<LONG>(next() % 30)};
            const sgl::Rect source_rect{0, 0, 12, 10};
            const sgl::Rect destination{left, top, left + width, top + height};
            const std::uint32_t modulation = 0xff000000u | (next() & 0x00ffffffu) | ((next() % 2) << 30);
            HRESULT hr = random_renderer.draw(&sprite_id, sprite, &target_id, target, source_rect, destination, clip,
                std::nullopt, mode, nullptr, mode == sgl::BlendMode::Opaque ? 0xffffffffu : modulation);
            if (FAILED(hr)) return hr;
            if (synchronize_each) {
                hr = random_renderer.synchronize_to_cpu(&target_id, target);
                if (FAILED(hr)) return hr;
            }
        }
        const HRESULT hr = random_renderer.synchronize_to_cpu(&target_id, target);
        output = target.pixels;
        return hr;
    };
    std::vector<std::uint8_t> random_each, random_queued;
    if (FAILED(run_random(true, random_each)) || FAILED(run_random(false, random_queued))) return 22;
    if (random_each != random_queued) {
        std::size_t differences = 0;
        for (std::size_t index = 0; index < random_each.size(); ++index) differences += random_each[index] != random_queued[index];
        std::fprintf(stderr, "Queued random draws differ from drawing them one at a time in %zu bytes.\n", differences);
        return 23;
    }

    // The GPU water effect must equal the game's routine, which rewrites its tiles in place: tiles in list order, rows
    // from the top, pixels from the left, each reading a displaced pixel of the same image. Random heights give large
    // displacements, so reads of already rewritten pixels form long chains; reads past the limit size are clamped.
    {
        constexpr LONG water_width = 96, water_height = 72, limit_width = 86, limit_height = 68;
        std::uint32_t seed = 12345;
        const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        sgl::DecodedImage water{static_cast<std::uint32_t>(water_width), static_cast<std::uint32_t>(water_height),
            static_cast<std::uint32_t>(water_width) * 4, sgl::ImageFormat::Bgra32, {}, {}};
        water.pixels.resize(static_cast<std::size_t>(water.pitch) * water_height);
        for (auto& byte : water.pixels) byte = static_cast<std::uint8_t>(next());
        const LONG height_pitch = water_width + 2;
        std::vector<std::uint8_t> heights(static_cast<std::size_t>(height_pitch) * (water_height + 2));
        for (LONG y = 0; y < water_height + 2; ++y)
            for (LONG x = 0; x < height_pitch; ++x)
                heights[static_cast<std::size_t>(y) * height_pitch + x] = static_cast<std::uint8_t>(
                    (x * 9 + y * 5) % 120 + (next() % 4 == 0 ? next() % 136 : 0));
        const RECT tile_bounds{3, 5, water_width - 6, water_height - 2};
        std::vector<RECT> tiles;
        for (LONG cell = 0; cell < (water_width / 8) * (water_height / 8); ++cell) {
            if (next() % 5 == 0) continue;
            const LONG x = cell % (water_width / 8) * 8, y = cell / (water_width / 8) * 8;
            const RECT tile{std::max(x, tile_bounds.left), std::max(y, tile_bounds.top), std::min(x + 8, tile_bounds.right),
                std::min(y + 8, tile_bounds.bottom)};
            if (tile.left < tile.right && tile.top < tile.bottom) tiles.push_back(tile);
        }
        for (std::size_t index = tiles.size(); index > 1; --index) std::swap(tiles[index - 1], tiles[next() % index]);
        RECT area{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
        for (const RECT& tile : tiles)
            area = {std::min(area.left, tile.left), std::min(area.top, tile.top), std::max(area.right, tile.right),
                std::max(area.bottom, tile.bottom)};
        const auto apply = [&](std::vector<std::uint8_t>& pixels, bool in_place) {
            const std::vector<std::uint8_t> before = pixels;
            const auto& read = in_place ? pixels : before;
            for (const RECT& tile : tiles) for (LONG y = tile.top; y < tile.bottom; ++y) for (LONG x = tile.left; x < tile.right; ++x) {
                const auto h = [&](LONG hx, LONG hy) { return static_cast<int>(heights[static_cast<std::size_t>(hy) * height_pitch + hx]); };
                const int dx = h(x + 1, y) - h(x, y), dy = h(x, y + 1) - h(x, y);
                const LONG sx = std::clamp<LONG>(x + dx / 4 * 2, 0, limit_width - 1);
                const LONG sy = std::clamp<LONG>(y + dy / 2, 0, limit_height - 1);
                for (int channel = 0; channel < 3; ++channel)
                    pixels[static_cast<std::size_t>(y) * water.pitch + x * 4 + channel] = static_cast<std::uint8_t>(
                        std::clamp(read[static_cast<std::size_t>(sy) * water.pitch + sx * 4 + channel] - dy, 0, 255));
            }
        };
        std::vector<std::uint8_t> expected = water.pixels, snapshot = water.pixels;
        apply(expected, true);
        apply(snapshot, false);
        if (expected == snapshot) {
            std::fprintf(stderr, "The water test data does not distinguish in-place from snapshot reads.\n");
            return 18;
        }
        int water_id = 0;
        sgl::SurfaceRenderer water_renderer;
        result = water_renderer.draw_water(&water_id, water, tiles.data(), static_cast<UINT>(tiles.size()), area,
            heights.data() + static_cast<std::size_t>(area.top) * height_pitch + area.left, height_pitch,
            limit_width, limit_height);
        if (FAILED(result)) return 19;
        result = water_renderer.synchronize_to_cpu(&water_id, water);
        if (FAILED(result)) return 20;
        if (water.pixels != expected) {
            std::size_t differences = 0;
            for (std::size_t index = 0; index < expected.size(); ++index) differences += water.pixels[index] != expected[index];
            std::fprintf(stderr, "GPU water effect differs from the in-place original in %zu bytes.\n", differences);
            return 21;
        }
    }

    // Copies onto the same surface read its pixels as before each call, including those of a queued draw just before
    // and of the previous overlapping copy, also when scaled (fixed-point sampling of the SGL contract).
    {
        constexpr std::uint32_t self_width = 40, self_height = 30;
        std::uint32_t seed = 777;
        const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        const auto random_image = [&](std::uint32_t image_width, std::uint32_t image_height) {
            sgl::DecodedImage image{image_width, image_height, image_width * 4, sgl::ImageFormat::Bgra32, {}, {}};
            image.pixels.resize(static_cast<std::size_t>(image.pitch) * image_height);
            for (auto& byte : image.pixels) byte = static_cast<std::uint8_t>(next());
            return image;
        };
        sgl::DecodedImage self = random_image(self_width, self_height);
        const sgl::DecodedImage patch = random_image(8, 6);
        std::vector<std::uint32_t> expected(self_width * self_height);
        std::memcpy(expected.data(), self.pixels.data(), self.pixels.size());
        // The SGL contract's fixed-point sampling, applied to a copy of the expected pixels taken before the call.
        const auto copy = [&](RECT source_rect, RECT target_rect) {
            const std::vector<std::uint32_t> before = expected;
            const auto step_x = (static_cast<std::uint32_t>(source_rect.right - source_rect.left) << 16) /
                static_cast<std::uint32_t>(target_rect.right - target_rect.left);
            const auto step_y = (static_cast<std::uint32_t>(source_rect.bottom - source_rect.top) << 16) /
                static_cast<std::uint32_t>(target_rect.bottom - target_rect.top);
            for (LONG y = target_rect.top; y < target_rect.bottom; ++y) for (LONG x = target_rect.left; x < target_rect.right; ++x) {
                const auto sx = source_rect.left + static_cast<LONG>(((x - target_rect.left) * step_x + (step_x >> 1)) >> 16);
                const auto sy = source_rect.top + static_cast<LONG>(((y - target_rect.top) * step_y + (step_y >> 1)) >> 16);
                expected[static_cast<std::size_t>(y) * self_width + x] = before[static_cast<std::size_t>(sy) * self_width + sx];
            }
        };
        const auto rect = [](RECT value) {
            return sgl::Rect{static_cast<float>(value.left), static_cast<float>(value.top),
                static_cast<float>(value.right), static_cast<float>(value.bottom)};
        };
        int self_id = 0, patch_id = 0;
        sgl::SurfaceRenderer self_renderer;
        const RECT self_clip{0, 0, static_cast<LONG>(self_width), static_cast<LONG>(self_height)};
        // A queued draw the first copy reads.
        const RECT patch_target{4, 4, 12, 10};
        if (FAILED(self_renderer.draw(&patch_id, patch, &self_id, self, sgl::Rect{0, 0, 8, 6}, rect(patch_target), self_clip,
                std::nullopt, sgl::BlendMode::Opaque))) return 24;
        for (LONG y = patch_target.top; y < patch_target.bottom; ++y)
            std::memcpy(expected.data() + static_cast<std::size_t>(y) * self_width + patch_target.left,
                patch.pixels.data() + static_cast<std::size_t>(y - patch_target.top) * patch.pitch, patch.pitch);
        const RECT copies[][2] = {
            {{2, 2, 22, 16}, {6, 5, 26, 19}},     // overlaps the queued draw and its own target
            {{5, 3, 15, 11}, {12, 9, 27, 21}},    // scaled 10x8 to 15x12, reading the first copy
            {{20, 10, 36, 26}, {18, 12, 34, 28}}, // overlapping shift back over both
        };
        for (const auto& pair : copies) {
            if (FAILED(self_renderer.copy_within(&self_id, self, rect(pair[0]), rect(pair[1]), self_clip))) return 25;
            copy(pair[0], pair[1]);
        }
        if (FAILED(self_renderer.synchronize_to_cpu(&self_id, self))) return 26;
        std::size_t differences = 0;
        for (std::size_t index = 0; index < expected.size(); ++index) {
            std::uint32_t actual = 0;
            std::memcpy(&actual, self.pixels.data() + index * 4, 4);
            differences += actual != expected[index];
        }
        if (differences) {
            std::fprintf(stderr, "GPU self-copy differs from sequential copies in %zu pixels.\n", differences);
            return 27;
        }
    }

    std::puts("GPU region copy preserved blended pixels and untouched destination content.");
    return 0;
}
