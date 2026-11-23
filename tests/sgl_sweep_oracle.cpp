// Runs one suite of SGL calls over its whole parameter space with one library and prints one line per case, so the
// original library and the replacement can be compared case by case (tests/run-sweep.mjs). Each line holds the case
// parameters and a hash of the resulting pixels (or the pixels themselves with --dump). A case that faults prints
// "fault" and ends the run; the replacement ends the process on input it does not support. The runner restarts the
// run after such a case.
// Usage: sgl_sweep_oracle <SGL DLL> <game directory> <suite> <first case> <last case> [--dump] [--names file]
// Suites: blend, stretch, clip, fill, fillrow, mask, blt, bltrc, clear, nodest, lock, text, assets. "count" as first case prints the case count.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {
struct LockedTexture {
    std::uint8_t* pixels;
    std::uint32_t* palette;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
};
static_assert(sizeof(LockedTexture) == 20);

struct Api {
    int (__cdecl* initialize)();
    std::uint32_t (__cdecl* set_key)(std::uint32_t);
    void* (__cdecl* create)(std::uint16_t, std::uint16_t, std::uint32_t);
    void (__cdecl* free_texture)(void*);
    int (__cdecl* lock)(void*, LockedTexture*, int);
    void (__cdecl* unlock)(void*);
    int (__cdecl* change_palette)(void*, std::uint32_t*);
    void* (__cdecl* set_source)(void*);
    void* (__cdecl* set_target)(void*);
    int (__cdecl* clip_mode)(int);
    void (__cdecl* clip_rect)(int, int, int, int);
    void (__cdecl* reset_clipper)();
    void (__cdecl* add_clip)(int, int, int, int);
    void (__cdecl* sub_clip)(int, int, int, int);
    void* (__cdecl* create_clipper)();
    void* (__cdecl* set_clipper)(void*);
    void (__cdecl* free_clipper)(void*);
    void* (__cdecl* font_proportional)(const unsigned char*, void*, int, int, int, int);
    void* (__cdecl* font_kerning)(const unsigned char*, void*, int, int, int, int);
    void* (__cdecl* font_fixed)(const unsigned char*, void*, int, int, int);
    void (__cdecl* set_font)(const void*);
    void (__cdecl* text_mode)(int);
    void (__cdecl* draw_text)(int, int, const unsigned char*);
    void (__cdecl* draw_text_rect)(int, int, const std::int32_t*, const unsigned char*);
    std::uint32_t (__cdecl* set_color)(std::uint32_t);
    std::uint32_t (__cdecl* set_blend_mode)(std::uint32_t);
    void (__cdecl* draw)(int, int, int, int, int, int);
    void (__cdecl* stretch)(int, int, int, int, int, int, int, int);
    int (__cdecl* draw_masked)(std::uint32_t, int, int, int, int, int, int);
    void (__cdecl* fill)(int, int, int, int);
    void (__cdecl* blt)(int, int, int, int, int, int, std::uint32_t);
    void (__cdecl* blt_rc)(std::uint32_t);
    void (__cdecl* clear)(std::uint32_t, std::uint32_t, float);
    void* (__cdecl* load)(const char*, std::uint32_t);
    void (__cdecl* texture_info)(const void*, std::uint32_t*);
    void* (__cdecl* set_archive)(void*);
};
Api api{};
bool dump = false;
std::vector<std::string> names;

template<typename T> bool resolve(HMODULE module, const char* name, T& target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

constexpr std::uint32_t key = 0x00ff00ffu;
constexpr std::array<std::uint32_t, 3> colors = {0xffffffffu, 0xff804020u, 0x80ffffffu};

std::uint32_t source_pixel(int x, int y) {
    if ((x + y) % 5 == 0) return key;
    if ((x + 2 * y) % 7 == 0) return 0xffff00ffu;
    const auto alpha = static_cast<std::uint32_t>((x * 37 + y * 53) & 0xff);
    return (alpha << 24) | ((x * 29u + 40u) & 0xff) << 16 | ((y * 31u + 20u) & 0xff) << 8 | ((x * 3u + y * 17u) & 0xff);
}

std::uint32_t target_pixel(int x, int y) {
    return 0xff000000u | ((x * 19u + 60u) & 0xff) << 16 | ((y * 23u + 90u) & 0xff) << 8 | ((x * 7u + y * 11u + 30u) & 0xff);
}

std::uint64_t fnv(const std::uint8_t* data, std::size_t size, std::uint64_t hash = 1469598103934665603ull) {
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

// An 8x8 source: BGRA, or indexed with a palette built from the same pattern.
void* make_source(bool indexed, std::uint32_t flags, int size = 8) {
    void* source = api.create(static_cast<std::uint16_t>(size), static_cast<std::uint16_t>(size), flags | (indexed ? 0x20u : 0u));
    if (!source) return nullptr;
    LockedTexture pixels{};
    if (!api.lock(source, &pixels, 2)) return source;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (indexed) pixels.pixels[y * pixels.pitch + x] = static_cast<std::uint8_t>((x * 7 + y * 3) % 12);
            else {
                const auto value = source_pixel(x, y);
                std::memcpy(pixels.pixels + y * pixels.pitch + x * 4, &value, 4);
            }
        }
    }
    api.unlock(source);
    if (indexed) {
        std::array<std::uint32_t, 256> palette{};
        for (int index = 0; index < 256; ++index) palette[index] = source_pixel(index % 8, index / 8 % 8);
        palette[0] = key;
        palette[1] = 0xffff00ffu;
        api.change_palette(source, palette.data());
    }
    return source;
}

void* make_target(int size) {
    void* target = api.create(static_cast<std::uint16_t>(size), static_cast<std::uint16_t>(size), 0);
    if (!target) return nullptr;
    LockedTexture pixels{};
    if (!api.lock(target, &pixels, 2)) return target;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto value = target_pixel(x, y);
            std::memcpy(pixels.pixels + y * pixels.pitch + x * 4, &value, 4);
        }
    }
    api.unlock(target);
    return target;
}

void print_target(void* target, int size) {
    LockedTexture pixels{};
    if (!api.lock(target, &pixels, 1)) { std::printf(" lockfail"); return; }
    if (dump) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                std::uint32_t value = 0;
                std::memcpy(&value, pixels.pixels + y * pixels.pitch + x * 4, 4);
                std::printf(" %08x", value);
            }
        }
    } else {
        std::uint64_t hash = 1469598103934665603ull;
        for (int y = 0; y < size; ++y) hash = fnv(pixels.pixels + y * pixels.pitch, static_cast<std::size_t>(size) * 4, hash);
        std::printf(" %016llx", static_cast<unsigned long long>(hash));
    }
    api.unlock(target);
}

void reset_state(int size) {
    api.set_source(nullptr);
    api.set_target(nullptr);
    api.set_color(0xffffffffu);
    api.set_key(key);
    api.clip_mode(1);
    api.clip_rect(0, 0, size, size);
}

// blend: source format x color flags x blend byte x constant color.
constexpr int blend_cases = 2 * 8 * 256 * static_cast<int>(colors.size());
void blend_case(int number) {
    int rest = number;
    const auto color = colors[rest % colors.size()];
    rest /= static_cast<int>(colors.size());
    const auto blend = static_cast<std::uint8_t>(rest % 256);
    rest /= 256;
    const auto color_flags = static_cast<std::uint32_t>(rest % 8);
    const bool indexed = rest / 8 != 0;
    std::printf("%d %s flags=%u blend=%u color=%08x", number, indexed ? "indexed" : "bgra", color_flags, blend, color);
    std::fflush(stdout);
    void* source = make_source(indexed, color_flags);
    void* target = make_target(12);
    *(static_cast<std::uint8_t*>(source) + 0x37) = blend;
    reset_state(12);
    api.set_color(color);
    api.set_source(source);
    api.set_target(target);
    api.draw(0, 0, 2, 2, 8, 8);
    reset_state(12);
    print_target(target, 12);
    api.free_texture(source);
    api.free_texture(target);
}

// stretch: source format x texture flags (color flags and the upper flag bits) x blend byte x size x color.
constexpr std::array<std::uint32_t, 3> stretch_color_flags = {0, 3, 7};
constexpr std::array<std::uint32_t, 6> stretch_high_flags = {0, 0x08, 0x10, 0x40, 0x80, 0x01000000};
constexpr std::array<std::uint8_t, 5> stretch_blends = {0, 1, 2, 3, 5};
constexpr std::array<std::array<int, 2>, 4> stretch_sizes = {{{13, 11}, {5, 3}, {16, 16}, {8, 8}}};
constexpr std::array<std::uint32_t, 3> stretch_colors = {0xffffffffu, 0x80ffffffu, 0xff804020u};
constexpr int stretch_cases = 2 * 3 * 6 * 5 * 4 * 3;
void stretch_case(int number) {
    int rest = number;
    const auto color = stretch_colors[rest % 3]; rest /= 3;
    const auto size = stretch_sizes[rest % 4]; rest /= 4;
    const auto blend = stretch_blends[rest % 5]; rest /= 5;
    const auto high = stretch_high_flags[rest % 6]; rest /= 6;
    const auto color_flags = stretch_color_flags[rest % 3]; rest /= 3;
    const bool indexed = rest != 0;
    std::printf("%d %s flags=0x%x blend=%u size=%dx%d color=%08x", number, indexed ? "indexed" : "bgra",
        color_flags | high, blend, size[0], size[1], color);
    std::fflush(stdout);
    void* source = make_source(indexed, color_flags | high);
    void* target = make_target(18);
    *(static_cast<std::uint8_t*>(source) + 0x37) = blend;
    reset_state(18);
    api.set_color(color);
    api.set_source(source);
    api.set_target(target);
    api.stretch(0, 0, 1, 1, 8, 8, size[0], size[1]);
    reset_state(18);
    print_target(target, 18);
    api.free_texture(source);
    api.free_texture(target);
}

// clip: clipping mode x clip setup x destination position x draw kind.
struct Position { int sx, sy, dx, dy, width, height; };
constexpr std::array<Position, 7> clip_positions = {{
    {0, 0, 2, 2, 8, 8}, {0, 0, -3, -2, 8, 8}, {0, 0, 8, 9, 8, 8}, {4, 2, 1, 1, 8, 8}, {2, 3, 3, 1, 5, 4},
    {-2, -1, 2, 2, 8, 8}, {0, 0, 2, 2, -4, 8}}};
constexpr std::array<int, 8> clip_modes = {0, 1, 2, 3, 4, 5, 8, -1};
constexpr int clip_cases = 8 * 4 * 7 * 3;
// Clip setups: 0 full clip rectangle, 1 partial clip rectangle, 2 a clipper with two rectangles minus a third,
// 3 that clipper and the partial clip rectangle.
void clip_case(int number) {
    int rest = number;
    const int kind = rest % 3; rest /= 3;
    const auto position = clip_positions[rest % 7]; rest /= 7;
    const int setup = rest % 4; rest /= 4;
    const int mode = clip_modes[rest];
    std::printf("%d mode=%d setup=%d position=%d,%d,%d,%d,%d,%d kind=%d", number, mode, setup, position.sx, position.sy,
        position.dx, position.dy, position.width, position.height, kind);
    std::fflush(stdout);
    const bool indexed = kind == 2;
    void* source = make_source(indexed, indexed ? 7u : 0u);
    void* target = make_target(12);
    *(static_cast<std::uint8_t*>(source) + 0x37) = static_cast<std::uint8_t>(kind == 0 ? 0 : 1);
    reset_state(12);
    static void* clipper = api.create_clipper();
    void* previous_clipper = api.set_clipper(clipper);
    api.reset_clipper();
    if (setup == 1 || setup == 3) api.clip_rect(3, 3, 9, 10);
    if (setup >= 2) {
        api.add_clip(2, 2, 6, 12);
        api.add_clip(7, 1, 11, 5);
        api.sub_clip(3, 3, 5, 5);
    }
    api.clip_mode(mode);
    if (kind == 1) api.set_color(0x80ffffffu);
    api.set_source(source);
    api.set_target(target);
    api.draw(position.sx, position.sy, position.dx, position.dy, position.width, position.height);
    api.reset_clipper();
    api.set_clipper(previous_clipper);
    reset_state(12);
    print_target(target, 12);
    api.free_texture(source);
    api.free_texture(target);
}

// fill: blend mode x color x rectangle.
constexpr std::array<std::uint32_t, 4> fill_colors = {0xff204080u, 0x80204080u, 0x00204080u, 0xffffffffu};
constexpr std::array<std::array<int, 4>, 6> fill_rects = {{{2, 2, 6, 5}, {-2, -3, 6, 6}, {8, 8, 10, 10}, {1, 1, 9, 2},
    {3, 4, 7, 1}, {0, 0, 12, 12}}};
constexpr int fill_cases = 8 * 4 * 6;
void fill_case(int number) {
    int rest = number;
    const auto rect = fill_rects[rest % 6]; rest /= 6;
    const auto color = fill_colors[rest % 4]; rest /= 4;
    const auto mode = static_cast<std::uint32_t>(rest);
    std::printf("%d mode=%u color=%08x rect=%d,%d,%d,%d", number, mode, color, rect[0], rect[1], rect[2], rect[3]);
    std::fflush(stdout);
    void* target = make_target(12);
    reset_state(12);
    const auto previous = api.set_blend_mode(mode);
    api.set_color(color);
    api.set_target(target);
    api.fill(rect[0], rect[1], rect[2], rect[3]);
    api.set_blend_mode(previous);
    reset_state(12);
    print_target(target, 12);
    api.free_texture(target);
}

// fillrow: multiplying fills of one row at every start and width, for the pixel order of the original's loop.
constexpr int fillrow_cases = 4 * 12;
void fillrow_case(int number) {
    const int x = number % 4;
    const int width = 1 + number / 4;
    std::printf("%d x=%d width=%d", number, x, width);
    std::fflush(stdout);
    void* target = make_target(12);
    reset_state(12);
    const auto previous = api.set_blend_mode(2);
    api.set_color(0xffffffffu);
    api.set_target(target);
    api.fill(x, 0, width, 1);
    api.set_blend_mode(previous);
    reset_state(12);
    print_target(target, 12);
    api.free_texture(target);
}

// mask: position x blend byte x color flags x color, indexed source with an indexed alpha mask.
constexpr std::array<Position, 3> mask_positions = {{{0, 0, 2, 2, 8, 8}, {4, 2, 1, 1, 8, 8}, {-2, 0, 2, 2, 8, 8}}};
constexpr int mask_matrix_cases = 3 * 8 * 2 * 3;
constexpr int city_gate_mask_cases = 2;
void city_gate_mask_case(int number) {
    constexpr int size = 320;
    reset_state(size);
    void* source = api.load("images/module_stadtkarte/Stadttor-mitte(Mark).aim", 0x01000007);
    void* mask = api.load("images/module_stadtkarte/fogmaps/Stadttor-mitte(Mark).aim", 0x01000007);
    void* target = make_target(size);
    const bool clipped = number == mask_matrix_cases;
    std::printf("%d city gate first draw %s", number, clipped ? "clipped" : "full");
    std::fflush(stdout);
    if (!source || !mask || !target) {
        std::fputs("Cannot load the city gate mask fixture\n", stderr);
        std::exit(8);
    }
    api.set_source(source);
    api.set_target(target);
    const auto result = api.draw_masked(reinterpret_cast<std::uintptr_t>(mask), 0, 0,
        clipped ? 279 : 0, clipped ? -73 : 0, clipped ? 10 : 58, 109);
    reset_state(size);
    std::printf(" result=%d", result);
    print_target(target, size);
    api.free_texture(source);
    api.free_texture(mask);
    api.free_texture(target);
}

void mask_case(int number) {
    if (number >= mask_matrix_cases) {
        city_gate_mask_case(number);
        return;
    }
    int rest = number;
    const auto position = mask_positions[rest % 3]; rest /= 3;
    const auto color = colors[rest % 3]; rest /= 3;
    const auto color_flags = rest % 2 ? 7u : 0u; rest /= 2;
    const auto blend = static_cast<std::uint8_t>(rest);
    std::printf("%d flags=%u blend=%u color=%08x position=%d,%d", number, color_flags, blend, color, position.sx,
        position.sy);
    std::fflush(stdout);
    void* source = make_source(true, color_flags);
    void* mask = api.create(8, 8, 0x20);
    LockedTexture pixels{};
    if (mask && api.lock(mask, &pixels, 2)) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) pixels.pixels[y * pixels.pitch + x] = static_cast<std::uint8_t>(y * 36 + x * 5);
        api.unlock(mask);
    }
    void* target = make_target(12);
    *(static_cast<std::uint8_t*>(source) + 0x37) = blend;
    reset_state(12);
    api.set_color(color);
    api.set_source(source);
    api.set_target(target);
    const int result = api.draw_masked(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(mask)), position.sx,
        position.sy, position.dx, position.dy, position.width, position.height);
    reset_state(12);
    std::printf(" result=%d", result);
    print_target(target, 12);
    api.free_texture(source);
    api.free_texture(mask);
    api.free_texture(target);
}

// blt: surface formats x flags x same or separate surfaces x position.
constexpr std::array<Position, 4> blt_positions = {{
    {0, 0, 2, 2, 8, 8}, {1, 1, 0, 0, 8, 8}, {0, 0, -2, 7, 8, 8}, {-2, -1, 1, 1, 8, 8}}};
constexpr int blt_cases = 3 * 8 * 2 * 4;
void blt_case(int number) {
    int rest = number;
    const auto position = blt_positions[rest % 4]; rest /= 4;
    const bool same = rest % 2 != 0; rest /= 2;
    const auto flags = static_cast<std::uint32_t>(rest % 8); rest /= 8;
    const int formats = rest;  // 0 BGRA to BGRA, 1 indexed to BGRA, 2 indexed to indexed
    std::printf("%d formats=%d flags=%u same=%d position=%d,%d,%d,%d", number, formats, flags, same ? 1 : 0,
        position.sx, position.sy, position.dx, position.dy);
    std::fflush(stdout);
    if (formats != 0) {
        if (same && formats == 1) return;
        void* indexed_target = formats == 2 ? make_source(true, 0, 12) : make_target(12);
        void* indexed_source = same ? indexed_target : make_source(true, 0, 12);
        reset_state(12);
        api.set_source(indexed_source);
        api.set_target(indexed_target);
        api.blt(position.sx, position.sy, position.dx, position.dy, position.width, position.height, flags);
        reset_state(12);
        if (formats == 1) {
            print_target(indexed_target, 12);
            if (!same) api.free_texture(indexed_source);
            api.free_texture(indexed_target);
            return;
        }
        LockedTexture pixels{};
        if (api.lock(indexed_target, &pixels, 1)) {
            std::uint64_t hash = 1469598103934665603ull;
            const auto bytes = static_cast<std::size_t>(formats == 2 ? 12 : 48);
            for (int y = 0; y < 12; ++y) hash = fnv(pixels.pixels + y * pixels.pitch, bytes, hash);
            std::printf(" %016llx", static_cast<unsigned long long>(hash));
            api.unlock(indexed_target);
        }
        if (!same) api.free_texture(indexed_source);
        api.free_texture(indexed_target);
        return;
    }
    void* target = make_target(12);
    void* source = same ? target : make_target(12);
    if (!same) {
        LockedTexture pixels{};
        if (api.lock(source, &pixels, 2)) {
            for (int y = 0; y < 12; ++y)
                for (int x = 0; x < 12; ++x) {
                    const auto value = source_pixel(x, y);
                    std::memcpy(pixels.pixels + y * pixels.pitch + x * 4, &value, 4);
                }
            api.unlock(source);
        }
    }
    reset_state(12);
    api.set_source(source);
    api.set_target(target);
    api.blt(position.sx, position.sy, position.dx, position.dy, position.width, position.height, flags);
    reset_state(12);
    print_target(target, 12);
    if (!same) api.free_texture(source);
    api.free_texture(target);
}

// bltrc: flags x clipping mode, copying the clipper's rectangles.
constexpr int bltrc_cases = 8 * 2;
void bltrc_case(int number) {
    const int mode = number % 2 ? 3 : 2;
    const auto flags = static_cast<std::uint32_t>(number / 2);
    std::printf("%d flags=%u mode=%d", number, flags, mode);
    std::fflush(stdout);
    void* target = make_target(12);
    void* source = make_target(12);
    LockedTexture pixels{};
    if (api.lock(source, &pixels, 2)) {
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x) {
                const auto value = source_pixel(x, y);
                std::memcpy(pixels.pixels + y * pixels.pitch + x * 4, &value, 4);
            }
        api.unlock(source);
    }
    reset_state(12);
    static void* clipper = api.create_clipper();
    void* previous_clipper = api.set_clipper(clipper);
    api.reset_clipper();
    api.add_clip(2, 2, 6, 12);
    api.add_clip(7, 1, 11, 5);
    api.sub_clip(3, 3, 5, 5);
    api.clip_rect(1, 0, 10, 9);
    api.clip_mode(mode);
    api.set_source(source);
    api.set_target(target);
    api.blt_rc(flags);
    api.reset_clipper();
    api.set_clipper(previous_clipper);
    reset_state(12);
    print_target(target, 12);
    api.free_texture(source);
    api.free_texture(target);
}

// nodest: each drawing call with an indexed destination, which is never selected, and the source left unchanged.
constexpr int nodest_cases = 6;
void nodest_case(int number) {
    std::printf("%d call=%d", number, number);
    std::fflush(stdout);
    void* target = make_source(true, 0, 12);
    void* source = make_source(false, 0);
    void* mask = api.create(8, 8, 0x20);
    void* indexed_source = make_source(true, 0);
    reset_state(12);
    api.set_source(number == 4 ? indexed_source : source);
    api.set_target(target);
    if (number == 0) api.draw(0, 0, 2, 2, 8, 8);
    if (number == 1) api.stretch(0, 0, 1, 1, 8, 8, 10, 9);
    if (number == 2) api.fill(1, 1, 6, 6);
    if (number == 3) api.clear(1, 0xff102030u, 1.0f);
    if (number == 4) api.draw_masked(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(mask)), 0, 0, 1, 1, 8, 8);
    if (number == 5) api.blt(0, 0, 1, 1, 8, 8, 0);
    reset_state(12);
    LockedTexture pixels{};
    if (api.lock(target, &pixels, 1)) {
        std::uint64_t hash = 1469598103934665603ull;
        for (int y = 0; y < 12; ++y) hash = fnv(pixels.pixels + y * pixels.pitch, 12, hash);
        std::printf(" %016llx", static_cast<unsigned long long>(hash));
        api.unlock(target);
    }
    api.free_texture(mask);
    api.free_texture(indexed_source);
    api.free_texture(source);
    api.free_texture(target);
}

// clear: flags x color x clip x blend mode.
constexpr std::array<std::uint32_t, 3> clear_colors = {0x80402010u, 0xff402010u, 0x00402010u};
constexpr int clear_cases = 8 * 3 * 2 * 3;
void clear_case(int number) {
    int rest = number;
    const auto mode = static_cast<std::uint32_t>(1 + rest % 3); rest /= 3;
    const bool clipped = rest % 2 != 0; rest /= 2;
    const auto color = clear_colors[rest % 3]; rest /= 3;
    const auto flags = static_cast<std::uint32_t>(rest);
    std::printf("%d flags=%u color=%08x clipped=%d mode=%u", number, flags, color, clipped ? 1 : 0, mode);
    std::fflush(stdout);
    void* target = make_target(12);
    reset_state(12);
    if (clipped) api.clip_rect(3, 2, 9, 7);
    const auto previous = api.set_blend_mode(mode);
    api.set_target(target);
    api.clear(flags, color, 1.0f);
    api.set_blend_mode(previous);
    reset_state(12);
    print_target(target, 12);
    api.free_texture(target);
}

// lock: lock flags x texture format.
constexpr int lock_cases = 6 * 2;
void lock_case(int number) {
    const bool indexed = number % 2 != 0;
    const int flags = number / 2;
    std::printf("%d %s flags=%d", number, indexed ? "indexed" : "bgra", flags);
    std::fflush(stdout);
    void* texture = make_source(indexed, 0);
    LockedTexture pixels{};
    const int result = api.lock(texture, &pixels, flags);
    std::printf(" result=%d", result);
    if (result) {
        std::printf(" %ux%u pitch=%u palette=%d", pixels.width, pixels.height, pixels.pitch, pixels.palette ? 1 : 0);
        api.unlock(texture);
    }
    api.free_texture(texture);
}

// text: font kind x text mode x clipping mode x DrawText or DrawText_Rect x color, with a font from the archive.
constexpr std::array<int, 6> text_clip_modes = {0, 1, 2, 3, 4, -1};
constexpr int text_cases = 3 * 5 * 6 * 2 * 3;
void text_case(int number) {
    int rest = number;
    const auto color = colors[rest % 3]; rest /= 3;
    const bool use_rect = rest % 2 != 0; rest /= 2;
    const int clip = text_clip_modes[rest % 6]; rest /= 6;
    const int mode = rest % 5; rest /= 5;
    const int kind = rest;
    std::printf("%d font=%d mode=%d clip=%d rect=%d color=%08x", number, kind, mode, clip, use_rect ? 1 : 0, color);
    std::fflush(stdout);
    static void* image = api.load("fonts/tiepolo_black16.aim", 3);
    static const auto characters = reinterpret_cast<const unsigned char*>(" ABCD");
    static void* fonts[3] = {
        api.font_proportional(characters, image, 15, 16, 0, 4),
        api.font_kerning(characters, image, 15, 16, 1, 5),
        api.font_fixed(characters, image, 15, 16, 12)};
    static void* clipper = api.create_clipper();
    void* target = make_target(80);
    reset_state(80);
    void* previous_clipper = api.set_clipper(clipper);
    api.reset_clipper();
    api.add_clip(0, 0, 38, 20);
    api.add_clip(25, 4, 72, 25);
    api.sub_clip(12, 7, 18, 13);
    api.clip_rect(4, 3, 72, 27);
    api.clip_mode(clip);
    api.set_color(color);
    api.set_font(fonts[kind]);
    api.text_mode(mode);
    api.set_target(target);
    const auto* text = reinterpret_cast<const unsigned char*>("ABCD ABC DA");
    const int x = mode >= 2 ? 50 : 3;
    if (use_rect) {
        const std::array<std::int32_t, 4> rectangle{20, 4, 55, 20};
        api.draw_text_rect(x, 5, rectangle.data(), text);
    } else {
        api.draw_text(x, 5, text);
    }
    api.text_mode(1);
    api.reset_clipper();
    api.set_clipper(previous_clipper);
    reset_state(80);
    print_target(target, 80);
    api.free_texture(target);
}

// assets: every image of the game archive x load flags.
constexpr std::array<std::uint32_t, 3> asset_flags = {1u, 7u, 0x00010040u};
int asset_cases() { return static_cast<int>(names.size() * asset_flags.size()); }
void asset_case(int number) {
    const auto& name = names[static_cast<std::size_t>(number) / asset_flags.size()];
    const auto flags = asset_flags[static_cast<std::size_t>(number) % asset_flags.size()];
    std::printf("%d %s flags=0x%x", number, name.c_str(), flags);
    std::fflush(stdout);
    api.set_key(key);
    void* image = api.load(name.c_str(), flags);
    if (!image) { std::printf(" null"); return; }
    std::uint32_t info[3] = {};
    api.texture_info(image, info);
    std::printf(" info=%u,%u", info[0], info[1]);
    LockedTexture pixels{};
    if (!api.lock(image, &pixels, 1)) {
        std::printf(" lockfail");
    } else {
        const std::uint32_t bytes = pixels.palette ? pixels.width : pixels.width * 4;
        std::uint64_t hash = 1469598103934665603ull;
        for (std::uint32_t y = 0; y < pixels.height; ++y) hash = fnv(pixels.pixels + y * pixels.pitch, bytes, hash);
        std::printf(" %ux%u palette=%d pixels=%016llx", pixels.width, pixels.height, pixels.palette ? 1 : 0,
            static_cast<unsigned long long>(hash));
        if (pixels.palette)
            std::printf(" colors=%016llx", static_cast<unsigned long long>(
                fnv(reinterpret_cast<const std::uint8_t*>(pixels.palette), 1024)));
        api.unlock(image);
    }
    api.free_texture(image);
}

struct Suite { const char* name; int (*count)(); void (*run)(int); };
const std::array<Suite, 13> suites = {{
    {"blend", [] { return blend_cases; }, blend_case},
    {"stretch", [] { return stretch_cases; }, stretch_case},
    {"clip", [] { return clip_cases; }, clip_case},
    {"fill", [] { return fill_cases; }, fill_case},
    {"fillrow", [] { return fillrow_cases; }, fillrow_case},
    {"mask", [] { return mask_matrix_cases + city_gate_mask_cases; }, mask_case},
    {"blt", [] { return blt_cases; }, blt_case},
    {"bltrc", [] { return bltrc_cases; }, bltrc_case},
    {"clear", [] { return clear_cases; }, clear_case},
    {"nodest", [] { return nodest_cases; }, nodest_case},
    {"lock", [] { return lock_cases; }, lock_case},
    {"text", [] { return text_cases; }, text_case},
    {"assets", asset_cases, asset_case},
}};

// Runs one case; a fault inside the library is reported instead of ending the run.
bool guarded(void (*run)(int), int number) {
    __try {
        run(number);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 6) {
        std::fputs("Usage: sgl_sweep_oracle <SGL DLL> <game directory> <suite> <first case|count> <last case> "
            "[--dump] [--names file]\n", stderr);
        return 2;
    }
    const std::wstring game(argv[2]);
    const Suite* suite = nullptr;
    char suite_name[32] = {};
    std::snprintf(suite_name, sizeof(suite_name), "%ls", argv[3]);
    for (const auto& candidate : suites) if (std::strcmp(candidate.name, suite_name) == 0) suite = &candidate;
    if (!suite) { std::fprintf(stderr, "Unknown suite %s\n", suite_name); return 2; }
    for (int index = 6; index < argc; ++index) {
        if (std::wcscmp(argv[index], L"--dump") == 0) dump = true;
        else if (std::wcscmp(argv[index], L"--names") == 0 && index + 1 < argc) {
            FILE* file = nullptr;
            if (_wfopen_s(&file, argv[++index], L"rb") != 0 || !file) {
                std::fputs("Cannot open the names file\n", stderr);
                return 2;
            }
            char line[512];
            while (std::fgets(line, sizeof(line), file)) {
                std::string name(line);
                while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
                if (!name.empty()) names.push_back(name);
            }
            std::fclose(file);
        }
    }
    if (std::wcscmp(argv[4], L"count") == 0) { std::printf("%d\n", suite->count()); return 0; }
    const int first = std::wcstol(argv[4], nullptr, 10);
    const int last = std::min(std::wcstol(argv[5], nullptr, 10), static_cast<long>(suite->count() - 1));

    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetDllDirectoryW(game.c_str());
    if (!LoadLibraryW((game + L"\\AIM.dll").c_str()) || !LoadLibraryW((game + L"\\archiver.dll").c_str())) {
        std::fprintf(stderr, "Cannot load AIM.dll or archiver.dll from %ls\n", game.c_str());
        return 3;
    }
    std::array<wchar_t, MAX_PATH> full{};
    if (!GetFullPathNameW(argv[1], static_cast<DWORD>(full.size()), full.data(), nullptr)) return 3;
    SetCurrentDirectoryW(game.c_str());
    const HMODULE library = LoadLibraryW(full.data());
    if (!library) { std::fprintf(stderr, "Cannot load %ls: %lu\n", full.data(), GetLastError()); return 3; }
    const bool resolved =
        resolve(library, "sgl_InitHardware", api.initialize) && resolve(library, "sgl_SetColorKey", api.set_key) &&
        resolve(library, "sgl_CreateMemoryTexture", api.create) &&
        resolve(library, "sgl_FreeMemoryTexture", api.free_texture) && resolve(library, "sgl_LockTexture", api.lock) &&
        resolve(library, "sgl_UnlockTexture", api.unlock) && resolve(library, "sgl_ChangePalette", api.change_palette) &&
        resolve(library, "sgl_SetRenderSource", api.set_source) && resolve(library, "sgl_SetRenderDest", api.set_target) &&
        resolve(library, "sgl_Set2dClippingMode", api.clip_mode) && resolve(library, "sgl_SetClipRect_4i", api.clip_rect) &&
        resolve(library, "sgl_ResetClipper", api.reset_clipper) && resolve(library, "sgl_AddClipRect_4i", api.add_clip) &&
        resolve(library, "sgl_SubClipRect_4i", api.sub_clip) && resolve(library, "sgl_CreateClipper", api.create_clipper) &&
        resolve(library, "sgl_SetActiveClipper", api.set_clipper) && resolve(library, "sgl_FreeClipper", api.free_clipper) &&
        resolve(library, "sgl_MemoryFont_CreateProportional", api.font_proportional) &&
        resolve(library, "sgl_MemoryFont_CreateKerning", api.font_kerning) &&
        resolve(library, "sgl_MemoryFont_CreateFixed", api.font_fixed) && resolve(library, "sgl_SetFont", api.set_font) &&
        resolve(library, "sgl_SetTextMode", api.text_mode) && resolve(library, "sgl_DrawText", api.draw_text) &&
        resolve(library, "sgl_DrawText_Rect", api.draw_text_rect) && resolve(library, "sgl_SetConstantColor", api.set_color) &&
        resolve(library, "sgl_SetBlendMode", api.set_blend_mode) && resolve(library, "sgl_DrawBitmapRect", api.draw) &&
        resolve(library, "sgl_StretchBitmapRect", api.stretch) &&
        resolve(library, "sgl_DrawBitmapRectWithAlphaMask", api.draw_masked) &&
        resolve(library, "sgl_FillSolidRect", api.fill) && resolve(library, "sgl_Blt", api.blt) && resolve(library, "sgl_Blt_RC", api.blt_rc) &&
        resolve(library, "sgl_ClearBuffer", api.clear) && resolve(library, "sgl_LoadMemoryTexture", api.load) &&
        resolve(library, "sgl_GetTextureInfo", api.texture_info) && resolve(library, "sgl_SetArchive", api.set_archive);
    if (!resolved) return 4;
    api.set_key(key);
    if (!api.initialize()) return 5;
    api.set_key(key);
    if (std::strcmp(suite->name, "assets") == 0 || std::strcmp(suite->name, "text") == 0 ||
        (std::strcmp(suite->name, "mask") == 0 && last >= mask_matrix_cases)) {
        const HMODULE archiver = GetModuleHandleW(L"archiver.dll");
        void* (__cdecl* create_container)(int) = nullptr;
        int (__cdecl* append_archive)(void*, const char*) = nullptr;
        if (!archiver || !resolve(archiver, "arc_CreateContainer", create_container) ||
            !resolve(archiver, "arc_AppendArchive", append_archive)) return 6;
        void* container = create_container(0);
        if (!container || !append_archive(container, "p2arch0_eng.cpr")) return 7;
        api.set_archive(container);
    }
    for (int number = first; number <= last; ++number) {
        // A fault can leave the library in any state, so the run ends after it and continues in a new process.
        if (!guarded(suite->run, number)) {
            std::printf(" fault\n");
            std::fflush(stdout);
            return 9;
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    return 0;
}
