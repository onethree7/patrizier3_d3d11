#include "compositor.hpp"

#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
struct LockedTexture {
    std::uint8_t* pixels;
    std::uint32_t* palette;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
};
static_assert(sizeof(LockedTexture) == 20);
struct DrawCase {
    int sx, sy, dx, dy, width, height;
    int destination_width, destination_height;
    RECT clip;
    std::optional<unsigned> changed;
};

struct StretchFixture {
    int source_width = 272, source_height = 155;
    int target_width = 996, target_height = 982;
    DrawCase draw{0, 0, -2023, -538, 272, 155, 4369, 2497, {0, 0, 996, 982}, std::nullopt};
    std::uint32_t source_flags = 0, target_flags = 0x41, color = 0xffffffffu;
    int clip_mode = 3;
    int screen_width = 1920, screen_height = 1080, screen_x = 0, screen_y = 42;
};

bool read_stretch_fixture(const wchar_t* path, StretchFixture& fixture) {
    std::ifstream input(path);
    std::string version, extra;
    auto& draw = fixture.draw;
    if (!(input >> version) || version != "P3_STRETCH_V1" ||
        !(input >> fixture.source_width >> fixture.source_height >> fixture.target_width >> fixture.target_height >>
            draw.sx >> draw.sy >> draw.dx >> draw.dy >> draw.width >> draw.height >>
            draw.destination_width >> draw.destination_height >>
            draw.clip.left >> draw.clip.top >> draw.clip.right >> draw.clip.bottom >>
            fixture.source_flags >> fixture.target_flags >> fixture.color >> fixture.clip_mode >>
            fixture.screen_width >> fixture.screen_height >> fixture.screen_x >> fixture.screen_y) || input >> extra)
        return false;
    for (int size : {fixture.source_width, fixture.source_height, fixture.target_width, fixture.target_height,
            draw.width, draw.height, draw.destination_width, draw.destination_height,
            fixture.screen_width, fixture.screen_height}) {
        if (size <= 0 || size > 65535) return false;
    }
    return input.eof() && draw.sx >= 0 && draw.sy >= 0 &&
        static_cast<std::int64_t>(draw.sx) + draw.width <= fixture.source_width &&
        static_cast<std::int64_t>(draw.sy) + draw.height <= fixture.source_height &&
        draw.clip.left < draw.clip.right && draw.clip.top < draw.clip.bottom &&
        (fixture.source_flags & 0x20) == 0 && (fixture.target_flags & 0x20) == 0 &&
        fixture.clip_mode >= 0 && fixture.clip_mode <= 3;
}

template<typename T> T resolve(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 7) {
        std::fputs("Usage: original_sgl_oracle <reference game directory> <replacement DLL> [BGRA32 source [BGRA32 intermediate | destination-before clip-rectangles.txt [stretch-parameters.txt]]]\n", stderr);
        return 2;
    }
    StretchFixture stretch_fixture;
    if (argc >= 6) stretch_fixture.draw.dx = -3349;
    if (argc == 7 && !read_stretch_fixture(argv[6], stretch_fixture)) {
        std::fwprintf(stderr, L"Invalid stretch fixture: %ls\n", argv[6]);
        return 2;
    }
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    if (!SetDllDirectoryW(argv[1]) || !SetCurrentDirectoryW(argv[1])) return 3;
    const std::wstring library_path = std::wstring(argv[1]) + L"\\ddraw_Dll.dll";
    const HMODULE library = LoadLibraryW(library_path.c_str());
    if (!library) {
        std::fprintf(stderr, "Cannot load reference SGL: %lu\n", GetLastError());
        return 4;
    }
    const HMODULE replacement = LoadLibraryW(argv[2]);
    if (!replacement || replacement == library) return 28;
    const auto reference_archive = resolve<void* (__cdecl*)(void*)>(library, "sgl_SetArchive");
    const auto replacement_archive = resolve<void* (__cdecl*)(void*)>(replacement, "sgl_SetArchive");
    if (!reference_archive || !replacement_archive) return 29;
    reference_archive(nullptr);
    replacement_archive(nullptr);
    for (void* value : std::array<void*, 4>{reinterpret_cast<void*>(0x12345678), reinterpret_cast<void*>(0x23456789), nullptr, nullptr}) {
        if (reference_archive(value) != replacement_archive(value)) return 30;
    }
    std::puts("Archive state: original and replacement agree.");
    const auto original_info = resolve<void (__cdecl*)(const void*, std::uint32_t*)>(library, "sgl_GetTextureInfo");
    const auto replacement_info = resolve<void (__cdecl*)(const void*, std::uint32_t*)>(replacement, "sgl_GetTextureInfo");
    if (!original_info || !replacement_info) return 31;
    std::array<std::uint8_t, 0x24> object = {};
    object[0x20] = 0x34; object[0x21] = 0x12;
    object[0x22] = 0xcd; object[0x23] = 0xab;
    for (const void* value : std::array<const void*, 2>{nullptr, object.data()}) {
        std::array<std::uint32_t, 3> original_output = {9, 9, 9};
        std::array<std::uint32_t, 3> replacement_output = {9, 9, 9};
        original_info(value, original_output.data());
        replacement_info(value, replacement_output.data());
        if (original_output != replacement_output || (value &&
            (original_output[0] != 0x1234 || original_output[1] != 0xabcd))) return 32;
    }
    std::puts("Texture information: original and replacement agree.");
    const auto initialize = resolve<int (__cdecl*)()>(library, "sgl_InitHardware");
    const auto replacement_initialize = resolve<int (__cdecl*)()>(replacement, "sgl_InitHardware");
    const auto create = resolve<void* (__cdecl*)(std::uint16_t, std::uint16_t, std::uint32_t, int)>(library, "sgl_CreateTexture");
    const auto free_texture = resolve<void (__cdecl*)(void*)>(library, "sgl_FreeTexture");
    const auto texture_info = resolve<void (__cdecl*)(void*, std::uint32_t*)>(library, "sgl_GetTextureInfo");
    const auto lock = resolve<int (__cdecl*)(void*, LockedTexture*, int)>(library, "sgl_LockTexture");
    const auto unlock = resolve<void (__cdecl*)(void*)>(library, "sgl_UnlockTexture");
    const auto set_key = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(library, "sgl_SetColorKey");
    const auto replacement_key = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(replacement, "sgl_SetColorKey");
    const auto old_texture_key = resolve<std::uint32_t (__cdecl*)(void*)>(library, "sgl_GetColorKey");
    const auto new_texture_key = resolve<std::uint32_t (__cdecl*)(void*)>(replacement, "sgl_GetColorKey");
    const auto set_palette = resolve<int (__cdecl*)(void*, std::uint32_t*)>(library, "sgl_ChangePalette");
    const auto replacement_palette = resolve<int (__cdecl*)(void*, std::uint32_t*)>(replacement, "sgl_ChangePalette");
    const auto change_texture_flags = resolve<std::uint32_t (__cdecl*)(void*, std::uint32_t)>(library, "sgl_ChangeTextureFlags");
    const auto replacement_change_texture_flags = resolve<std::uint32_t (__cdecl*)(void*, std::uint32_t)>(replacement, "sgl_ChangeTextureFlags");
    const auto set_source = resolve<void* (__cdecl*)(void*)>(library, "sgl_SetRenderSource");
    const auto set_target = resolve<void* (__cdecl*)(void*)>(library, "sgl_SetRenderDest");
    const auto clip_mode = resolve<int (__cdecl*)(int)>(library, "sgl_Set2dClippingMode");
    const auto clip_rect = resolve<void (__cdecl*)(int, int, int, int)>(library, "sgl_SetClipRect_4i");
    const auto replacement_clip_mode = resolve<int (__cdecl*)(int)>(replacement, "sgl_Set2dClippingMode");
    const auto get_clip_mode = resolve<int (__cdecl*)()>(library, "sgl_Get2dClippingMode");
    const auto replacement_get_clip_mode = resolve<int (__cdecl*)()>(replacement, "sgl_Get2dClippingMode");
    const auto replacement_clip_rect = resolve<void (__cdecl*)(int, int, int, int)>(replacement, "sgl_SetClipRect_4i");
    const auto set_clip_pointer = resolve<void (__cdecl*)(const std::int32_t*)>(library, "sgl_SetClipRect_r");
    const auto replacement_clip_pointer = resolve<void (__cdecl*)(const std::int32_t*)>(replacement, "sgl_SetClipRect_r");
    const auto get_clip_rect = resolve<void (__cdecl*)(std::int32_t*)>(library, "sgl_GetClipRect_r");
    const auto replacement_get_clip_rect = resolve<void (__cdecl*)(std::int32_t*)>(replacement, "sgl_GetClipRect_r");
    const auto old_get_clip_4i = resolve<void (__cdecl*)(std::int32_t*, std::int32_t*, std::int32_t*, std::int32_t*)>(library, "sgl_GetClipRect_4i");
    const auto new_get_clip_4i = resolve<void (__cdecl*)(std::int32_t*, std::int32_t*, std::int32_t*, std::int32_t*)>(replacement, "sgl_GetClipRect_4i");
    const auto old_get_color = resolve<std::uint32_t (__cdecl*)()>(library, "sgl_GetConstantColor");
    const auto new_get_color = resolve<std::uint32_t (__cdecl*)()>(replacement, "sgl_GetConstantColor");
    const auto old_set_color = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(library, "sgl_SetConstantColor");
    const auto draw = resolve<void (__cdecl*)(int, int, int, int, int, int)>(library, "sgl_DrawBitmapRect");
    const auto stretch = resolve<void (__cdecl*)(int, int, int, int, int, int, int, int)>(library, "sgl_StretchBitmapRect");
    const auto old_blend = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(library, "sgl_SetBlendMode");
    const auto new_blend = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(replacement, "sgl_SetBlendMode");
    if (!initialize || !replacement_initialize || !create || !free_texture || !texture_info || !lock || !unlock || !set_key || !replacement_key || !set_palette || !replacement_palette || !change_texture_flags || !replacement_change_texture_flags || !set_source ||
        !set_target || !clip_mode || !clip_rect || !draw || !stretch || !replacement_clip_mode || !get_clip_mode ||
        !replacement_get_clip_mode || !replacement_clip_rect || !set_clip_pointer || !replacement_clip_pointer ||
        !get_clip_rect || !replacement_get_clip_rect || !old_get_clip_4i || !new_get_clip_4i || !old_get_color || !new_get_color || !old_texture_key || !new_texture_key) return 5;
    if (!old_blend || !new_blend || !old_set_color) return 5;
    const auto key_before_init = set_key(0xabcd1234);
    if (replacement_key(0xabcd1234) != key_before_init) return 48;
    std::printf("Color key before initialization: 0x%08x\n", key_before_init);
    if (!initialize()) {
        std::fputs("Reference SGL initialization failed.\n", stderr);
        return 6;
    }
    if (!replacement_initialize() || !replacement_initialize()) return 118;
    const auto old_font = resolve<void (__cdecl*)(const void*)>(library, "sgl_SetFont");
    const auto new_font = resolve<void (__cdecl*)(const void*)>(replacement, "sgl_SetFont");
    const auto old_strlen = resolve<int (__cdecl*)(const unsigned char*)>(library, "sgl_StrLen");
    const auto new_strlen = resolve<int (__cdecl*)(const unsigned char*)>(replacement, "sgl_StrLen");
    if (!old_font || !new_font || !old_strlen || !new_strlen) return 130;
    std::array<std::uint8_t, 0x128> font{};
    std::array<std::uint8_t, 4> glyph_widths{3, 5, 7, 11};
    std::array<std::uint8_t, 16> pairs{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const std::uint32_t count = 4;
    const auto* width_ptr = glyph_widths.data();
    const auto* pair_ptr = pairs.data();
    std::memcpy(font.data() + 0x10, &count, sizeof(count));
    std::memcpy(font.data() + 0x14, &width_ptr, sizeof(width_ptr));
    font[0x20 + 'A'] = 1;
    font[0x20 + 'B'] = 2;
    font[0x20 + 0xe4] = 3;
    const std::array<const unsigned char*, 5> texts = {
        reinterpret_cast<const unsigned char*>(""), reinterpret_cast<const unsigned char*>("A"),
        reinterpret_cast<const unsigned char*>("ABBA"), reinterpret_cast<const unsigned char*>("?A"),
        reinterpret_cast<const unsigned char*>("\xe4" "B")};
    old_font(nullptr);
    new_font(nullptr);
    for (const auto* text : texts) if (old_strlen(text) != new_strlen(text)) return 131;
    for (int with_kerning = 0; with_kerning < 2; ++with_kerning) {
        if (with_kerning) std::memcpy(font.data() + 0x18, &pair_ptr, sizeof(pair_ptr));
        old_font(font.data());
        new_font(font.data());
        for (const auto* text : texts) if (old_strlen(text) != new_strlen(text)) return 132;
    }
    old_font(nullptr);
    new_font(nullptr);
    const auto previous_key = set_key(0xabff00ff);
    if (replacement_key(0xabff00ff) != previous_key) return 119;
    std::printf("Initial color key: 0x%08x\n", previous_key);
    if (clip_mode(0) != replacement_clip_mode(0)) return 64;
    for (int mode : {1, 2, 3, 0, -1}) {
        if (clip_mode(mode) != replacement_clip_mode(mode) || get_clip_mode() != replacement_get_clip_mode()) return 65;
    }
    const std::array<std::int32_t, 4> rectangle = {-10, 7, 8192, 42};
    clip_rect(rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
    replacement_clip_rect(rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
    std::array<std::int32_t, 4> old_clip = {}, new_clip = {};
    get_clip_rect(old_clip.data());
    replacement_get_clip_rect(new_clip.data());
    if (old_clip != rectangle || new_clip != rectangle) return 66;
    old_get_clip_4i(&old_clip[0], &old_clip[1], &old_clip[2], &old_clip[3]);
    new_get_clip_4i(&new_clip[0], &new_clip[1], &new_clip[2], &new_clip[3]);
    if (old_clip != rectangle || new_clip != rectangle) return 125;
    const std::array<std::int32_t, 4> empty_rectangle = {8, 6, 3, -7};
    set_clip_pointer(empty_rectangle.data());
    replacement_clip_pointer(empty_rectangle.data());
    get_clip_rect(old_clip.data());
    replacement_get_clip_rect(new_clip.data());
    if (old_clip != empty_rectangle || new_clip != empty_rectangle) return 67;
    old_get_clip_4i(&old_clip[0], &old_clip[1], &old_clip[2], &old_clip[3]);
    new_get_clip_4i(&new_clip[0], &new_clip[1], &new_clip[2], &new_clip[3]);
    if (old_clip != empty_rectangle || new_clip != empty_rectangle) return 126;
    const auto old_create_region = resolve<void* (__cdecl*)()>(library, "sgl_CreateClipper");
    const auto new_create_region = resolve<void* (__cdecl*)()>(replacement, "sgl_CreateClipper");
    const auto old_set_region = resolve<void* (__cdecl*)(void*)>(library, "sgl_SetActiveClipper");
    const auto new_set_region = resolve<void* (__cdecl*)(void*)>(replacement, "sgl_SetActiveClipper");
    const auto old_free_region = resolve<void (__cdecl*)(void*)>(library, "sgl_FreeClipper");
    const auto new_free_region = resolve<void (__cdecl*)(void*)>(replacement, "sgl_FreeClipper");
    const auto old_add = resolve<void (__cdecl*)(const RECT*)>(library, "sgl_AddClipRect_r");
    const auto new_add = resolve<void (__cdecl*)(const RECT*)>(replacement, "sgl_AddClipRect_r");
    const auto old_sub = resolve<void (__cdecl*)(const RECT*)>(library, "sgl_SubClipRect_r");
    const auto new_sub = resolve<void (__cdecl*)(const RECT*)>(replacement, "sgl_SubClipRect_r");
    const auto old_copy_region = resolve<void (__cdecl*)(void*)>(library, "sgl_CopyClipper");
    const auto new_copy_region = resolve<void (__cdecl*)(void*)>(replacement, "sgl_CopyClipper");
    const auto old_visible = resolve<int (__cdecl*)(const RECT*)>(library, "sgl_IsRectInClippingRegion");
    const auto new_visible = resolve<int (__cdecl*)(const RECT*)>(replacement, "sgl_IsRectInClippingRegion");
    const auto old_reset = resolve<void (__cdecl*)()>(library, "sgl_ResetClipper");
    const auto new_reset = resolve<void (__cdecl*)()>(replacement, "sgl_ResetClipper");
    if (!old_create_region || !new_create_region || !old_set_region || !new_set_region || !old_free_region ||
        !new_free_region || !old_add || !new_add || !old_sub || !new_sub || !old_copy_region || !new_copy_region ||
        !old_visible || !new_visible || !old_reset || !new_reset) return 120;
    void* old_region = old_create_region();
    void* new_region = new_create_region();
    void* old_source_region = old_create_region();
    void* new_source_region = new_create_region();
    if (!old_region || !new_region || !old_source_region || !new_source_region) return 121;
    void* old_prior_region = old_set_region(old_region);
    void* new_prior_region = new_set_region(new_region);
    clip_mode(2);
    replacement_clip_mode(2);
    const RECT first_region{1, 1, 7, 6}, second_region{5, 3, 10, 9}, cut_region{3, 2, 6, 5};
    for (int step = 0; step < 5; ++step) {
        if (step == 1) { old_add(&first_region); new_add(&first_region); }
        if (step == 2) { old_add(&second_region); new_add(&second_region); }
        if (step == 3) { old_sub(&cut_region); new_sub(&cut_region); }
        if (step == 4) { old_reset(); new_reset(); }
        for (int y = 0; y < 11; ++y) for (int x = 0; x < 12; ++x) {
            const RECT pixel{x, y, x + 1, y + 1};
            if ((old_visible(&pixel) != 0) != (new_visible(&pixel) != 0)) return 122;
        }
        for (const RECT query : std::array<RECT, 5>{{{-2, -2, 2, 2}, {2, 2, 6, 5}, {7, 0, 12, 4}, {0, 0, 12, 11}, {10, 9, 12, 11}}}) {
            if ((old_visible(&query) != 0) != (new_visible(&query) != 0)) return 123;
        }
    }
    const std::array<RECT, 2> sentinel_regions{{
        {0x0c0c0c0c, 0x0c0c0c0c, 0x0c0c0c0d, 0x0c0c0c0d},
        {0x10101010, 0x10101010, 0x10101011, 0x10101011}}};
    for (const auto& region : sentinel_regions) {
        old_add(&region);
        new_add(&region);
        if (old_visible(&region) == 0 || new_visible(&region) == 0) return 180;
    }
    const RECT origin_pixel{0, 0, 1, 1};
    if (old_visible(&origin_pixel) != 0 || new_visible(&origin_pixel) != 0) return 181;
    old_sub(&sentinel_regions[0]);
    new_sub(&sentinel_regions[0]);
    if (old_visible(&sentinel_regions[0]) != 0 || new_visible(&sentinel_regions[0]) != 0 ||
        old_visible(&sentinel_regions[1]) == 0 || new_visible(&sentinel_regions[1]) == 0) return 182;
    old_reset();
    new_reset();
    if (old_visible(&sentinel_regions[1]) != 0 || new_visible(&sentinel_regions[1]) != 0) return 183;

    const RECT source_first{2, 3, 6, 8}, source_second{9, 1, 12, 5}, destination_only{20, 20, 22, 22};
    old_set_region(old_source_region);
    new_set_region(new_source_region);
    old_add(&source_first);
    old_add(&source_second);
    new_add(&source_first);
    new_add(&source_second);
    old_set_region(old_region);
    new_set_region(new_region);
    old_add(&destination_only);
    new_add(&destination_only);
    old_copy_region(old_source_region);
    new_copy_region(new_source_region);
    for (int y = 0; y < 24; ++y) for (int x = 0; x < 24; ++x) {
        const RECT pixel{x, y, x + 1, y + 1};
        if ((old_visible(&pixel) != 0) != (new_visible(&pixel) != 0)) return 184;
    }
    if (old_visible(&destination_only) != 0 || new_visible(&destination_only) != 0) return 185;
    old_set_region(old_source_region);
    new_set_region(new_source_region);
    old_reset();
    new_reset();
    old_set_region(old_region);
    new_set_region(new_region);
    old_add(&destination_only);
    new_add(&destination_only);
    old_copy_region(old_source_region);
    new_copy_region(new_source_region);
    if (old_visible(&destination_only) != 0 || new_visible(&destination_only) != 0) return 186;
    old_set_region(old_prior_region);
    new_set_region(new_prior_region);
    old_free_region(old_source_region);
    new_free_region(new_source_region);
    old_free_region(old_region);
    new_free_region(new_region);
    std::puts("Clipper membership: original and replacement match for unions, subtraction, reset, and distant rectangles.");
    void* old_mode3_region = old_create_region();
    void* new_mode3_region = new_create_region();
    if (!old_mode3_region || !new_mode3_region) return 187;
    old_set_region(old_mode3_region);
    new_set_region(new_mode3_region);
    const RECT mode3_region{1, 1, 7, 7};
    old_add(&mode3_region);
    new_add(&mode3_region);
    clip_rect(3, 3, 6, 6);
    replacement_clip_rect(3, 3, 6, 6);
    clip_mode(3);
    replacement_clip_mode(3);
    for (const RECT query : std::array<RECT, 4>{{{1, 1, 2, 2}, {4, 4, 5, 5}, {6, 6, 7, 7}, {0, 0, 8, 8}}}) {
        const int original_visible = old_visible(&query);
        const int replacement_visible = new_visible(&query);
        if ((original_visible != 0) != (replacement_visible != 0)) {
            std::fprintf(stderr, "Mode 3 clipper membership differs: original=%d replacement=%d query=[%ld,%ld,%ld,%ld].\n",
                original_visible, replacement_visible, query.left, query.top, query.right, query.bottom);
            return 188;
        }
    }
    old_set_region(old_prior_region);
    new_set_region(new_prior_region);
    old_free_region(old_mode3_region);
    new_free_region(new_mode3_region);
    std::puts("Clipper membership: mode 3 matches the original with an active clip rectangle.");
    clip_mode(0);
    replacement_clip_mode(0);
    std::puts("Clip state: original and replacement agree.");

    const HMODULE archiver = GetModuleHandleW(L"archiver.dll");
    if (!archiver) return 33;
    const auto create_container = resolve<void* (__cdecl*)(int)>(archiver, "arc_CreateContainer");
    const auto append_archive = resolve<int (__cdecl*)(void*, const char*)>(archiver, "arc_AppendArchive");
    const auto release_container = resolve<void (__cdecl*)(void*)>(archiver, "arc_ReleaseContainer");
    const auto load_image = resolve<void* (__cdecl*)(const char*, std::uint32_t)>(library, "sgl_LoadMemoryTexture");
    const auto free_image = resolve<void (__cdecl*)(void*)>(library, "sgl_FreeMemoryTexture");
    const auto replacement_load = resolve<void* (__cdecl*)(const char*, std::uint32_t)>(replacement, "sgl_LoadMemoryTexture");
    const auto replacement_lock = resolve<int (__cdecl*)(void*, LockedTexture*, int)>(replacement, "sgl_LockTexture");
    const auto replacement_unlock = resolve<void (__cdecl*)(void*)>(replacement, "sgl_UnlockTexture");
    const auto replacement_free = resolve<void (__cdecl*)(void*)>(replacement, "sgl_FreeMemoryTexture");
    const auto old_prefetch = resolve<void (__cdecl*)(void*)>(library, "sgl_PrefetchTexture");
    const auto new_prefetch = resolve<void (__cdecl*)(void*)>(replacement, "sgl_PrefetchTexture");
    if (!create_container || !append_archive || !release_container || !load_image || !free_image ||
        !replacement_load || !replacement_lock || !replacement_unlock || !replacement_free || !old_prefetch || !new_prefetch) return 34;
    void* container = create_container(0);
    if (!container || !append_archive(container, "p2arch0_eng.cpr")) return 35;
    reference_archive(container);
    replacement_archive(container);
    if (load_image("__missing_p3_test_resource__", 1) || replacement_load("__missing_p3_test_resource__", 1)) return 42;
    for (const char* name : {"images/options/Hauptmenu.jpg", "images/options/hmenu_colormap.bmp", "images/options/p_hmenu01.bmp",
        "images/module_stadtkarte/10_03_rasen_grass_klein _5bis12.aim",
        "images/module_stadtkarte/10_03_rasen_grass_1bis6.aim", "images/module_stadtkarte/wasser_neu.aim",
        "images/Credits/Dag.bmp",
        "images/feder0003.tga", "images/sidemenu/frohr_pirat01.bmp", "fonts/tiepolo_black16.aim"}) {
        set_key(0x00ff00ff);
        replacement_key(0x00ff00ff);
        const std::uint32_t captured_flags =
            std::strcmp(name, "images/options/Hauptmenu.jpg") == 0 ||
            std::strcmp(name, "images/options/hmenu_colormap.bmp") == 0 ||
            std::strcmp(name, "images/Credits/Dag.bmp") == 0 ? 0x01000001u :
            std::strcmp(name, "images/options/p_hmenu01.bmp") == 0 ? 0x01000007u :
            std::strcmp(name, "images/feder0003.tga") == 0 ? 0x01000003u :
            std::strcmp(name, "images/sidemenu/frohr_pirat01.bmp") == 0 ? 0x02000003u : 0u;
        for (std::uint32_t flags : {1u, 3u, 7u, captured_flags, 0x00010040u, 0x05000010u}) {
            if (flags == 0) continue;
            void* image = load_image(name, flags);
            if (!image) return 36;
            std::uint32_t image_info[3] = {};
            texture_info(image, image_info);
            const auto image_object = static_cast<const std::uint8_t*>(image);
            std::uint32_t before_pixels = 0, before_palette = 0;
            std::memcpy(&before_pixels, image_object + 0x18, sizeof(before_pixels));
            std::memcpy(&before_palette, image_object + 0x1c, sizeof(before_palette));
            const auto* methods = *reinterpret_cast<const std::uintptr_t* const*>(image);
            if (!methods || methods[7] != reinterpret_cast<std::uintptr_t>(library) + 0x4030) return 183;
            old_prefetch(image);
            std::uint32_t after_prefetch = 0;
            std::memcpy(&after_prefetch, image_object + 0x18, sizeof(after_prefetch));
            if (after_prefetch != before_pixels) return 184;
            LockedTexture loaded = {};
            if (!lock(image, &loaded, 1) || !loaded.pixels || loaded.width != image_info[0] || loaded.height != image_info[1]) return 37;
            std::printf("Image %s flags=%u: %ux%u pitch=%u bpp=%u object_flags=%u palette=%s pre_pixels=%s pre_palette=%s\n",
                name, flags, loaded.width, loaded.height, loaded.pitch, image_object[0x24], image_object[0x34], loaded.palette ? "yes" : "no",
                before_pixels ? "yes" : "no", before_palette ? "yes" : "no");
            std::fflush(stdout);
            if ((flags & 7u) == 7u) {
                std::uint32_t image_key = 0;
                std::memcpy(&image_key, image_object + 8, sizeof(image_key));
                unsigned transparent = 0;
                if (loaded.palette) {
                    for (unsigned i = 0; i < 256; ++i) if ((loaded.palette[i] >> 24) == 0) ++transparent;
                } else {
                    for (std::uint32_t y = 0; y < loaded.height; ++y) {
                        const auto row = reinterpret_cast<const std::uint32_t*>(loaded.pixels + y * loaded.pitch);
                        for (std::uint32_t x = 0; x < loaded.width; ++x) if ((row[x] >> 24) == 0) ++transparent;
                    }
                }
                std::printf("  color key=%08x transparent=%u\n", image_key, transparent);
            }
            {
                void* new_image = replacement_load(name, flags);
                if (!new_image) return 38;
                std::uint32_t replacement_details[3] = {};
                replacement_info(new_image, replacement_details);
                if (replacement_details[0] != image_info[0] || replacement_details[1] != image_info[1]) return 39;
                const auto new_object = static_cast<const std::uint8_t*>(new_image);
                std::uint32_t new_before_pixels = 0, new_before_palette = 0;
                std::memcpy(&new_before_pixels, new_object + 0x18, sizeof(new_before_pixels));
                std::memcpy(&new_before_palette, new_object + 0x1c, sizeof(new_before_palette));
                new_prefetch(new_image);
                std::uint32_t new_after_prefetch = 0;
                std::memcpy(&new_after_prefetch, new_object + 0x18, sizeof(new_after_prefetch));
                if (new_after_prefetch != new_before_pixels) return 185;
                if (!!new_before_pixels != !!before_pixels || !!new_before_palette != !!before_palette) return 43;
                if (new_object[0x24] != image_object[0x24] || new_object[0x34] != image_object[0x34]) return 40;
                if (new_object[0x37] != image_object[0x37]) {
                    std::fprintf(stderr, "Loaded texture blend: resource=%s flags=%08x original=%u replacement=%u.\n",
                        name, flags, image_object[0x37], new_object[0x37]);
                    return 190;
                }
                LockedTexture new_lock = {}, nested_lock = {};
                if (!replacement_lock(new_image, &new_lock, 1) || !replacement_lock(new_image, &nested_lock, 1)) return 41;
                if (new_lock.pixels != nested_lock.pixels || new_lock.palette != nested_lock.palette) {
                    std::fprintf(stderr, "Nested lock changed the returned pointers for %s flags=%u.\n", name, flags);
                    return 41;
                }
                if (new_lock.width != loaded.width || new_lock.height != loaded.height || new_lock.pitch != loaded.pitch) {
                    std::fprintf(stderr, "%s flags=%u: dimensions/pitch differ: original=%ux%u/%d replacement=%ux%u/%d.\n",
                        name, flags, loaded.width, loaded.height, loaded.pitch,
                        new_lock.width, new_lock.height, new_lock.pitch);
                    return 41;
                }
                const std::size_t row_bytes = static_cast<std::size_t>(loaded.width) * image_object[0x24];
                if (row_bytes > static_cast<std::size_t>(loaded.pitch)) {
                    std::fprintf(stderr, "%s flags=%u: pixel width exceeds pitch.\n", name, flags);
                    return 41;
                }
                for (std::uint32_t y = 0; y < loaded.height; ++y) {
                    const auto* old_row = loaded.pixels + y * loaded.pitch;
                    const auto* new_row = new_lock.pixels + y * new_lock.pitch;
                    if (std::memcmp(new_row, old_row, row_bytes) != 0) {
                        for (std::size_t x = 0; x < row_bytes; ++x) {
                            if (new_row[x] != old_row[x]) {
                                std::fprintf(stderr, "%s flags=%u: first pixel mismatch at row %u byte %zu: original=%02x replacement=%02x.\n",
                                    name, flags, y, x, old_row[x], new_row[x]);
                                break;
                            }
                        }
                        return 41;
                    }
                }
                if ((loaded.palette == nullptr) != (new_lock.palette == nullptr)) {
                    std::fprintf(stderr, "%s flags=%u: palette presence differs.\n", name, flags);
                    return 41;
                }
                if (loaded.palette && std::memcmp(new_lock.palette, loaded.palette, 1024) != 0) {
                    for (std::size_t offset = 0; offset < 1024; ++offset) {
                        if (new_lock.palette[offset] != loaded.palette[offset]) {
                            std::fprintf(stderr, "%s flags=%u: first palette mismatch at byte %zu: original=%02x replacement=%02x.\n",
                                name, flags, offset, loaded.palette[offset], new_lock.palette[offset]);
                            break;
                        }
                    }
                    return 41;
                }
                replacement_unlock(new_image);
                replacement_unlock(new_image);
                replacement_free(new_image);
            }
            if (flags == 3 && !loaded.palette) {
                std::uint32_t first_pixel = 0;
                std::memcpy(&first_pixel, loaded.pixels, sizeof(first_pixel));
                set_key(first_pixel);
                replacement_key(first_pixel);
            }
            unlock(image);
            free_image(image);
        }
    }
    const auto make_font = resolve<void* (__cdecl*)(const void*, void*, int, int, int, int)>(library, "sgl_MemoryFont_CreateProportional");
    const auto free_font = resolve<void (__cdecl*)(void*)>(library, "sgl_FreeFont");
    const auto new_make_font = resolve<void* (__cdecl*)(const void*, void*, int, int, int, int)>(replacement, "sgl_MemoryFont_CreateProportional");
    const auto new_free_font = resolve<void (__cdecl*)(void*)>(replacement, "sgl_FreeFont");
    const auto create_font_fixture = resolve<void* (__cdecl*)(std::uint16_t, std::uint16_t, std::uint32_t)>(library, "sgl_CreateMemoryTexture");
    const auto new_create_font_fixture = resolve<void* (__cdecl*)(std::uint16_t, std::uint16_t, std::uint32_t)>(replacement, "sgl_CreateMemoryTexture");
    const auto old_draw_text = resolve<void (__cdecl*)(int, int, const unsigned char*)>(library, "sgl_DrawText");
    const auto new_draw_text = resolve<void (__cdecl*)(int, int, const unsigned char*)>(replacement, "sgl_DrawText");
    const auto old_draw_text_rect = resolve<void (__cdecl*)(int, int, const std::int32_t*, const unsigned char*)>(library, "sgl_DrawText_Rect");
    const auto new_draw_text_rect = resolve<void (__cdecl*)(int, int, const std::int32_t*, const unsigned char*)>(replacement, "sgl_DrawText_Rect");
    const auto old_text_mode = resolve<void (__cdecl*)(int)>(library, "sgl_SetTextMode");
    const auto new_text_mode = resolve<void (__cdecl*)(int)>(replacement, "sgl_SetTextMode");
    const auto new_source_font_test = resolve<void* (__cdecl*)(void*)>(replacement, "sgl_SetRenderSource");
    const auto new_target_font_test = resolve<void* (__cdecl*)(void*)>(replacement, "sgl_SetRenderDest");
    const auto new_constant_font_test = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(replacement, "sgl_SetConstantColor");
    void* font_image = load_image("fonts/tiepolo_black16.aim", 3);
    void* new_font_image = replacement_load("fonts/tiepolo_black16.aim", 3);
    if (!make_font || !free_font || !new_make_font || !new_free_font || !font_image || !new_font_image || !create_font_fixture ||
        !new_create_font_fixture || !old_draw_text || !new_draw_text || !old_draw_text_rect || !new_draw_text_rect ||
        !old_text_mode || !new_text_mode ||
        !new_source_font_test || !new_target_font_test || !new_constant_font_test) return 133;
    void* real_font = make_font(" ABCD", font_image, 15, 16, 0, 4);
    if (!real_font) return 134;
    const auto* real_font_data = static_cast<const std::uint8_t*>(real_font);
    std::uint32_t font_count = 0;
    const std::uint8_t* font_widths = nullptr;
    std::memcpy(&font_count, real_font_data + 0x10, sizeof(font_count));
    std::memcpy(&font_widths, real_font_data + 0x14, sizeof(font_widths));
    if (font_count != 5 || !font_widths || real_font_data[0x20 + 'A'] != 1 || real_font_data[0x20 + 'B'] != 2) return 135;
    std::printf("Original font: glyphs=%u A=%u/%u B=%u/%u space=%u/%u\n", font_count,
        real_font_data[0x20 + 'A'], font_widths[real_font_data[0x20 + 'A']],
        real_font_data[0x20 + 'B'], font_widths[real_font_data[0x20 + 'B']],
        real_font_data[0x20 + ' '], font_widths[real_font_data[0x20 + ' ']]);
    std::uint32_t font_params[4]{};
    std::memcpy(&font_params[0], real_font_data + 4, 12);
    std::memcpy(&font_params[3], real_font_data + 0x120, 4);
    std::printf("Font metadata %u %u %u %u, widths:", font_params[0], font_params[1], font_params[2], font_params[3]);
    for (std::uint32_t i = 0; i < font_count; ++i) std::printf(" %u", font_widths[i]);
    std::puts("");
    LockedTexture font_pixels{}, copy_pixels{};
    void* font_copy = create_font_fixture(1770, 16, 0);
    if (!font_copy || !lock(font_image, &font_pixels, 1) || !lock(font_copy, &copy_pixels, 2)) return 146;
    for (std::uint32_t y = 0; y < font_pixels.height; ++y)
        std::memcpy(copy_pixels.pixels + y * copy_pixels.pitch, font_pixels.pixels + y * font_pixels.pitch, font_pixels.width * 4);
    unlock(font_copy); unlock(font_image);
    void* copied_font = make_font(" ABCD", font_copy, 15, 16, 0, 4);
    if (!copied_font) return 147;
    const std::uint8_t* copied_widths = nullptr;
    std::memcpy(&copied_widths, static_cast<const std::uint8_t*>(copied_font) + 0x14, sizeof(copied_widths));
    std::printf("Copied font widths:");
    for (std::uint32_t i = 0; i < font_count; ++i) std::printf(" %u", copied_widths[i]);
    std::puts("");
    for (const auto& spacing : std::array<std::array<int, 2>, 3>{{{0, 4}, {2, 7}, {5, 1}}}) {
        void* old_check = make_font(" ABCD", font_image, 15, 16, spacing[0], spacing[1]);
        void* new_check = new_make_font(" ABCD", new_font_image, 15, 16, spacing[0], spacing[1]);
        if (!old_check || !new_check) return 148;
        const auto* old_data = static_cast<const std::uint8_t*>(old_check);
        const auto* new_data = static_cast<const std::uint8_t*>(new_check);
        const std::uint8_t *old_widths = nullptr, *new_widths = nullptr;
        std::memcpy(&old_widths, old_data + 0x14, sizeof(old_widths));
        std::memcpy(&new_widths, new_data + 0x14, sizeof(new_widths));
        if (std::memcmp(old_data + 0x20, new_data + 0x20, 256) != 0 ||
            !old_widths || !new_widths || std::memcmp(old_widths, new_widths, font_count) != 0) return 149;
        old_font(old_check); new_font(new_check);
        for (const auto* text : texts) if (old_strlen(text) != new_strlen(text)) return 150;
        if (spacing[0] == 0) {
            void* old_canvas = create_font_fixture(80, 32, 0);
            void* new_canvas = new_create_font_fixture(80, 32, 0);
            if (!old_canvas || !new_canvas) return 155;
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x6e93c) = 0xffffffff;
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x8d080) = 0xffffffff;
            new_constant_font_test(0xffffffff);
            void* old_text_region = old_create_region();
            void* new_text_region = new_create_region();
            if (!old_text_region || !new_text_region) return 161;
            void* previous_old_region = old_set_region(old_text_region);
            void* previous_new_region = new_set_region(new_text_region);
            const RECT first{0, 0, 38, 20}, second{25, 4, 72, 25}, cut{12, 7, 18, 13};
            old_add(&first); new_add(&first); old_add(&second); new_add(&second); old_sub(&cut); new_sub(&cut);
            for (int clip_kind : {0, 1, 2, 3}) for (int mode : {1, 2, 3}) for (bool use_rect : {false, true}) {
                LockedTexture old_pixels{}, new_pixels{};
                if (!lock(old_canvas, &old_pixels, 2) || !replacement_lock(new_canvas, &new_pixels, 2)) return 156;
                std::memset(old_pixels.pixels, 0x5a, old_pixels.pitch * old_pixels.height);
                std::memset(new_pixels.pixels, 0x5a, new_pixels.pitch * new_pixels.height);
                unlock(old_canvas); replacement_unlock(new_canvas);
                set_source(old_canvas); new_source_font_test(new_canvas);
                set_target(old_canvas); new_target_font_test(new_canvas);
                clip_mode(clip_kind); replacement_clip_mode(clip_kind);
                clip_rect(4, 3, 72, 27); replacement_clip_rect(4, 3, 72, 27);
                old_text_mode(mode); new_text_mode(mode);
                const auto* sample = reinterpret_cast<const unsigned char*>("ABCD ABC");
                const int text_x = mode == 1 ? 2 : 30, text_y = mode == 1 ? 1 : 5;
                if (use_rect) {
                    const std::array<std::int32_t, 4> text_rect{20, 4, 55, 20};
                    old_draw_text_rect(text_x, text_y, text_rect.data(), sample);
                    new_draw_text_rect(text_x, text_y, text_rect.data(), sample);
                } else {
                    old_draw_text(text_x, text_y, sample); new_draw_text(text_x, text_y, sample);
                }
                if (set_source(nullptr) != old_canvas || new_source_font_test(nullptr) != new_canvas) return 157;
                get_clip_rect(old_clip.data()); replacement_get_clip_rect(new_clip.data());
                if (old_clip != std::array<std::int32_t, 4>{4, 3, 72, 27} || old_clip != new_clip ||
                    get_clip_mode() != clip_kind || replacement_get_clip_mode() != clip_kind) return 158;
                set_target(nullptr); new_target_font_test(nullptr);
                if (!lock(old_canvas, &old_pixels, 1) || !replacement_lock(new_canvas, &new_pixels, 1) ||
                    std::memcmp(old_pixels.pixels, new_pixels.pixels, old_pixels.pitch * old_pixels.height)) return 159;
                bool changed = false;
                for (std::uint32_t i = 0; i < old_pixels.pitch * old_pixels.height; ++i) changed |= old_pixels.pixels[i] != 0x5a;
                if (!changed) return 160;
                unlock(old_canvas); replacement_unlock(new_canvas);
            }
            old_set_region(previous_old_region); new_set_region(previous_new_region);
            old_free_region(old_text_region); new_free_region(new_text_region);
            free_image(old_canvas); replacement_free(new_canvas);
        }
        old_font(nullptr); new_font(nullptr);
        free_font(old_check); new_free_font(new_check);
    }
    replacement_free(new_font_image);
    free_font(copied_font); free_image(font_copy);
    free_font(real_font);
    free_image(font_image);
    struct FontFixture { const char* name; int width; int height; };
    const std::array<FontFixture, 5> real_fonts{{
        {"fonts/tiepolo_black16.aim", 15, 16}, {"fonts/tiepolo_bold16.aim", 15, 16},
        {"fonts/tiepolo_black20.aim", 19, 20}, {"fonts/tiepolo_bold24.aim", 25, 24},
        {"fonts/elgreco72.aim", 80, 75}}};
    const char characters[] = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
        "\xb0\xe4\xf6\xfc\xc4\xd6\xdc\xdf\xe1\xe9\xed\xf3\xfa\xe0\xe8\xec\xf2\xf9\xe2\xea\xee\xf4\xfb";
    for (const auto& fixture : real_fonts) {
        void* old_image = load_image(fixture.name, 3);
        void* new_image = replacement_load(fixture.name, 3);
        if (!old_image || !new_image) return 151;
        void* old_full = make_font(characters, old_image, fixture.width, fixture.height, 0, 4);
        void* new_full = new_make_font(characters, new_image, fixture.width, fixture.height, 0, 4);
        if (!old_full || !new_full) return 152;
        const auto* old_data = static_cast<const std::uint8_t*>(old_full);
        const auto* new_data = static_cast<const std::uint8_t*>(new_full);
        std::uint32_t old_count = 0, new_count = 0;
        const std::uint8_t *old_widths = nullptr, *new_widths = nullptr;
        std::memcpy(&old_count, old_data + 0x10, sizeof(old_count));
        std::memcpy(&new_count, new_data + 0x10, sizeof(new_count));
        std::memcpy(&old_widths, old_data + 0x14, sizeof(old_widths));
        std::memcpy(&new_widths, new_data + 0x14, sizeof(new_widths));
        if (old_count != new_count || !old_widths || !new_widths ||
            std::memcmp(old_data + 0x20, new_data + 0x20, 256) || std::memcmp(old_widths, new_widths, old_count)) return 153;
        old_font(old_full); new_font(new_full);
        if (old_strlen(reinterpret_cast<const unsigned char*>(characters)) != new_strlen(reinterpret_cast<const unsigned char*>(characters))) return 154;
        old_font(nullptr); new_font(nullptr);
        free_font(old_full); new_free_font(new_full); free_image(old_image); replacement_free(new_image);
        std::printf("Font %s: %u glyphs match.\n", fixture.name, old_count);
    }
    const auto old_kerning_font = resolve<void* (__cdecl*)(const void*, void*, int, int, int, int)>(library, "sgl_MemoryFont_CreateKerning");
    const auto new_kerning_font = resolve<void* (__cdecl*)(const void*, void*, int, int, int, int)>(replacement, "sgl_MemoryFont_CreateKerning");
    void* old_kimage = load_image("fonts/elgreco24.aim", 3);
    void* new_kimage = replacement_load("fonts/elgreco24.aim", 3);
    if (!old_kerning_font || !new_kerning_font || !old_kimage || !new_kimage) return 162;
    void* old_kfont = old_kerning_font(characters, old_kimage, 26, 24, 0, 4);
    void* new_kfont = new_kerning_font(characters, new_kimage, 26, 24, 0, 4);
    if (!old_kfont || !new_kfont) return 163;
    const auto* old_kdata = static_cast<const std::uint8_t*>(old_kfont);
    const auto* new_kdata = static_cast<const std::uint8_t*>(new_kfont);
    std::uint32_t kerning_count = 0;
    const std::uint8_t *old_pairs = nullptr, *new_pairs = nullptr;
    std::memcpy(&kerning_count, old_kdata + 0x10, sizeof(kerning_count));
    std::memcpy(&old_pairs, old_kdata + 0x18, sizeof(old_pairs));
    std::memcpy(&new_pairs, new_kdata + 0x18, sizeof(new_pairs));
    if (!old_pairs || !new_pairs || kerning_count != 118) return 164;
    unsigned pair_differences = 0;
    for (std::uint32_t i = 0; i < kerning_count * kerning_count; ++i)
        if (old_pairs[i] != new_pairs[i]) ++pair_differences;
    std::printf("Kerning table: %u differences across %u pairs.\n", pair_differences, kerning_count * kerning_count);
    if (pair_differences) return 165;
    old_font(old_kfont); new_font(new_kfont);
    for (const auto* text : texts) if (old_strlen(text) != new_strlen(text)) return 166;
    if (old_strlen(reinterpret_cast<const unsigned char*>(characters)) !=
        new_strlen(reinterpret_cast<const unsigned char*>(characters))) return 167;
    const std::uint8_t* old_kwidths = nullptr;
    std::memcpy(&old_kwidths, old_kdata + 0x14, sizeof(old_kwidths));
    const auto first_glyph = old_kdata[0x20 + 'W'], second_glyph = old_kdata[0x20 + 'A'];
    if (!old_kwidths || old_pairs[first_glyph * kerning_count + second_glyph] == old_kwidths[first_glyph]) return 168;
    void* old_kcanvas = create_font_fixture(160, 48, 0);
    void* new_kcanvas = new_create_font_fixture(160, 48, 0);
    void* old_kregion = old_create_region();
    void* new_kregion = new_create_region();
    if (!old_kcanvas || !new_kcanvas || !old_kregion || !new_kregion) return 169;
    void* old_kprior = old_set_region(old_kregion);
    void* new_kprior = new_set_region(new_kregion);
    const RECT kfirst{0, 0, 85, 35}, ksecond{70, 10, 150, 42}, kcut{52, 8, 61, 20};
    old_add(&kfirst); new_add(&kfirst); old_add(&ksecond); new_add(&ksecond); old_sub(&kcut); new_sub(&kcut);
    for (int clip_kind : {1, 2, 3}) for (int mode : {1, 2, 3}) {
        LockedTexture old_pixels{}, new_pixels{};
        if (!lock(old_kcanvas, &old_pixels, 2) || !replacement_lock(new_kcanvas, &new_pixels, 2)) return 170;
        std::memset(old_pixels.pixels, 0x5a, old_pixels.pitch * old_pixels.height);
        std::memset(new_pixels.pixels, 0x5a, new_pixels.pitch * new_pixels.height);
        unlock(old_kcanvas); replacement_unlock(new_kcanvas);
        set_source(old_kcanvas); new_source_font_test(new_kcanvas);
        set_target(old_kcanvas); new_target_font_test(new_kcanvas);
        clip_mode(clip_kind); replacement_clip_mode(clip_kind);
        clip_rect(4, 3, 150, 42); replacement_clip_rect(4, 3, 150, 42);
        old_text_mode(mode); new_text_mode(mode);
        const auto* sample = reinterpret_cast<const unsigned char*>("WAVE AVATAR");
        old_draw_text(70, 5, sample); new_draw_text(70, 5, sample);
        if (set_source(nullptr) != old_kcanvas || new_source_font_test(nullptr) != new_kcanvas) return 171;
        get_clip_rect(old_clip.data()); replacement_get_clip_rect(new_clip.data());
        if (old_clip != std::array<std::int32_t, 4>{4, 3, 150, 42} || old_clip != new_clip ||
            get_clip_mode() != clip_kind || replacement_get_clip_mode() != clip_kind) return 172;
        set_target(nullptr); new_target_font_test(nullptr);
        if (!lock(old_kcanvas, &old_pixels, 1) || !replacement_lock(new_kcanvas, &new_pixels, 1) ||
            std::memcmp(old_pixels.pixels, new_pixels.pixels, old_pixels.pitch * old_pixels.height)) return 173;
        bool changed = false;
        for (std::uint32_t i = 0; i < old_pixels.pitch * old_pixels.height; ++i) changed |= old_pixels.pixels[i] != 0x5a;
        if (!changed) return 174;
        unlock(old_kcanvas); replacement_unlock(new_kcanvas);
    }
    old_set_region(old_kprior); new_set_region(new_kprior);
    old_free_region(old_kregion); new_free_region(new_kregion);
    free_image(old_kcanvas); replacement_free(new_kcanvas);
    old_font(nullptr); new_font(nullptr);
    const auto old_fixed_font = resolve<void* (__cdecl*)(const void*, void*, int, int, int)>(library, "sgl_MemoryFont_CreateFixed");
    const auto new_fixed_font = resolve<void* (__cdecl*)(const void*, void*, int, int, int)>(replacement, "sgl_MemoryFont_CreateFixed");
    if (!old_fixed_font || !new_fixed_font) return 175;
    for (int advance : {0, 4, 15, 255}) {
        void* old_fixed = old_fixed_font(characters, old_kimage, 26, 24, advance);
        void* new_fixed = new_fixed_font(characters, new_kimage, 26, 24, advance);
        if (!old_fixed || !new_fixed) return 176;
        const auto* old_data = static_cast<const std::uint8_t*>(old_fixed);
        const auto* new_data = static_cast<const std::uint8_t*>(new_fixed);
        const std::uint8_t *old_widths = nullptr, *new_widths = nullptr;
        std::memcpy(&old_widths, old_data + 0x14, sizeof(old_widths));
        std::memcpy(&new_widths, new_data + 0x14, sizeof(new_widths));
        if (!old_widths || !new_widths || std::memcmp(old_data + 0x20, new_data + 0x20, 256) ||
            std::memcmp(old_widths, new_widths, kerning_count)) return 177;
        old_font(old_fixed); new_font(new_fixed);
        if (old_strlen(reinterpret_cast<const unsigned char*>(characters)) !=
            new_strlen(reinterpret_cast<const unsigned char*>(characters))) return 178;
        if (advance == 15) {
            void* old_canvas = create_font_fixture(160, 48, 0);
            void* new_canvas = new_create_font_fixture(160, 48, 0);
            if (!old_canvas || !new_canvas) return 179;
            LockedTexture old_pixels{}, new_pixels{};
            if (!lock(old_canvas, &old_pixels, 2) || !replacement_lock(new_canvas, &new_pixels, 2)) return 180;
            std::memset(old_pixels.pixels, 0x5a, old_pixels.pitch * old_pixels.height);
            std::memset(new_pixels.pixels, 0x5a, new_pixels.pitch * new_pixels.height);
            unlock(old_canvas); replacement_unlock(new_canvas);
            set_target(old_canvas); new_target_font_test(new_canvas);
            clip_mode(1); replacement_clip_mode(1);
            clip_rect(4, 3, 150, 42); replacement_clip_rect(4, 3, 150, 42);
            old_text_mode(1); new_text_mode(1);
            const auto* sample = reinterpret_cast<const unsigned char*>("FIXED FONT");
            old_draw_text(6, 5, sample); new_draw_text(6, 5, sample);
            set_target(nullptr); new_target_font_test(nullptr);
            if (!lock(old_canvas, &old_pixels, 1) || !replacement_lock(new_canvas, &new_pixels, 1) ||
                std::memcmp(old_pixels.pixels, new_pixels.pixels, old_pixels.pitch * old_pixels.height)) return 181;
            bool changed = false;
            for (std::uint32_t i = 0; i < old_pixels.pitch * old_pixels.height; ++i) changed |= old_pixels.pixels[i] != 0x5a;
            if (!changed) return 182;
            unlock(old_canvas); replacement_unlock(new_canvas);
            free_image(old_canvas); replacement_free(new_canvas);
        }
        old_font(nullptr); new_font(nullptr);
        free_font(old_fixed); new_free_font(new_fixed);
    }
    free_font(old_kfont); new_free_font(new_kfont); free_image(old_kimage); replacement_free(new_kimage);
    if (!create_font_fixture) return 141;
    void* fixture = create_font_fixture(12, 16, 0);
    LockedTexture fixture_pixels{};
    if (!fixture || !lock(fixture, &fixture_pixels, 2)) return 142;
    std::memset(fixture_pixels.pixels, 0, fixture_pixels.pitch * fixture_pixels.height);
    for (unsigned i = 0; i < 3; ++i) fixture_pixels.pixels[(i * 7) * fixture_pixels.pitch + ((i * 4) + i + 1) * 4 + 3] = 1;
    unlock(fixture);
    void* fixture_font = make_font("ABC", fixture, 4, 16, 0, 4);
    if (!fixture_font) return 143;
    const auto* fixture_data = static_cast<const std::uint8_t*>(fixture_font);
    const std::uint8_t* fixture_widths = nullptr;
    std::memcpy(&fixture_widths, fixture_data + 0x14, sizeof(fixture_widths));
    if (!fixture_widths) return 144;
    std::printf("Fixture font widths: %u %u %u\n", fixture_widths[0], fixture_widths[1], fixture_widths[2]);
    if (fixture_widths[0] != 2 || fixture_widths[1] != 3 || fixture_widths[2] != 4) return 145;
    free_font(fixture_font);
    free_image(fixture);
    void* written = load_image("images/options/hmenu_colormap.bmp", 3);
    if (!written) return 44;
    LockedTexture write_lock = {}, nested_write = {};
    if (!lock(written, &write_lock, 2) || !write_lock.pixels || !write_lock.palette) return 45;
    write_lock.pixels[0] ^= 0x5a;
    write_lock.palette[0] ^= 0x00123456;
    const auto pixel_after_write = write_lock.pixels[0];
    const auto palette_after_write = write_lock.palette[0];
    if (!lock(written, &nested_write, 1) || nested_write.pixels[0] != pixel_after_write || nested_write.palette[0] != palette_after_write) return 46;
    unlock(written);
    unlock(written);
    LockedTexture after_unlock = {};
    if (!lock(written, &after_unlock, 1) || !after_unlock.palette) return 47;
    std::printf("Write lock: pixel persists=%s palette persists=%s\n",
        after_unlock.pixels[0] == pixel_after_write ? "yes" : "no",
        after_unlock.palette[0] == palette_after_write ? "yes" : "no");
    unlock(written);
    free_image(written);
    for (const char* name : {"images/options/Hauptmenu.jpg", "images/options/hmenu_colormap.bmp", "images/feder0003.tga"}) {
        for (std::uint32_t flags : {3u, 7u}) {
            set_key(0x00123456);
            replacement_key(0x00123456);
            void* old_image = load_image(name, flags);
            void* new_image = replacement_load(name, flags);
            if (!old_image || !new_image) return 49;
            if (set_key(0x00abcdef) != replacement_key(0x00abcdef)) return 53;
            LockedTexture old_write = {}, new_write = {}, old_nested = {}, new_nested = {};
            if (!lock(old_image, &old_write, 2) || !replacement_lock(new_image, &new_write, 2)) return 50;
            if (old_write.palette) {
                old_write.pixels[0] ^= 0x5a;
                new_write.pixels[0] ^= 0x5a;
                old_write.palette[0] ^= 0x00123456;
                new_write.palette[0] ^= 0x00123456;
            } else {
                const std::uint32_t values[] = {0x00123456, 0x00010203};
                std::memcpy(old_write.pixels, values, sizeof(values));
                std::memcpy(new_write.pixels, values, sizeof(values));
            }
            if (!lock(old_image, &old_nested, 3) || !replacement_lock(new_image, &new_nested, 3)) return 51;
            unlock(old_image);
            replacement_unlock(new_image);
            unlock(old_image);
            replacement_unlock(new_image);
            LockedTexture old_after = {}, new_after = {};
            if (!lock(old_image, &old_after, 1) || !replacement_lock(new_image, &new_after, 1) ||
                old_after.pitch != new_after.pitch || old_after.height != new_after.height ||
                std::memcmp(old_after.pixels, new_after.pixels, old_after.pitch * old_after.height) != 0 ||
                (old_after.palette == nullptr) != (new_after.palette == nullptr) ||
                (old_after.palette && std::memcmp(old_after.palette, new_after.palette, 1024) != 0)) return 52;
            unlock(old_image);
            replacement_unlock(new_image);
            free_image(old_image);
            replacement_free(new_image);
            std::printf("Write-lock comparison: %s flags=%u matched.\n", name, flags);
        }
    }
    reference_archive(nullptr);
    replacement_archive(nullptr);
    release_container(container);

    const auto create_memory = resolve<void* (__cdecl*)(std::uint16_t, std::uint16_t, std::uint32_t)>(library, "sgl_CreateMemoryTexture");
    const auto replacement_create_memory = resolve<void* (__cdecl*)(std::uint16_t, std::uint16_t, std::uint32_t)>(replacement, "sgl_CreateMemoryTexture");
    const auto replacement_source = resolve<void* (__cdecl*)(void*)>(replacement, "sgl_SetRenderSource");
    const auto replacement_target = resolve<void* (__cdecl*)(void*)>(replacement, "sgl_SetRenderDest");
    if (!create_memory || !replacement_create_memory || !replacement_source || !replacement_target) return 54;
    void* old_surface = create_memory(8, 8, 3);
    void* new_surface = replacement_create_memory(8, 8, 3);
    void* old_indexed = create_memory(8, 8, 0x23);
    void* new_indexed = replacement_create_memory(8, 8, 0x23);
    if (!old_surface || !new_surface || !old_indexed || !new_indexed) return 61;
    if (set_source(old_surface) != nullptr || replacement_source(new_surface) != nullptr ||
        set_source(old_surface) != old_surface || replacement_source(new_surface) != new_surface ||
        set_source(nullptr) != old_surface || replacement_source(nullptr) != new_surface ||
        set_source(nullptr) != nullptr || replacement_source(nullptr) != nullptr) return 62;
    if (set_target(old_surface) != nullptr || replacement_target(new_surface) != nullptr ||
        set_target(old_surface) != old_surface || replacement_target(new_surface) != new_surface ||
        set_target(old_indexed) != old_surface || replacement_target(new_indexed) != new_surface ||
        set_target(old_surface) != nullptr || replacement_target(new_surface) != nullptr ||
        set_target(nullptr) != old_surface || replacement_target(nullptr) != new_surface ||
        set_target(nullptr) != nullptr || replacement_target(nullptr) != nullptr) return 63;
    free_image(old_surface);
    free_image(old_indexed);
    replacement_free(new_surface);
    replacement_free(new_indexed);
    std::puts("Source and destination selection: original and replacement agree.");

    void* old_selected_source = create_memory(8, 8, 0);
    void* new_selected_source = replacement_create_memory(8, 8, 0);
    if (!old_selected_source || !new_selected_source) return 64;
    set_source(old_selected_source);
    replacement_source(new_selected_source);
    free_image(old_selected_source);
    replacement_free(new_selected_source);
    set_source(nullptr);
    if (replacement_source(nullptr) != nullptr) return 65;
    std::puts("Freeing the selected source: original call completes and replacement clears stale state.");

    set_key(0x00121212);
    replacement_key(0x00121212);
    for (std::uint32_t palette_flags : {0x23u, 0x27u}) {
        void* palette_image = create_memory(8, 8, palette_flags);
        void* new_palette_image = replacement_create_memory(8, 8, palette_flags);
        if (!palette_image || !new_palette_image) return 68;
        std::array<std::uint32_t, 256> palette_a = {};
        palette_a[0] = 0xff121212;
        palette_a[1] = 0xffaabbcc;
        auto new_palette_a = palette_a;
        if (set_palette(nullptr, nullptr) != replacement_palette(nullptr, nullptr) ||
            set_palette(palette_image, palette_a.data()) != replacement_palette(new_palette_image, new_palette_a.data()) ||
            palette_a != new_palette_a) return 72;
        LockedTexture palette_lock = {}, new_palette_lock = {};
        if (!lock(palette_image, &palette_lock, 1) || !replacement_lock(new_palette_image, &new_palette_lock, 1) ||
            !palette_lock.palette || !new_palette_lock.palette || std::memcmp(palette_lock.palette, new_palette_lock.palette, 1024)) return 69;
        std::array<std::uint32_t, 256> palette_b = {};
        palette_b[0] = 0xff112233;
        palette_b[1] = 0xff121212;
        auto new_palette_b = palette_b;
        if (set_palette(palette_image, palette_b.data()) != replacement_palette(new_palette_image, new_palette_b.data()) ||
            palette_b != new_palette_b) return 73;
        LockedTexture palette_nested = {}, new_palette_nested = {};
        if (!lock(palette_image, &palette_nested, 1) || !replacement_lock(new_palette_image, &new_palette_nested, 1) ||
            !palette_nested.palette || !new_palette_nested.palette || std::memcmp(palette_nested.palette, new_palette_nested.palette, 1024)) return 70;
        const auto palette_header = static_cast<const std::uint8_t*>(palette_image);
        const std::uint32_t* internal_palette = nullptr;
        std::memcpy(&internal_palette, palette_header + 0x1c, sizeof(internal_palette));
        const std::uint32_t* new_internal_palette = nullptr;
        std::memcpy(&new_internal_palette, static_cast<const std::uint8_t*>(new_palette_image) + 0x1c, sizeof(new_internal_palette));
        if (!internal_palette || !new_internal_palette || std::memcmp(internal_palette, new_internal_palette, 1024)) return 71;
        std::printf("Palette: inputA=%08x inputB=%08x locked=%08x internal=%08x\n",
            palette_a[0], palette_b[1], palette_nested.palette[0], internal_palette[0]);
        unlock(palette_image);
        unlock(palette_image);
        replacement_unlock(new_palette_image);
        replacement_unlock(new_palette_image);
        free_image(palette_image);
        replacement_free(new_palette_image);
    }
    set_key(0x00121212);
    replacement_key(0x00121212);
    // Includes the exact width used by the game's VTO video texture.
    const std::array<std::uint16_t, 5> widths = {1, 7, 8, 13, 400};
    const std::array<std::uint32_t, 10> flag_values = {
        0u, 1u, 3u, 7u, 0x20u, 0x21u, 0x23u, 0x27u, 0x01000050u, 0x05000010u
    };
    for (std::size_t i = 0; i < widths.size() * flag_values.size(); ++i) {
        const auto width = widths[i / flag_values.size()];
        const auto flags = flag_values[i % flag_values.size()];
        void* image = create_memory(width, 3, flags);
        void* new_image = replacement_create_memory(width, 3, flags);
        if (!image || !new_image) return 55;
        const auto expected_key = (flags & 7) ? 0x00121212u : 0u;
        if (old_texture_key(image) != expected_key || new_texture_key(new_image) != expected_key) return 128;
        set_key(0x00abcdef);
        replacement_key(0x00abcdef);
        if (old_texture_key(image) != expected_key || new_texture_key(new_image) != expected_key) return 129;
        set_key(0x00121212);
        replacement_key(0x00121212);
        const auto header = static_cast<const std::uint8_t*>(image);
        const auto new_header = static_cast<const std::uint8_t*>(new_image);
        std::uint32_t raw_key = 0, before_pixels = 0, before_palette = 0, initial_pitch = 0;
        std::memcpy(&raw_key, header + 8, sizeof(raw_key));
        std::memcpy(&before_pixels, header + 0x18, sizeof(before_pixels));
        std::memcpy(&before_palette, header + 0x1c, sizeof(before_palette));
        std::memcpy(&initial_pitch, header + 0x28, sizeof(initial_pitch));
        std::uint32_t new_before_pixels = 0, new_before_palette = 0, new_key = 0, new_pitch = 0;
        std::memcpy(&new_before_pixels, new_header + 0x18, sizeof(new_before_pixels));
        std::memcpy(&new_before_palette, new_header + 0x1c, sizeof(new_before_palette));
        std::memcpy(&new_key, new_header + 8, sizeof(new_key));
        std::memcpy(&new_pitch, new_header + 0x28, sizeof(new_pitch));
        if (!before_pixels || !new_before_pixels || raw_key != new_key || initial_pitch != new_pitch ||
            (before_palette == 0) != (new_before_palette == 0) ||
            std::memcmp(reinterpret_cast<void*>(before_pixels), reinterpret_cast<void*>(new_before_pixels), initial_pitch * 3) != 0 ||
            (before_palette && std::memcmp(reinterpret_cast<void*>(before_palette), reinterpret_cast<void*>(new_before_palette), 1024) != 0)) return 60;
        LockedTexture output = {}, new_output = {};
        if (!lock(image, &output, 1) || !replacement_lock(new_image, &new_output, 1) || !output.pixels || !new_output.pixels) return 56;
        if (!before_pixels || !new_before_pixels || header[0x24] != new_header[0x24] || header[0x34] != new_header[0x34] ||
            header[0x37] != new_header[0x37] || output.width != new_output.width || output.height != new_output.height || output.pitch != new_output.pitch ||
            std::memcmp(output.pixels, new_output.pixels, output.pitch * output.height) != 0 ||
            (output.palette == nullptr) != (new_output.palette == nullptr) ||
            (output.palette && std::memcmp(output.palette, new_output.palette, 1024) != 0)) return 57;
        std::printf("Created flags=%02x: %ux%u pitch=%u bpp=%u state=%02x key=%08x pre_pixels=%s palette=%s",
            flags, output.width, output.height, output.pitch, header[0x24], header[0x34], raw_key,
            before_pixels ? "yes" : "no", output.palette ? "yes" : "no");
        if (output.palette) std::printf(" palette[0]=%08x palette[18]=%08x", output.palette[0], output.palette[18]);
        std::printf(" first=%02x\n", output.pixels[0]);
        unlock(image);
        replacement_unlock(new_image);
        if (!lock(image, &output, 2) || !replacement_lock(new_image, &new_output, 2)) return 58;
        if (output.palette) {
            output.pixels[0] = new_output.pixels[0] = 18;
            output.palette[0] = new_output.palette[0] = 0xabcdef01;
        } else {
            const std::uint32_t colors[] = {0x00121212, 0x00010203};
            std::memcpy(output.pixels, colors, sizeof(colors));
            std::memcpy(new_output.pixels, colors, sizeof(colors));
        }
        unlock(image);
        replacement_unlock(new_image);
        if (!lock(image, &output, 1) || !replacement_lock(new_image, &new_output, 1) ||
            std::memcmp(output.pixels, new_output.pixels, output.pitch * output.height) != 0 ||
            (output.palette && std::memcmp(output.palette, new_output.palette, 1024) != 0)) return 59;
        unlock(image);
        replacement_unlock(new_image);
        free_image(image);
        replacement_free(new_image);
    }

    constexpr std::array<std::uint32_t, 4> flag_initializers = {0u, 1u, 3u, 0x05000010u};
    constexpr std::array<std::uint32_t, 9> flag_changes = {
        0u, 1u, 0x10u, 0x01000000u, 0x05000010u, 0x07018197u, 0x08000000u, 1u, 1u
    };
    for (const auto initial_flags : flag_initializers) {
        void* image = create_memory(1, 1, initial_flags);
        void* new_image = replacement_create_memory(1, 1, initial_flags);
        if (!image || !new_image) return 131;
        for (const auto changed_flags : flag_changes) {
            std::uint32_t previous_flags = 0;
            std::memcpy(&previous_flags, static_cast<const std::uint8_t*>(image) + 0x34, sizeof(previous_flags));
            const auto old_return = change_texture_flags(image, changed_flags);
            const auto new_return = replacement_change_texture_flags(new_image, changed_flags);
            if (old_return != (previous_flags & 0x07018197u) || new_return != old_return) {
                std::fprintf(stderr, "Texture flag return: before=%08x requested=%08x original=%08x replacement=%08x.\n",
                    previous_flags, changed_flags, old_return, new_return);
                return 189;
            }
            std::uint32_t old_header_flags = 0;
            std::uint32_t new_header_flags = 0;
            std::memcpy(&old_header_flags, static_cast<const std::uint8_t*>(image) + 0x34, sizeof(old_header_flags));
            std::memcpy(&new_header_flags, static_cast<const std::uint8_t*>(new_image) + 0x34, sizeof(new_header_flags));
            if (old_header_flags != new_header_flags) return 132;
        }
        free_image(image);
        replacement_free(new_image);
    }
    std::puts("Texture flag changes: original and replacement header state agree.");

    void* resource = create(7, 3, 0x27, 119);
    if (!resource || create(7, 3, 0x27, 119)) return 24;
    std::array<std::uint32_t, 3> info = {};
    texture_info(resource, info.data());
    if (info[0] != 7 || info[1] != 3 || info[2] != reinterpret_cast<std::uint32_t>(resource)) return 25;
    LockedTexture first = {}, nested = {};
    if (!lock(resource, &first, 2) || !lock(resource, &nested, 1) ||
        !first.pixels || !first.palette || first.pixels != nested.pixels || first.palette != nested.palette ||
        first.width != 7 || first.height != 3 || first.pitch < 7) return 26;
    unlock(resource);
    unlock(resource);
    free_texture(resource);
    resource = create(7, 3, 0x27, 119);
    if (!resource) return 27;
    free_texture(resource);
    std::puts("Texture contract: duplicate ID rejected; nested locks and ID reuse verified.");

    constexpr std::uint32_t key = 0x00ff00ff;
    set_key(key);
    void* source = create(8, 8, 0x27, 120);
    void* target = create(12, 12, 0, 121);
    if (!source || !target) return 7;
    std::array<std::uint32_t, 256> colors = {};
    colors[0] = 0xffff00ff;
    colors[1] = 0xff123456;
    colors[2] = 0xff93c5e2;
    set_palette(source, colors.data());
    LockedTexture src = {};
    if (!lock(source, &src, 2) || !src.pixels || src.width != 8 || src.height != 8 || src.pitch < 8) return 8;
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) src.pixels[y * src.pitch + x] = static_cast<std::uint8_t>((x + y) % 3);
    }
    unlock(source);
    if (!lock(source, &src, 1) || !src.pixels || !src.palette) return 9;
    std::vector<std::uint8_t> indices(src.pixels, src.pixels + src.pitch * src.height);
    std::array<std::uint32_t, 256> actual_palette = {};
    std::memcpy(actual_palette.data(), src.palette, sizeof(actual_palette));
    const std::uint32_t source_pitch = src.pitch;
    unlock(source);

    set_source(source);
    set_target(target);
    clip_mode(1);
    // The headless fixture has no SGL screen; SetConstantColor dereferences that screen.
    *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x6e93c) = 0xffffffff;
    *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x8d080) = 0xffffffff;

    const auto replacement_draw = resolve<void (__cdecl*)(int, int, int, int, int, int)>(replacement, "sgl_DrawBitmapRect");
    const auto replacement_stretch = resolve<void (__cdecl*)(int, int, int, int, int, int, int, int)>(replacement, "sgl_StretchBitmapRect");
    const auto replacement_constant = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(replacement, "sgl_SetConstantColor");
    if (!replacement_draw || !replacement_stretch || !replacement_constant) return 74;
    replacement_key(key);
    void* new_source = replacement_create_memory(8, 8, 0x27);
    void* new_target = replacement_create_memory(12, 12, 0);
    if (!new_source || !new_target) return 75;
    auto new_colors = colors;
    replacement_palette(new_source, new_colors.data());
    LockedTexture new_src = {};
    if (!replacement_lock(new_source, &new_src, 2)) return 76;
    for (std::uint32_t y = 0; y < 8; ++y) {
        std::memcpy(new_src.pixels + y * new_src.pitch, indices.data() + y * source_pitch, 8);
    }
    replacement_unlock(new_source);
    replacement_source(new_source);
    replacement_target(new_target);
    replacement_clip_mode(1);
    replacement_constant(0xffffffff);
    if (old_get_color() != new_get_color()) return 127;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context))) return 12;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = 8;
    description.Height = 8;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8_UINT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA input = {indices.data(), source_pitch, 0};
    ComPtr<ID3D11Texture2D> index_texture;
    if (FAILED(device->CreateTexture2D(&description, &input, &index_texture))) return 13;
    ComPtr<ID3D11ShaderResourceView> index_view;
    if (FAILED(device->CreateShaderResourceView(index_texture.Get(), nullptr, &index_view))) return 14;
    description.Width = 256;
    description.Height = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    input = {actual_palette.data(), sizeof(actual_palette), 0};
    ComPtr<ID3D11Texture2D> palette_texture;
    if (FAILED(device->CreateTexture2D(&description, &input, &palette_texture))) return 15;
    ComPtr<ID3D11ShaderResourceView> palette_view;
    if (FAILED(device->CreateShaderResourceView(palette_texture.Get(), nullptr, &palette_view))) return 16;
    std::array<std::uint32_t, 144> initial = {};
    initial.fill(0xff224466);
    description.Width = 12;
    description.Height = 12;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    input = {initial.data(), 48, 0};
    ComPtr<ID3D11Texture2D> output;
    if (FAILED(device->CreateTexture2D(&description, &input, &output))) return 17;
    ComPtr<ID3D11RenderTargetView> target_view;
    if (FAILED(device->CreateRenderTargetView(output.Get(), nullptr, &target_view))) return 18;
    sgl::Compositor compositor;
    if (FAILED(compositor.initialize(device.Get()))) return 19;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) return 21;
    const std::array<DrawCase, 9> cases = {{
        {1, 1, 3, 4, 5, 4, 5, 4, {0, 0, 12, 12}, 13},
        {1, 1, 3, 4, 5, 4, 5, 4, {5, 5, 7, 7}, 2},
        {6, 5, 0, 0, 2, 3, 2, 3, {0, 0, 12, 12}, 4},
        {0, 0, 10, 10, 1, 1, 1, 1, {0, 0, 12, 12}, 0},
        {1, 1, 1, 1, 5, 4, 10, 8, {0, 0, 12, 12}, 52},
        {1, 1, 1, 2, 3, 2, 5, 3, {0, 0, 12, 12}, std::nullopt},
        {1, 1, 1, 2, 3, 2, 5, 3, {2, 3, 5, 5}, std::nullopt},
        {0, 0, 2, 3, 7, 7, 3, 4, {0, 0, 12, 12}, std::nullopt},
        {0, 0, -2, -1, 4, 3, 8, 6, {0, 0, 12, 12}, std::nullopt},
    }};
    for (std::size_t case_index = 0; case_index < cases.size(); ++case_index) {
        const auto& item = cases[case_index];
        LockedTexture dst = {};
        if (!lock(target, &dst, 2) || !dst.pixels || dst.width != 12 || dst.height != 12 || dst.pitch < 48) return 10;
        for (std::uint32_t y = 0; y < 12; ++y) {
            auto row = reinterpret_cast<std::uint32_t*>(dst.pixels + y * dst.pitch);
            for (std::uint32_t x = 0; x < 12; ++x) row[x] = 0xff224466;
        }
        unlock(target);
        LockedTexture new_dst = {};
        if (!replacement_lock(new_target, &new_dst, 2)) return 77;
        for (std::uint32_t y = 0; y < 12; ++y) {
            std::memcpy(new_dst.pixels + y * new_dst.pitch, initial.data() + y * 12, 48);
        }
        replacement_unlock(new_target);
        clip_rect(item.clip.left, item.clip.top, item.clip.right, item.clip.bottom);
        replacement_clip_rect(item.clip.left, item.clip.top, item.clip.right, item.clip.bottom);
        if (item.width == item.destination_width && item.height == item.destination_height) {
            draw(item.sx, item.sy, item.dx, item.dy, item.width, item.height);
            replacement_draw(item.sx, item.sy, item.dx, item.dy, item.width, item.height);
        } else {
            stretch(item.sx, item.sy, item.dx, item.dy, item.width, item.height, item.destination_width, item.destination_height);
            replacement_stretch(item.sx, item.sy, item.dx, item.dy, item.width, item.height, item.destination_width, item.destination_height);
        }
        if (!lock(target, &dst, 1) || !dst.pixels) return 11;
        const std::uint32_t target_pitch = dst.pitch;
        std::vector<std::uint8_t> expected(dst.pixels, dst.pixels + dst.pitch * dst.height);
        unlock(target);
        if (!replacement_lock(new_target, &new_dst, 1)) return 78;
        for (std::uint32_t y = 0; y < 12; ++y) {
            if (std::memcmp(expected.data() + y * target_pitch, new_dst.pixels + y * new_dst.pitch, 48)) {
                std::fprintf(stderr, "Replacement differs from original in case %zu row %u.\n", case_index, y);
                return 79;
            }
        }
        replacement_unlock(new_target);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->UpdateSubresource(output.Get(), 0, nullptr, initial.data(), 48, 0);
        if (FAILED(compositor.copy_indexed(context.Get(), target_view.Get(), index_view.Get(), palette_view.Get(),
            12, 12,
            {static_cast<float>(item.sx), static_cast<float>(item.sy), static_cast<float>(item.sx + item.width), static_cast<float>(item.sy + item.height)},
            {static_cast<float>(item.dx), static_cast<float>(item.dy), static_cast<float>(item.dx + item.destination_width), static_cast<float>(item.dy + item.destination_height)},
            item.clip, key, sgl::Sampling::SglFixedPoint))) return 20;
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 22;
        unsigned mismatches = 0;
        unsigned changed = 0;
        for (std::uint32_t y = 0; y < 12; ++y) {
            const auto gpu = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            const auto reference = expected.data() + y * target_pitch;
            for (std::uint32_t x = 0; x < 12; ++x) {
                if (std::memcmp(gpu + x * 4, reference + x * 4, 4) != 0) {
                    std::uint32_t actual_pixel, reference_pixel;
                    std::memcpy(&actual_pixel, gpu + x * 4, sizeof(actual_pixel));
                    std::memcpy(&reference_pixel, reference + x * 4, sizeof(reference_pixel));
                    if (mismatches < 16) std::fprintf(stderr, "  (%u,%u): GPU %08x, SGL %08x\n", x, y, actual_pixel, reference_pixel);
                    ++mismatches;
                }
                std::uint32_t value;
                std::memcpy(&value, reference + x * 4, sizeof(value));
                if (value != 0xff224466) ++changed;
            }
        }
        context->Unmap(staging.Get(), 0);
        if (mismatches || (item.changed ? changed != *item.changed : changed == 0)) {
            std::fprintf(stderr, "Case %zu: %u changed pixels, %u mismatches.\n", case_index, changed, mismatches);
            return 23;
        }
        std::printf("Case %zu: %u changed pixels, %u mismatches.\n", case_index, changed, mismatches);
    }
    LockedTexture empty_old = {}, empty_new = {};
    if (!lock(target, &empty_old, 2) || !empty_old.pixels ||
        !replacement_lock(new_target, &empty_new, 2) || !empty_new.pixels) return 135;
    for (std::uint32_t y = 0; y < 12; ++y) {
        std::memcpy(empty_old.pixels + y * empty_old.pitch, initial.data() + y * 12, 48);
        std::memcpy(empty_new.pixels + y * empty_new.pitch, initial.data() + y * 12, 48);
    }
    unlock(target);
    replacement_unlock(new_target);
    draw(0, 0, 3, 4, 8, 0);
    replacement_draw(0, 0, 3, 4, 8, 0);
    if (!lock(target, &empty_old, 1) || !empty_old.pixels ||
        !replacement_lock(new_target, &empty_new, 1) || !empty_new.pixels) return 136;
    for (std::uint32_t y = 0; y < 12; ++y) {
        const auto* expected = reinterpret_cast<const std::uint8_t*>(initial.data() + y * 12);
        if (std::memcmp(empty_old.pixels + y * empty_old.pitch, expected, 48) ||
            std::memcmp(empty_new.pixels + y * empty_new.pitch, expected, 48)) return 137;
    }
    unlock(target);
    replacement_unlock(new_target);
    std::puts("Empty bitmap rectangle: original and replacement leave the target unchanged.");
    const RECT bitmap_region{1, 1, 7, 7};
    const RECT bitmap_cut{3, 3, 5, 5};
    auto verify_region_draw = [&](int mode, RECT clip, bool populated) {
        void* old_bitmap_region = old_create_region();
        void* new_bitmap_region = new_create_region();
        if (!old_bitmap_region || !new_bitmap_region) {
            if (old_bitmap_region) old_free_region(old_bitmap_region);
            if (new_bitmap_region) new_free_region(new_bitmap_region);
            return false;
        }
        void* old_previous = old_set_region(old_bitmap_region);
        void* new_previous = new_set_region(new_bitmap_region);
        std::array<std::int32_t, 4> old_clip_before = {}, new_clip_before = {};
        get_clip_rect(old_clip_before.data());
        replacement_get_clip_rect(new_clip_before.data());
        if (populated) {
            old_add(&bitmap_region); new_add(&bitmap_region);
            old_sub(&bitmap_cut); new_sub(&bitmap_cut);
        }
        clip_rect(clip.left, clip.top, clip.right, clip.bottom);
        replacement_clip_rect(clip.left, clip.top, clip.right, clip.bottom);
        const int old_mode_before = clip_mode(mode);
        const int new_mode_before = replacement_clip_mode(mode);
        LockedTexture old_pixels = {}, new_pixels = {};
        const bool old_seed_locked = lock(target, &old_pixels, 2) != 0;
        const bool new_seed_locked = replacement_lock(new_target, &new_pixels, 2) != 0;
        bool success = old_seed_locked && old_pixels.pixels && new_seed_locked && new_pixels.pixels;
        if (success) {
            for (std::uint32_t y = 0; y < 12; ++y) {
                std::memcpy(old_pixels.pixels + y * old_pixels.pitch, initial.data() + y * 12, 48);
                std::memcpy(new_pixels.pixels + y * new_pixels.pitch, initial.data() + y * 12, 48);
            }
        }
        if (old_seed_locked) unlock(target);
        if (new_seed_locked) replacement_unlock(new_target);
        if (success) {
            draw(0, 0, 0, 0, 8, 8);
            replacement_draw(0, 0, 0, 0, 8, 8);
            const bool old_read_locked = lock(target, &old_pixels, 1) != 0;
            const bool new_read_locked = replacement_lock(new_target, &new_pixels, 1) != 0;
            success = old_read_locked && old_pixels.pixels && new_read_locked && new_pixels.pixels;
            bool changed = false;
            if (success) {
                for (std::uint32_t y = 0; y < 12; ++y) {
                    for (std::uint32_t x = 0; x < 12; ++x) {
                        const auto* old_pixel = old_pixels.pixels + y * old_pixels.pitch + x * 4;
                        const auto* new_pixel = new_pixels.pixels + y * new_pixels.pitch + x * 4;
                        if (std::memcmp(old_pixel, new_pixel, 4) != 0) success = false;
                        std::uint32_t value = 0;
                        std::memcpy(&value, new_pixel, sizeof(value));
                        const bool in_region = x >= 1 && x < 7 && y >= 1 && y < 7 &&
                            !(x >= 3 && x < 5 && y >= 3 && y < 5);
                        const bool expected_visible = populated && in_region && (mode != 3 ||
                            (x >= static_cast<std::uint32_t>(clip.left) && x < static_cast<std::uint32_t>(clip.right) &&
                             y >= static_cast<std::uint32_t>(clip.top) && y < static_cast<std::uint32_t>(clip.bottom)));
                        if (!expected_visible && value != initial[y * 12 + x]) success = false;
                        if (expected_visible && value != initial[y * 12 + x]) changed = true;
                    }
                }
                success = success && changed == populated;
            }
            if (old_read_locked) unlock(target);
            if (new_read_locked) replacement_unlock(new_target);
        }
        clip_mode(old_mode_before);
        replacement_clip_mode(new_mode_before);
        clip_rect(old_clip_before[0], old_clip_before[1], old_clip_before[2], old_clip_before[3]);
        replacement_clip_rect(new_clip_before[0], new_clip_before[1], new_clip_before[2], new_clip_before[3]);
        old_set_region(old_previous);
        new_set_region(new_previous);
        old_free_region(old_bitmap_region);
        new_free_region(new_bitmap_region);
        return success;
    };
    if (!verify_region_draw(2, {0, 0, 12, 12}, true) ||
        !verify_region_draw(3, {2, 2, 6, 6}, true) ||
        !verify_region_draw(2, {0, 0, 12, 12}, false)) return 82;
    std::puts("Bitmap region clipping: original and replacement match for modes 2, 3 and an empty region.");
    // Keep the same surfaces selected across writes, palette changes and draws.
    LockedTexture old_change = {}, new_change = {};
    if (!lock(source, &old_change, 2) || !replacement_lock(new_source, &new_change, 2)) return 80;
    old_change.pixels[0] = new_change.pixels[0] = 2;
    unlock(source);
    replacement_unlock(new_source);
    std::array<std::uint32_t, 256> updated_colors = {};
    updated_colors[0] = 0xffff00ff;
    updated_colors[1] = 0xffaabbcc;
    updated_colors[2] = 0xff123abc;
    auto new_updated_colors = updated_colors;
    set_palette(source, updated_colors.data());
    replacement_palette(new_source, new_updated_colors.data());
    clip_rect(0, 0, 12, 12);
    replacement_clip_rect(0, 0, 12, 12);
    draw(0, 0, 0, 0, 8, 8);
    replacement_draw(0, 0, 0, 0, 8, 8);
    draw(0, 0, 4, 4, 8, 8);
    replacement_draw(0, 0, 4, 4, 8, 8);
    LockedTexture final_old = {}, final_new = {};
    if (!lock(target, &final_old, 1) || !replacement_lock(new_target, &final_new, 1)) return 81;
    for (std::uint32_t y = 0; y < 12; ++y) {
        if (std::memcmp(final_old.pixels + y * final_old.pitch, final_new.pixels + y * final_new.pitch, 48)) return 82;
    }
    unlock(target);
    replacement_unlock(new_target);
    set_source(nullptr);
    set_target(nullptr);
    replacement_source(nullptr);
    replacement_target(nullptr);
    free_texture(source);
    free_texture(target);
    replacement_free(new_source);
    replacement_free(new_target);
    std::puts("Consecutive draws and source changes: original and replacement agree.");

    constexpr std::array<std::uint32_t, 5> indexed_flags = {0x20u, 0x21u, 0x23u, 0x27u, 0x03000027u};
    for (const auto flags : indexed_flags) {
        void* old_indexed_draw = create_memory(8, 8, flags);
        void* new_indexed_draw = replacement_create_memory(8, 8, flags);
        void* old_output = create_memory(12, 12, 0);
        void* new_output = replacement_create_memory(12, 12, 0);
        if (!old_indexed_draw || !new_indexed_draw || !old_output || !new_output) return 138;

        std::array<std::uint32_t, 256> palette = {};
        palette[0] = 0xffff00ff;
        palette[1] = 0xff123456;
        palette[2] = 0xff93c5e2;
        auto new_palette = palette;
        set_palette(old_indexed_draw, palette.data());
        replacement_palette(new_indexed_draw, new_palette.data());

        LockedTexture old_pixels = {}, new_pixels = {};
        if (!lock(old_indexed_draw, &old_pixels, 2) || !replacement_lock(new_indexed_draw, &new_pixels, 2)) return 139;
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const auto index = static_cast<std::uint8_t>((x + y) % 3);
                old_pixels.pixels[y * old_pixels.pitch + x] = index;
                new_pixels.pixels[y * new_pixels.pitch + x] = index;
            }
        }
        unlock(old_indexed_draw);
        replacement_unlock(new_indexed_draw);

        set_source(old_indexed_draw);
        set_target(old_output);
        replacement_source(new_indexed_draw);
        replacement_target(new_output);
        clip_mode(1);
        replacement_clip_mode(1);
        clip_rect(0, 0, 12, 12);
        replacement_clip_rect(0, 0, 12, 12);
        if (flags == 0x27u) {
            const auto old_previous = old_blend(2);
            const auto new_previous = new_blend(2);
            if ((old_previous & 0xff) != (new_previous & 0xff)) {
                std::fprintf(stderr, "Blend state before indexed draw flags 0x%02x: original=%u replacement=%u\n",
                    flags, old_previous & 0xff, new_previous & 0xff);
                return 143;
            }
        }
        if (!lock(old_output, &old_pixels, 2) || !replacement_lock(new_output, &new_pixels, 2)) return 140;
        for (std::uint32_t y = 0; y < 12; ++y) {
            auto* old_row = reinterpret_cast<std::uint32_t*>(old_pixels.pixels + y * old_pixels.pitch);
            auto* new_row = reinterpret_cast<std::uint32_t*>(new_pixels.pixels + y * new_pixels.pitch);
            std::fill_n(old_row, 12, 0xff224466);
            std::fill_n(new_row, 12, 0xff224466);
        }
        unlock(old_output);
        replacement_unlock(new_output);
        draw(0, 0, 0, 0, 8, 8);
        replacement_draw(0, 0, 0, 0, 8, 8);

        if (!lock(old_output, &old_pixels, 1) || !replacement_lock(new_output, &new_pixels, 1)) return 141;
        bool matches = true;
        for (std::uint32_t y = 0; y < 12; ++y) {
            for (std::uint32_t x = 0; x < 12; ++x) {
                std::uint32_t original_pixel = 0, replacement_pixel = 0;
                std::memcpy(&original_pixel, old_pixels.pixels + y * old_pixels.pitch + x * 4, 4);
                std::memcpy(&replacement_pixel, new_pixels.pixels + y * new_pixels.pitch + x * 4, 4);
                if (original_pixel == replacement_pixel) continue;
                matches = false;
                std::fprintf(stderr, "  first differing pixel (%u,%u): original=%08x replacement=%08x\n",
                    x, y, original_pixel, replacement_pixel);
                break;
            }
            if (!matches) break;
        }
        unlock(old_output);
        replacement_unlock(new_output);
        if (!matches) {
            std::fprintf(stderr, "Indexed source flags 0x%02x: original and replacement differ.\n", flags);
            return 142;
        }

        set_source(nullptr);
        set_target(nullptr);
        replacement_source(nullptr);
        replacement_target(nullptr);
        free_image(old_indexed_draw);
        replacement_free(new_indexed_draw);
        free_image(old_output);
        replacement_free(new_output);
    }
    std::puts("Indexed drawing and additive blend mode 3: original and replacement agree.");

    {
        void* old_multiply_source = create_memory(8, 8, 0x23);
        void* new_multiply_source = replacement_create_memory(8, 8, 0x23);
        void* old_multiply_target = create_memory(12, 12, 0);
        void* new_multiply_target = replacement_create_memory(12, 12, 0);
        if (!old_multiply_source || !new_multiply_source || !old_multiply_target || !new_multiply_target) return 195;

        std::array<std::uint32_t, 256> palette = {};
        palette[0] = 0xff204080;
        palette[1] = 0xff80c040;
        palette[2] = 0xffe060a0;
        auto new_palette = palette;
        set_palette(old_multiply_source, palette.data());
        replacement_palette(new_multiply_source, new_palette.data());

        LockedTexture old_pixels = {}, new_pixels = {};
        if (!lock(old_multiply_source, &old_pixels, 2) || !replacement_lock(new_multiply_source, &new_pixels, 2)) return 196;
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const auto index = static_cast<std::uint8_t>((x + y) % 3);
                old_pixels.pixels[y * old_pixels.pitch + x] = index;
                new_pixels.pixels[y * new_pixels.pitch + x] = index;
            }
        }
        unlock(old_multiply_source);
        replacement_unlock(new_multiply_source);
        *(static_cast<std::uint8_t*>(old_multiply_source) + 0x37) = 2;
        *(static_cast<std::uint8_t*>(new_multiply_source) + 0x37) = 2;

        set_source(old_multiply_source);
        set_target(old_multiply_target);
        replacement_source(new_multiply_source);
        replacement_target(new_multiply_target);
        clip_mode(1);
        replacement_clip_mode(1);
        clip_rect(0, 0, 12, 12);
        replacement_clip_rect(0, 0, 12, 12);

        constexpr std::array<std::uint32_t, 2> modulation_colors = {0xffffffffu, 0xff804020u};
        for (const auto modulation : modulation_colors) {
            if (!lock(old_multiply_target, &old_pixels, 2) || !replacement_lock(new_multiply_target, &new_pixels, 2)) return 197;
            for (std::uint32_t y = 0; y < 12; ++y) {
                auto* old_row = reinterpret_cast<std::uint32_t*>(old_pixels.pixels + y * old_pixels.pitch);
                auto* new_row = reinterpret_cast<std::uint32_t*>(new_pixels.pixels + y * new_pixels.pitch);
                std::fill_n(old_row, 12, 0xffa0c080);
                std::fill_n(new_row, 12, 0xffa0c080);
            }
            unlock(old_multiply_target);
            replacement_unlock(new_multiply_target);

            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x6e93c) = modulation;
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x8d080) = modulation;
            replacement_constant(modulation);
            draw(0, 0, 2, 2, 8, 8);
            replacement_draw(0, 0, 2, 2, 8, 8);

            if (!lock(old_multiply_target, &old_pixels, 1) || !replacement_lock(new_multiply_target, &new_pixels, 1)) return 198;
            for (std::uint32_t y = 0; y < 12; ++y) {
                if (std::memcmp(old_pixels.pixels + y * old_pixels.pitch,
                    new_pixels.pixels + y * new_pixels.pitch, 12 * sizeof(std::uint32_t))) {
                    std::fprintf(stderr, "Indexed multiply mode 2 color 0x%08x row %u differs.\n", modulation, y);
                    for (std::uint32_t x = 0; x < 12; ++x) {
                        std::uint32_t old_value = 0, new_value = 0;
                        std::memcpy(&old_value, old_pixels.pixels + y * old_pixels.pitch + x * 4, 4);
                        std::memcpy(&new_value, new_pixels.pixels + y * new_pixels.pitch + x * 4, 4);
                        if (old_value != new_value)
                            std::fprintf(stderr, "  x=%u original=%08x replacement=%08x\n", x, old_value, new_value);
                    }
                    return 199;
                }
            }
            unlock(old_multiply_target);
            replacement_unlock(new_multiply_target);
        }

        set_source(nullptr);
        set_target(nullptr);
        replacement_source(nullptr);
        replacement_target(nullptr);
        free_image(old_multiply_source);
        free_image(old_multiply_target);
        replacement_free(new_multiply_source);
        replacement_free(new_multiply_target);
    }
    std::puts("Indexed multiply blend mode 2: original and replacement agree.");

    struct BgraDrawState { std::uint32_t flags; std::uint32_t blend; std::uint32_t color; };
    constexpr BgraDrawState bgra_draw_states[] = {
        {0u, 0u, 0xffffffffu}, {0u, 1u, 0xffffffffu}, {1u, 1u, 0xffffffffu},
        {3u, 1u, 0xffffffffu}, {3u, 3u, 0xffffffffu}, {3u, 3u, 0xff804020u},
        {7u, 1u, 0xffffffffu}, {0x05000010u, 1u, 0xffffffffu}
    };
    for (const auto [flags, blend, color] : bgra_draw_states) {
        void* old_bgra = create_memory(8, 8, flags);
        void* new_bgra = replacement_create_memory(8, 8, flags);
        void* old_output = create_memory(12, 12, 0);
        void* new_output = replacement_create_memory(12, 12, 0);
        if (!old_bgra || !new_bgra || !old_output || !new_output) return 83;
        LockedTexture old_pixels = {}, new_pixels = {};
        if (!lock(old_bgra, &old_pixels, 2) || !replacement_lock(new_bgra, &new_pixels, 2)) return 84;
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const std::uint32_t pixel = (x + y) % 3 == 0 ? key :
                    ((x * 37u) << 24) | (x * 29u << 16) | (y * 31u << 8) | (x * 3u + y);
                std::memcpy(old_pixels.pixels + y * old_pixels.pitch + x * 4, &pixel, 4);
                std::memcpy(new_pixels.pixels + y * new_pixels.pitch + x * 4, &pixel, 4);
            }
        }
        unlock(old_bgra);
        replacement_unlock(new_bgra);
        set_source(old_bgra);
        set_target(old_output);
        replacement_source(new_bgra);
        replacement_target(new_output);
        const auto old_source_mode = *(static_cast<const std::uint8_t*>(old_bgra) + 0x37);
        const auto new_source_mode = *(static_cast<const std::uint8_t*>(new_bgra) + 0x37);
        const auto old_previous_blend = old_blend(blend);
        const auto new_previous_blend = new_blend(blend);
        if ((old_previous_blend & 0xff) != (new_previous_blend & 0xff)) {
            std::fprintf(stderr, "Blend state before BGRA draw flags 0x%08x mode %u: original=%u replacement=%u source modes=%u/%u\n",
                flags, blend, old_previous_blend & 0xff, new_previous_blend & 0xff,
                old_source_mode, new_source_mode);
            return 167;
        }
        if (*(static_cast<const std::uint8_t*>(old_bgra) + 0x37) != old_source_mode ||
            *(static_cast<const std::uint8_t*>(new_bgra) + 0x37) != new_source_mode) return 168;
        if (flags == 3u && blend == 3u) {
            *(static_cast<std::uint8_t*>(old_bgra) + 0x37) = 3;
            *(static_cast<std::uint8_t*>(new_bgra) + 0x37) = 3;
        }
        *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x6e93c) = color;
        *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x8d080) = color;
        replacement_constant(color);
        if (old_get_color() != color || new_get_color() != color) {
            std::fprintf(stderr, "Could not set BGRA blend modulation color 0x%08x.\n", color);
            return 169;
        }
        std::size_t verified_cases = 0;
        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto& item = cases[index];
            if (!lock(old_output, &old_pixels, 2) || !replacement_lock(new_output, &new_pixels, 2)) return 85;
            for (std::uint32_t y = 0; y < 12; ++y) {
                std::memcpy(old_pixels.pixels + y * old_pixels.pitch, initial.data() + y * 12, 48);
                std::memcpy(new_pixels.pixels + y * new_pixels.pitch, initial.data() + y * 12, 48);
            }
            unlock(old_output);
            replacement_unlock(new_output);
            ++verified_cases;
            clip_rect(item.clip.left, item.clip.top, item.clip.right, item.clip.bottom);
            replacement_clip_rect(item.clip.left, item.clip.top, item.clip.right, item.clip.bottom);
            if (item.width == item.destination_width && item.height == item.destination_height) {
                draw(item.sx, item.sy, item.dx, item.dy, item.width, item.height);
                replacement_draw(item.sx, item.sy, item.dx, item.dy, item.width, item.height);
            } else {
                stretch(item.sx, item.sy, item.dx, item.dy, item.width, item.height, item.destination_width, item.destination_height);
                replacement_stretch(item.sx, item.sy, item.dx, item.dy, item.width, item.height, item.destination_width, item.destination_height);
            }
            if (!lock(old_output, &old_pixels, 1) || !replacement_lock(new_output, &new_pixels, 1)) return 86;
            for (std::uint32_t y = 0; y < 12; ++y) {
                if (std::memcmp(old_pixels.pixels + y * old_pixels.pitch, new_pixels.pixels + y * new_pixels.pitch, 48)) {
                    std::fprintf(stderr, "BGRA flags %u blend %u color 0x%08x case %zu row %u differs.\n",
                        flags, blend, color, index, y);
                    for (std::uint32_t x = 0; x < 12; ++x) {
                        std::uint32_t old_value, new_value;
                        std::memcpy(&old_value, old_pixels.pixels + y * old_pixels.pitch + x * 4, 4);
                        std::memcpy(&new_value, new_pixels.pixels + y * new_pixels.pitch + x * 4, 4);
                        if (old_value != new_value) std::fprintf(stderr, "  x=%u original=%08x replacement=%08x\n", x, old_value, new_value);
                    }
                    return 87;
                }
            }
            unlock(old_output);
            replacement_unlock(new_output);
        }
        set_source(nullptr);
        set_target(nullptr);
        replacement_source(nullptr);
        replacement_target(nullptr);
        free_image(old_bgra);
        free_image(old_output);
        replacement_free(new_bgra);
        replacement_free(new_output);
        std::printf("BGRA flags %u blend %u color 0x%08x: %zu drawing cases match.\n",
            flags, blend, color, verified_cases);
    }
    void* old_alpha = create_memory(16, 16, 3);
    void* new_alpha = replacement_create_memory(16, 16, 3);
    void* old_alpha_target = create_memory(16, 16, 0);
    void* new_alpha_target = replacement_create_memory(16, 16, 0);
    if (!old_alpha || !new_alpha || !old_alpha_target || !new_alpha_target) return 102;
    LockedTexture old_alpha_pixels = {}, new_alpha_pixels = {};
    if (!lock(old_alpha, &old_alpha_pixels, 2) || !replacement_lock(new_alpha, &new_alpha_pixels, 2)) return 103;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const std::uint32_t pixel = (i << 24) | ((255 - i) << 16) | (i << 8) | ((i * 73) & 255);
        std::memcpy(old_alpha_pixels.pixels + (i / 16) * old_alpha_pixels.pitch + (i % 16) * 4, &pixel, 4);
        std::memcpy(new_alpha_pixels.pixels + (i / 16) * new_alpha_pixels.pitch + (i % 16) * 4, &pixel, 4);
    }
    unlock(old_alpha);
    replacement_unlock(new_alpha);
    if (!lock(old_alpha_target, &old_alpha_pixels, 2) || !replacement_lock(new_alpha_target, &new_alpha_pixels, 2)) return 104;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const std::uint32_t pixel = ((255 - i) << 24) | (i << 16) | ((255 - i) << 8) | ((i * 151) & 255);
        std::memcpy(old_alpha_pixels.pixels + (i / 16) * old_alpha_pixels.pitch + (i % 16) * 4, &pixel, 4);
        std::memcpy(new_alpha_pixels.pixels + (i / 16) * new_alpha_pixels.pitch + (i % 16) * 4, &pixel, 4);
    }
    unlock(old_alpha_target);
    replacement_unlock(new_alpha_target);
    set_source(old_alpha);
    set_target(old_alpha_target);
    replacement_source(new_alpha);
    replacement_target(new_alpha_target);
    clip_rect(0, 0, 16, 16);
    replacement_clip_rect(0, 0, 16, 16);
    draw(0, 0, 0, 0, 16, 16);
    replacement_draw(0, 0, 0, 0, 16, 16);
    if (!lock(old_alpha_target, &old_alpha_pixels, 1) || !replacement_lock(new_alpha_target, &new_alpha_pixels, 1)) return 105;
    for (std::uint32_t y = 0; y < 16; ++y) {
        if (std::memcmp(old_alpha_pixels.pixels + y * old_alpha_pixels.pitch,
            new_alpha_pixels.pixels + y * new_alpha_pixels.pitch, 64)) {
            std::fprintf(stderr, "Alpha range differs at row %u.\n", y);
            return 106;
        }
    }
    unlock(old_alpha_target);
    replacement_unlock(new_alpha_target);
    set_source(nullptr);
    set_target(nullptr);
    replacement_source(nullptr);
    replacement_target(nullptr);
    free_image(old_alpha);
    free_image(old_alpha_target);
    replacement_free(new_alpha);
    replacement_free(new_alpha_target);
    std::puts("BGRA alpha range: all 256 source alpha values match.");
    const auto old_alpha_mask = resolve<int (__cdecl*)(std::uint32_t, int, int, int, int, int, int)>(
        library, "sgl_DrawBitmapRectWithAlphaMask");
    const auto new_alpha_mask = resolve<int (__cdecl*)(std::uint32_t, int, int, int, int, int, int)>(
        replacement, "sgl_DrawBitmapRectWithAlphaMask");
    const auto new_set_constant = resolve<std::uint32_t (__cdecl*)(std::uint32_t)>(replacement, "sgl_SetConstantColor");
    if (!old_alpha_mask || !new_alpha_mask || !new_set_constant) return 184;
    void* old_mask_source = create_memory(16, 16, 0x20);
    void* new_mask_source = replacement_create_memory(16, 16, 0x20);
    void* old_mask = create_memory(16, 16, 0x20);
    void* new_mask = replacement_create_memory(16, 16, 0x20);
    void* old_mask_target = create_memory(16, 16, 0);
    void* new_mask_target = replacement_create_memory(16, 16, 0);
    if (!old_mask_source || !new_mask_source || !old_mask || !new_mask || !old_mask_target || !new_mask_target)
        return 185;
    std::array<std::uint32_t, 256> mask_source_palette = {};
    for (std::uint32_t i = 0; i < 256; ++i) {
        mask_source_palette[i] = 0xff000000u | ((i * 37u & 255u) << 16) |
            ((i * 71u & 255u) << 8) | (i * 109u & 255u);
    }
    if (set_palette(old_mask_source, mask_source_palette.data()) !=
        replacement_palette(new_mask_source, mask_source_palette.data())) return 186;
    LockedTexture old_mask_pixels = {}, new_mask_pixels = {};
    if (!lock(old_mask_source, &old_mask_pixels, 2) || !replacement_lock(new_mask_source, &new_mask_pixels, 2)) return 187;
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const auto index = static_cast<std::uint8_t>(200);
            old_mask_pixels.pixels[y * old_mask_pixels.pitch + x] = index;
            new_mask_pixels.pixels[y * new_mask_pixels.pitch + x] = index;
        }
    }
    unlock(old_mask_source);
    replacement_unlock(new_mask_source);
    if (!lock(old_mask, &old_mask_pixels, 2) || !replacement_lock(new_mask, &new_mask_pixels, 2)) return 188;
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const auto alpha = static_cast<std::uint8_t>(y * 16 + x);
            old_mask_pixels.pixels[y * old_mask_pixels.pitch + x] = alpha;
            new_mask_pixels.pixels[y * new_mask_pixels.pitch + x] = alpha;
        }
    }
    unlock(old_mask);
    replacement_unlock(new_mask);
    std::array<std::uint32_t, 256> mask_target_initial = {};
    if (!lock(old_mask_target, &old_mask_pixels, 2) || !replacement_lock(new_mask_target, &new_mask_pixels, 2)) return 189;
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const auto pixel = 0xff5a9bd3u;
            mask_target_initial[y * 16 + x] = pixel;
            std::memcpy(old_mask_pixels.pixels + y * old_mask_pixels.pitch + x * 4, &pixel, 4);
            std::memcpy(new_mask_pixels.pixels + y * new_mask_pixels.pitch + x * 4, &pixel, 4);
        }
    }
    unlock(old_mask_target);
    replacement_unlock(new_mask_target);
    set_source(old_mask_source);
    set_target(old_mask_target);
    replacement_source(new_mask_source);
    replacement_target(new_mask_target);
    const auto alpha_mask_modulation = old_get_color();
    new_set_constant(alpha_mask_modulation);
    clip_mode(1);
    replacement_clip_mode(1);
    struct AlphaMaskCase { int sx, sy, dx, dy, width, height; RECT clip; };
    const std::array<AlphaMaskCase, 4> alpha_mask_cases{{
        {0, 0, 0, 0, 16, 16, {0, 0, 16, 16}},
        {2, 3, 5, 4, 8, 7, {6, 5, 11, 10}},
        {0, 0, 4, 4, 4, 4, {9, 9, 15, 15}},
        {0, 0, 4, 4, 4, 4, {8, 4, 12, 8}},
    }};
    const std::array<std::uint32_t, 3> modulation_colors{
        alpha_mask_modulation, 0xffc08040u, 0x8060b0e0u
    };
    for (const auto modulation : modulation_colors) {
        old_set_color(modulation);
        new_set_constant(modulation);
        if (old_get_color() != modulation || new_get_color() != modulation) return 194;
        for (std::size_t test = 0; test < alpha_mask_cases.size(); ++test) {
            if (!lock(old_mask_target, &old_mask_pixels, 2) || !replacement_lock(new_mask_target, &new_mask_pixels, 2)) return 190;
            for (std::uint32_t y = 0; y < 16; ++y) {
                std::memcpy(old_mask_pixels.pixels + y * old_mask_pixels.pitch, mask_target_initial.data() + y * 16, 64);
                std::memcpy(new_mask_pixels.pixels + y * new_mask_pixels.pitch, mask_target_initial.data() + y * 16, 64);
            }
            unlock(old_mask_target);
            replacement_unlock(new_mask_target);
            const auto& item = alpha_mask_cases[test];
            clip_rect(item.clip.left, item.clip.top, item.clip.right, item.clip.bottom);
            replacement_clip_rect(item.clip.left, item.clip.top, item.clip.right, item.clip.bottom);
            const int old_result = old_alpha_mask(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(old_mask)),
                item.sx, item.sy, item.dx, item.dy, item.width, item.height);
            const int new_result = new_alpha_mask(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(new_mask)),
                item.sx, item.sy, item.dx, item.dy, item.width, item.height);
            if (old_result != 1 || new_result != old_result) return 191;
            if (!lock(old_mask_target, &old_mask_pixels, 1) || !replacement_lock(new_mask_target, &new_mask_pixels, 1)) return 192;
            for (std::uint32_t y = 0; y < 16; ++y) {
                for (std::uint32_t x = 0; x < 16; ++x) {
                    std::uint32_t original_pixel = 0, replacement_pixel = 0;
                    std::memcpy(&original_pixel, old_mask_pixels.pixels + y * old_mask_pixels.pitch + x * 4, 4);
                    std::memcpy(&replacement_pixel, new_mask_pixels.pixels + y * new_mask_pixels.pitch + x * 4, 4);
                    if (original_pixel != replacement_pixel) {
                        std::fprintf(stderr,
                            "Alpha-mask mismatch tint=%08x case=%zu at (%u,%u): original=%08x replacement=%08x\n",
                            modulation, test, x, y, original_pixel, replacement_pixel);
                        unlock(old_mask_target);
                        replacement_unlock(new_mask_target);
                        return 193;
                    }
                }
            }
            unlock(old_mask_target);
            replacement_unlock(new_mask_target);
        }
    }
    old_set_color(alpha_mask_modulation);
    new_set_constant(alpha_mask_modulation);
    set_source(nullptr);
    set_target(nullptr);
    replacement_source(nullptr);
    replacement_target(nullptr);
    free_texture(old_mask_source);
    free_texture(old_mask);
    free_texture(old_mask_target);
    replacement_free(new_mask_source);
    replacement_free(new_mask);
    replacement_free(new_mask_target);
    clip_mode(0);
    replacement_clip_mode(0);
    std::puts("Indexed alpha-mask drawing: original and D3D11 replacement match for 3 modulation colors, full-frame and clipped cases.");
    const auto old_blt = resolve<void (__cdecl*)(int, int, int, int, int, int, std::uint32_t)>(library, "sgl_Blt");
    const auto new_blt = resolve<void (__cdecl*)(int, int, int, int, int, int, std::uint32_t)>(replacement, "sgl_Blt");
    if (!old_blt || !new_blt) return 107;
    void* old_blt_source = create_memory(8, 8, 0);
    void* new_blt_source = replacement_create_memory(8, 8, 0);
    void* old_blt_target = create_memory(12, 12, 0);
    void* new_blt_target = replacement_create_memory(12, 12, 0);
    if (!old_blt_source || !new_blt_source || !old_blt_target || !new_blt_target) return 108;
    LockedTexture old_blt_pixels = {}, new_blt_pixels = {};
    if (!lock(old_blt_source, &old_blt_pixels, 2) || !replacement_lock(new_blt_source, &new_blt_pixels, 2)) return 109;
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::uint32_t pixel = (x * 31u << 24) | (y * 29u << 16) | (x * 17u << 8) | y;
            std::memcpy(old_blt_pixels.pixels + y * old_blt_pixels.pitch + x * 4, &pixel, 4);
            std::memcpy(new_blt_pixels.pixels + y * new_blt_pixels.pitch + x * 4, &pixel, 4);
        }
    }
    unlock(old_blt_source);
    replacement_unlock(new_blt_source);
    set_source(old_blt_source);
    set_target(old_blt_target);
    replacement_source(new_blt_source);
    replacement_target(new_blt_target);
    struct BlitCase { int sx, sy, dx, dy, width, height; std::uint32_t flags; };
    for (const auto& item : std::array<BlitCase, 4>{{{1, 1, 3, 4, 5, 4, 0},
        {0, 0, -2, -1, 8, 8, 2}, {0, 0, 10, 10, 8, 8, 0}, {0, 0, 2, 2, 8, 8, 4}}}) {
        if (!lock(old_blt_target, &old_blt_pixels, 2) || !replacement_lock(new_blt_target, &new_blt_pixels, 2)) return 110;
        for (std::uint32_t y = 0; y < 12; ++y) {
            std::memcpy(old_blt_pixels.pixels + y * old_blt_pixels.pitch, initial.data() + y * 12, 48);
            std::memcpy(new_blt_pixels.pixels + y * new_blt_pixels.pitch, initial.data() + y * 12, 48);
        }
        unlock(old_blt_target);
        replacement_unlock(new_blt_target);
        old_blt(item.sx, item.sy, item.dx, item.dy, item.width, item.height, item.flags);
        new_blt(item.sx, item.sy, item.dx, item.dy, item.width, item.height, item.flags);
        if (!lock(old_blt_target, &old_blt_pixels, 1) || !replacement_lock(new_blt_target, &new_blt_pixels, 1)) return 111;
        for (std::uint32_t y = 0; y < 12; ++y) {
            if (std::memcmp(old_blt_pixels.pixels + y * old_blt_pixels.pitch, new_blt_pixels.pixels + y * new_blt_pixels.pitch, 48)) {
                std::fprintf(stderr, "Blit differs at row %u.\n", y);
                return 112;
            }
        }
        unlock(old_blt_target);
        replacement_unlock(new_blt_target);
    }
    set_source(old_blt_source);
    set_target(old_blt_source);
    replacement_source(new_blt_source);
    replacement_target(new_blt_source);
    struct SelfBlitProbe { int dx, dy; std::uint32_t flags; };
    for (const auto& probe : std::array<SelfBlitProbe, 6>{{{1, 1, 0}, {1, 1, 2}, {1, 1, 4}, {1, 0, 4}, {0, 1, 4}, {0, 0, 4}}}) {
        if (!lock(old_blt_source, &old_blt_pixels, 2) || !replacement_lock(new_blt_source, &new_blt_pixels, 2)) return 164;
        std::array<std::uint32_t, 64> expected{};
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const std::uint32_t pixel = (x * 31u << 24) | (y * 29u << 16) | (x * 17u << 8) | y;
                std::memcpy(old_blt_pixels.pixels + y * old_blt_pixels.pitch + x * 4, &pixel, 4);
                std::memcpy(new_blt_pixels.pixels + y * new_blt_pixels.pitch + x * 4, &pixel, 4);
                expected[y * 8 + x] = pixel;
            }
        }
        unlock(old_blt_source);
        replacement_unlock(new_blt_source);

        const auto snapshot = expected;
        for (int y = 0; y < 8 - probe.dy; ++y) {
            for (int x = 0; x < 8 - probe.dx; ++x) {
                expected[static_cast<std::size_t>(y + probe.dy) * 8 + x + probe.dx] =
                    snapshot[static_cast<std::size_t>(y) * 8 + x];
            }
        }
        new_blt(0, 0, probe.dx, probe.dy, 8 - probe.dx, 8 - probe.dy, probe.flags);
        if (!replacement_lock(new_blt_source, &new_blt_pixels, 1)) return 165;
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                std::uint32_t actual = 0;
                std::memcpy(&actual, new_blt_pixels.pixels + y * new_blt_pixels.pitch + x * 4, 4);
                if (actual != expected[y * 8 + x]) {
                    std::fprintf(stderr,
                        "Snapshot self-blit mismatch flags=%u shift=(%d,%d) at (%u,%u): expected=%08x actual=%08x.\n",
                        probe.flags, probe.dx, probe.dy, x, y, expected[y * 8 + x], actual);
                    return 166;
                }
            }
        }
        std::printf("Snapshot self-blit flags=%u shift=(%d,%d): pass.\n", probe.flags, probe.dx, probe.dy);
        replacement_unlock(new_blt_source);
    }
    constexpr std::uint32_t qhd_scroll_width = 2560;
    constexpr std::uint32_t qhd_scroll_height = 1440;
    void* new_qhd_scroll = replacement_create_memory(static_cast<std::uint16_t>(qhd_scroll_width),
        static_cast<std::uint16_t>(qhd_scroll_height), 0);
    if (!new_qhd_scroll) return 167;
    LockedTexture qhd_scroll_pixels = {};
    if (!replacement_lock(new_qhd_scroll, &qhd_scroll_pixels, 2) ||
        qhd_scroll_pixels.width != qhd_scroll_width || qhd_scroll_pixels.height != qhd_scroll_height ||
        qhd_scroll_pixels.pitch < qhd_scroll_width * 4u) return 168;
    std::vector<std::uint32_t> qhd_scroll_expected(static_cast<std::size_t>(qhd_scroll_width) * qhd_scroll_height);
    for (std::uint32_t y = 0; y < qhd_scroll_height; ++y) {
        auto* row = qhd_scroll_pixels.pixels + static_cast<std::size_t>(y) * qhd_scroll_pixels.pitch;
        for (std::uint32_t x = 0; x < qhd_scroll_width; ++x) {
            const std::uint32_t pixel = 0xff000000u | ((x & 0xffu) << 16) | ((y & 0xffu) << 8) | ((x ^ y) & 0xffu);
            std::memcpy(row + static_cast<std::size_t>(x) * 4, &pixel, 4);
            qhd_scroll_expected[static_cast<std::size_t>(y) * qhd_scroll_width + x] = pixel;
        }
    }
    replacement_unlock(new_qhd_scroll);
    replacement_source(new_qhd_scroll);
    replacement_target(new_qhd_scroll);
    const std::array<BlitCase, 4> qhd_scroll_steps{{
        {0, 42, 0, 174, 2276, 1266, 4}, {0, 42, 0, 174, 2276, 1266, 4},
        {0, 174, 0, 42, 2276, 1266, 4}, {0, 174, 0, 42, 2276, 1266, 4}
    }};
    for (std::size_t step = 0; step < qhd_scroll_steps.size(); ++step) {
        const auto& item = qhd_scroll_steps[step];
        const auto before = qhd_scroll_expected;
        for (int y = 0; y < item.height; ++y) {
            const auto* source_row = before.data() + static_cast<std::size_t>(item.sy + y) * qhd_scroll_width + item.sx;
            auto* target_row = qhd_scroll_expected.data() + static_cast<std::size_t>(item.dy + y) * qhd_scroll_width + item.dx;
            std::memcpy(target_row, source_row, static_cast<std::size_t>(item.width) * sizeof(std::uint32_t));
        }
        new_blt(item.sx, item.sy, item.dx, item.dy, item.width, item.height, item.flags);
        if (!replacement_lock(new_qhd_scroll, &qhd_scroll_pixels, 1)) return 169;
        for (std::uint32_t y = 0; y < qhd_scroll_height; ++y) {
            const auto* expected = qhd_scroll_expected.data() + static_cast<std::size_t>(y) * qhd_scroll_width;
            const auto* actual = qhd_scroll_pixels.pixels + static_cast<std::size_t>(y) * qhd_scroll_pixels.pitch;
            if (std::memcmp(expected, actual, static_cast<std::size_t>(qhd_scroll_width) * sizeof(std::uint32_t))) {
                for (std::uint32_t x = 0; x < qhd_scroll_width; ++x) {
                    std::uint32_t actual_pixel = 0;
                    std::memcpy(&actual_pixel, actual + static_cast<std::size_t>(x) * 4, 4);
                    if (expected[x] != actual_pixel) {
                        std::fprintf(stderr,
                            "QHD scroll self-copy mismatch after step %zu at (%u,%u): expected=%08x actual=%08x.\n",
                            step + 1, x, y, expected[x], actual_pixel);
                        return 170;
                    }
                }
            }
        }
        replacement_unlock(new_qhd_scroll);
    }
    replacement_source(nullptr);
    replacement_target(nullptr);
    replacement_free(new_qhd_scroll);
    std::puts("QHD scroll self-copies match the expected pixels at 2560x1440.");
    std::fflush(stdout);
    const auto old_blt_rc = resolve<void (__cdecl*)(std::uint32_t)>(library, "sgl_Blt_RC");
    const auto new_blt_rc = resolve<void (__cdecl*)(std::uint32_t)>(replacement, "sgl_Blt_RC");
    if (!old_blt_rc || !new_blt_rc) return 124;
    set_source(old_blt_source);
    set_target(old_blt_target);
    replacement_source(new_blt_source);
    replacement_target(new_blt_target);
    old_region = old_create_region();
    new_region = new_create_region();
    if (!old_region || !new_region) return 125;
    old_prior_region = old_set_region(old_region);
    new_prior_region = new_set_region(new_region);
    const RECT blit_region{1, 1, 7, 6};
    old_add(&blit_region);
    new_add(&blit_region);
    clip_mode(2);
    replacement_clip_mode(2);
    if (!lock(old_blt_target, &old_blt_pixels, 2) || !replacement_lock(new_blt_target, &new_blt_pixels, 2)) return 126;
    for (std::uint32_t y = 0; y < 12; ++y) {
        std::memcpy(old_blt_pixels.pixels + y * old_blt_pixels.pitch, initial.data() + y * 12, 48);
        std::memcpy(new_blt_pixels.pixels + y * new_blt_pixels.pitch, initial.data() + y * 12, 48);
    }
    unlock(old_blt_target);
    replacement_unlock(new_blt_target);
    old_blt_rc(0);
    new_blt_rc(0);
    if (!lock(old_blt_target, &old_blt_pixels, 1) || !replacement_lock(new_blt_target, &new_blt_pixels, 1)) return 127;
    for (std::uint32_t y = 0; y < 12; ++y) {
        if (std::memcmp(old_blt_pixels.pixels + y * old_blt_pixels.pitch, new_blt_pixels.pixels + y * new_blt_pixels.pitch, 48)) {
            std::fprintf(stderr, "Region blit differs on row %u.\n", y);
            return 128;
        }
    }
    unlock(old_blt_target);
    replacement_unlock(new_blt_target);
    old_reset();
    new_reset();
    const std::array<RECT, 3> disjoint_regions{{{1, 1, 3, 3}, {5, 2, 8, 4}, {2, 6, 4, 8}}};
    for (const auto& region : disjoint_regions) {
        old_add(&region);
        new_add(&region);
    }
    for (std::uint32_t y = 0; y < 12; ++y) {
        for (std::uint32_t x = 0; x < 12; ++x) {
            const RECT pixel{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + 1), static_cast<LONG>(y + 1)};
            bool expected = false;
            for (const auto& region : disjoint_regions) {
                expected |= x >= static_cast<std::uint32_t>(region.left) && x < static_cast<std::uint32_t>(region.right) &&
                    y >= static_cast<std::uint32_t>(region.top) && y < static_cast<std::uint32_t>(region.bottom);
            }
            if ((old_visible(&pixel) != 0) != expected || (new_visible(&pixel) != 0) != expected) return 163;
        }
    }
    if (!lock(old_blt_target, &old_blt_pixels, 2) || !replacement_lock(new_blt_target, &new_blt_pixels, 2)) return 160;
    for (std::uint32_t y = 0; y < 12; ++y) {
        std::memcpy(old_blt_pixels.pixels + y * old_blt_pixels.pitch, initial.data() + y * 12, 48);
        std::memcpy(new_blt_pixels.pixels + y * new_blt_pixels.pitch, initial.data() + y * 12, 48);
    }
    unlock(old_blt_target);
    replacement_unlock(new_blt_target);
    new_blt_rc(0);
    if (!lock(old_blt_target, &old_blt_pixels, 1) || !replacement_lock(new_blt_target, &new_blt_pixels, 1)) return 161;
    const auto clipped_by_union = [&](std::uint32_t x, std::uint32_t y) {
        for (const auto& region : disjoint_regions) {
            if (x >= static_cast<std::uint32_t>(region.left) && x < static_cast<std::uint32_t>(region.right) &&
                y >= static_cast<std::uint32_t>(region.top) && y < static_cast<std::uint32_t>(region.bottom)) return true;
        }
        return false;
    };
    for (std::uint32_t y = 0; y < 12; ++y) {
        for (std::uint32_t x = 0; x < 12; ++x) {
            std::uint32_t actual = 0;
            std::memcpy(&actual, new_blt_pixels.pixels + y * new_blt_pixels.pitch + x * 4, 4);
            const std::uint32_t expected = clipped_by_union(x, y)
                ? (x < 8 && y < 8 ? (x * 31u << 24) | (y * 29u << 16) | (x * 17u << 8) | y : initial[y * 12 + x])
                : initial[y * 12 + x];
            if (actual != expected) {
                std::fprintf(stderr, "Multi-region blit differs at (%u,%u): expected=%08x replacement=%08x.\n",
                    x, y, expected, actual);
                return 162;
            }
        }
    }
    unlock(old_blt_target);
    replacement_unlock(new_blt_target);
    std::puts("Single-region blit matches the original; multi-region blit covers the complete clipper union.");
    old_set_region(old_prior_region);
    new_set_region(new_prior_region);
    old_free_region(old_region);
    new_free_region(new_region);
    clip_mode(1);
    replacement_clip_mode(1);
    set_source(nullptr);
    set_target(nullptr);
    replacement_source(nullptr);
    replacement_target(nullptr);
    free_image(old_blt_source);
    free_image(old_blt_target);
    replacement_free(new_blt_source);
    replacement_free(new_blt_target);
    std::puts("BGRA blits: original and replacement match in four cases.");
    container = create_container(0);
    if (!container || !append_archive(container, "p2arch0_eng.cpr")) return 88;
    reference_archive(container);
    replacement_archive(container);
    void* old_menu = load_image("images/options/Hauptmenu.jpg", 1);
    void* new_menu = replacement_load("images/options/Hauptmenu.jpg", 1);
    void* old_menu_target = create_memory(800, 600, 0);
    void* new_menu_target = replacement_create_memory(800, 600, 0);
    if (!old_menu || !new_menu || !old_menu_target || !new_menu_target) return 89;
    set_source(old_menu);
    set_target(old_menu_target);
    replacement_source(new_menu);
    replacement_target(new_menu_target);
    clip_rect(0, 0, 800, 600);
    replacement_clip_rect(0, 0, 800, 600);
    draw(0, 0, 0, 0, 800, 600);
    replacement_draw(0, 0, 0, 0, 800, 600);
    LockedTexture old_menu_pixels = {}, new_menu_pixels = {};
    if (!lock(old_menu_target, &old_menu_pixels, 1) || !replacement_lock(new_menu_target, &new_menu_pixels, 1)) return 90;
    for (std::uint32_t y = 0; y < 600; ++y) {
        if (std::memcmp(old_menu_pixels.pixels + y * old_menu_pixels.pitch,
            new_menu_pixels.pixels + y * new_menu_pixels.pitch, 800 * 4)) {
            std::fprintf(stderr, "Original menu image draw differs on row %u.\n", y);
            return 91;
        }
    }
    unlock(old_menu_target);
    replacement_unlock(new_menu_target);
    set_key(0);
    replacement_key(0);
    void* old_cursor = load_image("images/feder0003.tga", 7);
    void* new_cursor = replacement_load("images/feder0003.tga", 7);
    if (!old_cursor || !new_cursor) return 92;
    set_source(old_cursor);
    replacement_source(new_cursor);
    draw(0, 0, 100, 100, 40, 40);
    replacement_draw(0, 0, 100, 100, 40, 40);
    if (!lock(old_menu_target, &old_menu_pixels, 1) || !replacement_lock(new_menu_target, &new_menu_pixels, 1)) return 93;
    for (std::uint32_t y = 0; y < 600; ++y) {
        if (std::memcmp(old_menu_pixels.pixels + y * old_menu_pixels.pitch,
            new_menu_pixels.pixels + y * new_menu_pixels.pitch, 800 * 4)) {
            std::fprintf(stderr, "Original cursor draw differs on row %u.\n", y);
            return 94;
        }
    }
    unlock(old_menu_target);
    replacement_unlock(new_menu_target);
    set_source(nullptr);
    set_target(nullptr);
    replacement_source(nullptr);
    replacement_target(nullptr);
    free_image(old_menu);
    free_image(old_cursor);
    free_image(old_menu_target);
    replacement_free(new_menu);
    replacement_free(new_cursor);
    replacement_free(new_menu_target);
    reference_archive(nullptr);
    replacement_archive(nullptr);
    release_container(container);
    std::puts("Original menu image and keyed cursor: drawing matches byte-for-byte.");

    void* alpha_source = create_memory(8, 8, 3);
    void* alpha_target = create_memory(12, 12, 0);
    if (!alpha_source || !alpha_target) return 97;
    LockedTexture alpha_pixels = {};
    if (!lock(alpha_source, &alpha_pixels, 2)) return 98;
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::uint32_t pixel = (x + y) % 3 == 0 ? key :
                ((x * 37u) << 24) | (x * 29u << 16) | (y * 31u << 8) | (x * 3u + y);
            std::memcpy(alpha_pixels.pixels + y * alpha_pixels.pitch + x * 4, &pixel, 4);
        }
    }
    unlock(alpha_source);
    if (!lock(alpha_target, &alpha_pixels, 2)) return 99;
    for (std::uint32_t y = 0; y < 12; ++y) std::memcpy(alpha_pixels.pixels + y * alpha_pixels.pitch, initial.data() + y * 12, 48);
    unlock(alpha_target);
    set_source(alpha_source);
    set_target(alpha_target);
    clip_mode(1);
    clip_rect(0, 0, 12, 12);
    *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(library) + 0x8d080) = 0xffffffff;
    draw(1, 1, 3, 4, 5, 4);
    if (!lock(alpha_target, &alpha_pixels, 1)) return 100;
    std::array<std::uint32_t, 5> alpha_row = {};
    std::memcpy(alpha_row.data(), alpha_pixels.pixels + 4 * alpha_pixels.pitch + 3 * 4, sizeof(alpha_row));
    unlock(alpha_target);
    set_source(nullptr);
    set_target(nullptr);
    free_image(alpha_source);
    free_image(alpha_target);
    if (alpha_row != std::array<std::uint32_t, 5>{0xe0213e58u, 0xff224466u, 0xc038333eu, 0xc0502e32u, 0xff224466u}) return 101;
    std::puts("Original alpha fixture: observed pixels match recorded reference.");

    const auto wide_source_width = static_cast<std::uint16_t>(stretch_fixture.source_width);
    const auto wide_source_height = static_cast<std::uint16_t>(stretch_fixture.source_height);
    const auto wide_target_width = static_cast<std::uint16_t>(stretch_fixture.target_width);
    const auto wide_target_height = static_cast<std::uint16_t>(stretch_fixture.target_height);
    std::vector<char> captured_stretch_source;
    std::vector<char> captured_intermediate_surface;
    std::vector<char> captured_target_before;
    std::vector<RECT> captured_clip_rectangles;
    if (argc >= 4) {
        std::ifstream stretch_input(argv[3], std::ios::binary);
        if (!stretch_input) {
            std::fwprintf(stderr, L"Cannot read captured stretch source: %ls\n", argv[3]);
            return 108;
        }
        captured_stretch_source.assign(std::istreambuf_iterator<char>(stretch_input), std::istreambuf_iterator<char>());
        const std::size_t expected_source_bytes = static_cast<std::size_t>(wide_source_width) * wide_source_height * 4u;
        if (captured_stretch_source.size() != expected_source_bytes) {
            std::fprintf(stderr, "Captured stretch source has %zu bytes; expected %zu for %ux%u BGRA32.\n",
                captured_stretch_source.size(), expected_source_bytes, wide_source_width, wide_source_height);
            return 109;
        }
    }
    if (argc == 5) {
        std::ifstream intermediate_input(argv[4], std::ios::binary);
        if (!intermediate_input) {
            std::fwprintf(stderr, L"Cannot read captured intermediate surface: %ls\n", argv[4]);
            return 115;
        }
        captured_intermediate_surface.assign(std::istreambuf_iterator<char>(intermediate_input), std::istreambuf_iterator<char>());
        const std::size_t expected_intermediate_bytes = static_cast<std::size_t>(wide_target_width) * wide_target_height * 4u;
        if (captured_intermediate_surface.size() != expected_intermediate_bytes) {
            std::fprintf(stderr, "Captured intermediate surface has %zu bytes; expected %zu for %ux%u BGRA32.\n",
                captured_intermediate_surface.size(), expected_intermediate_bytes, wide_target_width, wide_target_height);
            return 116;
        }
    }
    if (argc >= 6) {
        std::ifstream destination_input(argv[4], std::ios::binary);
        if (!destination_input) {
            std::fwprintf(stderr, L"Cannot read captured destination-before surface: %ls\n", argv[4]);
            return 127;
        }
        captured_target_before.assign(std::istreambuf_iterator<char>(destination_input), std::istreambuf_iterator<char>());
        const std::size_t expected_target_bytes = static_cast<std::size_t>(wide_target_width) * wide_target_height * 4u;
        if (captured_target_before.size() != expected_target_bytes) {
            std::fprintf(stderr, "Captured destination-before surface has %zu bytes; expected %zu for %ux%u BGRA32.\n",
                captured_target_before.size(), expected_target_bytes, wide_target_width, wide_target_height);
            return 128;
        }
        std::ifstream rectangles_input(argv[5]);
        if (!rectangles_input) {
            std::fwprintf(stderr, L"Cannot read captured clip rectangles: %ls\n", argv[5]);
            return 129;
        }
        std::string line;
        while (std::getline(rectangles_input, line)) {
            if (line.empty()) continue;
            std::istringstream values(line);
            RECT parsed_rectangle{};
            std::string extra;
            if (!(values >> parsed_rectangle.left >> parsed_rectangle.top >> parsed_rectangle.right >> parsed_rectangle.bottom) || values >> extra ||
                parsed_rectangle.left < 0 || parsed_rectangle.top < 0 || parsed_rectangle.right <= parsed_rectangle.left || parsed_rectangle.bottom <= parsed_rectangle.top ||
                parsed_rectangle.right > wide_target_width || parsed_rectangle.bottom > wide_target_height) {
                std::fprintf(stderr, "Invalid captured clip rectangle: %s\n", line.c_str());
                return 130;
            }
            captured_clip_rectangles.push_back(parsed_rectangle);
        }
        if (captured_clip_rectangles.empty()) {
            std::fputs("Captured clip rectangle file is empty.\n", stderr);
            return 131;
        }
    }
    void* wide_source = create_memory(wide_source_width, wide_source_height, stretch_fixture.source_flags);
    void* wide_target = create_memory(wide_target_width, wide_target_height, stretch_fixture.target_flags);
    void* new_wide_source = replacement_create_memory(wide_source_width, wide_source_height, stretch_fixture.source_flags);
    void* new_wide_target = replacement_create_memory(wide_target_width, wide_target_height, stretch_fixture.target_flags);
    if (!wide_source || !wide_target || !new_wide_source || !new_wide_target) return 102;

    LockedTexture wide_source_pixels = {}, new_wide_source_pixels = {};
    if (!lock(wide_source, &wide_source_pixels, 2) || !replacement_lock(new_wide_source, &new_wide_source_pixels, 2) ||
        wide_source_pixels.width != wide_source_width || wide_source_pixels.height != wide_source_height ||
        new_wide_source_pixels.width != wide_source_width || new_wide_source_pixels.height != wide_source_height ||
        wide_source_pixels.pitch < wide_source_width * 4u || new_wide_source_pixels.pitch < wide_source_width * 4u) return 103;
    for (std::uint32_t y = 0; y < wide_source_height; ++y) {
        if (!captured_stretch_source.empty()) {
            const auto* row = captured_stretch_source.data() + y * wide_source_width * 4u;
            std::memcpy(wide_source_pixels.pixels + y * wide_source_pixels.pitch, row, wide_source_width * 4u);
            std::memcpy(new_wide_source_pixels.pixels + y * new_wide_source_pixels.pitch, row, wide_source_width * 4u);
            continue;
        }
        for (std::uint32_t x = 0; x < wide_source_width; ++x) {
            const std::uint32_t pixel = 0xff000000u | (((x * 17u + y * 3u) & 0xffu) << 16) |
                (((x * 5u + y * 29u) & 0xffu) << 8) | ((x * 11u + y * 7u) & 0xffu);
            std::memcpy(wide_source_pixels.pixels + y * wide_source_pixels.pitch + x * 4u, &pixel, sizeof(pixel));
            std::memcpy(new_wide_source_pixels.pixels + y * new_wide_source_pixels.pitch + x * 4u, &pixel, sizeof(pixel));
        }
    }
    unlock(wide_source);
    replacement_unlock(new_wide_source);

    LockedTexture wide_target_pixels = {}, new_wide_target_pixels = {};
    if (!lock(wide_target, &wide_target_pixels, 2) || !replacement_lock(new_wide_target, &new_wide_target_pixels, 2) ||
        wide_target_pixels.width != wide_target_width || wide_target_pixels.height != wide_target_height ||
        new_wide_target_pixels.width != wide_target_width || new_wide_target_pixels.height != wide_target_height ||
        wide_target_pixels.pitch < wide_target_width * 4u || new_wide_target_pixels.pitch < wide_target_width * 4u) return 104;
    constexpr std::uint32_t initial_wide_pixel = 0xff3a5c7e;
    for (std::uint32_t y = 0; y < wide_target_height; ++y) {
        if (!captured_target_before.empty()) {
            const auto* row = captured_target_before.data() + y * wide_target_width * 4u;
            std::memcpy(wide_target_pixels.pixels + y * wide_target_pixels.pitch, row, wide_target_width * 4u);
            std::memcpy(new_wide_target_pixels.pixels + y * new_wide_target_pixels.pitch, row, wide_target_width * 4u);
        } else {
            for (std::uint32_t x = 0; x < wide_target_width; ++x) {
                std::memcpy(wide_target_pixels.pixels + y * wide_target_pixels.pitch + x * 4u,
                    &initial_wide_pixel, sizeof(initial_wide_pixel));
                std::memcpy(new_wide_target_pixels.pixels + y * new_wide_target_pixels.pitch + x * 4u,
                    &initial_wide_pixel, sizeof(initial_wide_pixel));
            }
        }
    }
    unlock(wide_target);
    replacement_unlock(new_wide_target);

    auto* wide_old_region = old_create_region();
    auto* wide_new_region = new_create_region();
    if (!wide_old_region || !wide_new_region) return 105;
    const RECT wide_clip = stretch_fixture.draw.clip;
    auto* wide_old_prior_region = old_set_region(wide_old_region);
    auto* wide_new_prior_region = new_set_region(wide_new_region);
    if (captured_clip_rectangles.empty()) {
        old_add(&wide_clip);
        new_add(&wide_clip);
    } else {
        for (const RECT& captured_rectangle : captured_clip_rectangles) {
            old_add(&captured_rectangle);
            new_add(&captured_rectangle);
        }
    }
    clip_mode(stretch_fixture.clip_mode);
    replacement_clip_mode(stretch_fixture.clip_mode);
    clip_rect(wide_clip.left, wide_clip.top, wide_clip.right, wide_clip.bottom);
    replacement_clip_rect(wide_clip.left, wide_clip.top, wide_clip.right, wide_clip.bottom);
    old_set_color(stretch_fixture.color);
    replacement_constant(stretch_fixture.color);
    set_source(wide_source);
    replacement_source(new_wide_source);
    set_target(wide_target);
    replacement_target(new_wide_target);
    const auto& captured_draw = stretch_fixture.draw;
    stretch(captured_draw.sx, captured_draw.sy, captured_draw.dx, captured_draw.dy,
        captured_draw.width, captured_draw.height, captured_draw.destination_width, captured_draw.destination_height);
    replacement_stretch(captured_draw.sx, captured_draw.sy, captured_draw.dx, captured_draw.dy,
        captured_draw.width, captured_draw.height, captured_draw.destination_width, captured_draw.destination_height);
    set_target(nullptr);
    replacement_target(nullptr);

    if (!lock(wide_target, &wide_target_pixels, 1) || !replacement_lock(new_wide_target, &new_wide_target_pixels, 1)) return 106;
    bool wide_stretch_matches = true;
    bool captured_stretch_matches = true;
    std::size_t original_capture_mismatches = 0;
    std::size_t replacement_capture_mismatches = 0;
    std::size_t original_changed_pixels = 0;
    std::size_t replacement_changed_pixels = 0;
    std::uint32_t capture_mismatch_x = 0, capture_mismatch_y = 0;
    std::uint32_t capture_expected_pixel = 0, capture_original_pixel = 0, capture_replacement_pixel = 0;
    std::uint32_t mismatch_x = 0, mismatch_y = 0, original_pixel = 0, replacement_pixel = 0;
    for (std::uint32_t y = 0; y < wide_target_height && wide_stretch_matches; ++y) {
        for (std::uint32_t x = 0; x < wide_target_width; ++x) {
            std::memcpy(&original_pixel, wide_target_pixels.pixels + y * wide_target_pixels.pitch + x * 4u, sizeof(original_pixel));
            std::memcpy(&replacement_pixel, new_wide_target_pixels.pixels + y * new_wide_target_pixels.pitch + x * 4u, sizeof(replacement_pixel));
            if (!captured_target_before.empty()) {
                std::uint32_t before_pixel = 0;
                std::memcpy(&before_pixel, captured_target_before.data() + y * wide_target_width * 4u + x * 4u,
                    sizeof(before_pixel));
                original_changed_pixels += original_pixel != before_pixel;
                replacement_changed_pixels += replacement_pixel != before_pixel;
            }
            if (!captured_intermediate_surface.empty()) {
                std::uint32_t captured_pixel = 0;
                std::memcpy(&captured_pixel, captured_intermediate_surface.data() + y * wide_target_width * 4u + x * 4u,
                    sizeof(captured_pixel));
                if (original_pixel != captured_pixel) {
                    if (original_capture_mismatches == 0) {
                        capture_mismatch_x = x;
                        capture_mismatch_y = y;
                        capture_expected_pixel = captured_pixel;
                        capture_original_pixel = original_pixel;
                    }
                    ++original_capture_mismatches;
                }
                if (replacement_pixel != captured_pixel) {
                    if (replacement_capture_mismatches == 0) capture_replacement_pixel = replacement_pixel;
                    ++replacement_capture_mismatches;
                }
            }
            if (original_pixel != replacement_pixel) {
                mismatch_x = x;
                mismatch_y = y;
                wide_stretch_matches = false;
                break;
            }
        }
    }
    unlock(wide_target);
    replacement_unlock(new_wide_target);
    if (!captured_intermediate_surface.empty()) {
        captured_stretch_matches = original_capture_mismatches == 0 && replacement_capture_mismatches == 0;
        std::printf("Captured intermediate comparison: original mismatches=%zu, replacement mismatches=%zu; first at (%u,%u) expected=%08x original=%08x replacement=%08x.\n",
            original_capture_mismatches, replacement_capture_mismatches, capture_mismatch_x, capture_mismatch_y,
            capture_expected_pixel, capture_original_pixel, capture_replacement_pixel);
    }
    if (!captured_target_before.empty()) {
        std::printf("Captured destination changes: original=%zu pixels, replacement=%zu pixels.\n",
            original_changed_pixels, replacement_changed_pixels);
    }
    bool wide_blit_matches = false;
    if (wide_stretch_matches) {
        const auto screen_width = static_cast<std::uint16_t>(stretch_fixture.screen_width);
        const auto screen_height = static_cast<std::uint16_t>(stretch_fixture.screen_height);
        constexpr std::uint32_t initial_screen_pixel = 0xff557799u;
        void* old_screen = create_memory(screen_width, screen_height, 0);
        void* new_screen = replacement_create_memory(screen_width, screen_height, 0);
        if (!old_screen || !new_screen) return 110;
        if (!captured_intermediate_surface.empty()) {
            if (!lock(wide_target, &wide_target_pixels, 2) || !replacement_lock(new_wide_target, &new_wide_target_pixels, 2) ||
                wide_target_pixels.width != wide_target_width || wide_target_pixels.height != wide_target_height ||
                new_wide_target_pixels.width != wide_target_width || new_wide_target_pixels.height != wide_target_height ||
                wide_target_pixels.pitch < wide_target_width * 4u || new_wide_target_pixels.pitch < wide_target_width * 4u) return 117;
            for (std::uint32_t y = 0; y < wide_target_height; ++y) {
                const auto* row = captured_intermediate_surface.data() + y * wide_target_width * 4u;
                std::memcpy(wide_target_pixels.pixels + y * wide_target_pixels.pitch, row, wide_target_width * 4u);
                std::memcpy(new_wide_target_pixels.pixels + y * new_wide_target_pixels.pitch, row, wide_target_width * 4u);
            }
            unlock(wide_target);
            replacement_unlock(new_wide_target);
        }
        LockedTexture old_screen_pixels = {}, new_screen_pixels = {};
        if (!lock(old_screen, &old_screen_pixels, 2) || !replacement_lock(new_screen, &new_screen_pixels, 2) ||
            old_screen_pixels.width != screen_width || old_screen_pixels.height != screen_height ||
            new_screen_pixels.width != screen_width || new_screen_pixels.height != screen_height ||
            old_screen_pixels.pitch < screen_width * 4u || new_screen_pixels.pitch < screen_width * 4u) return 111;
        for (std::uint32_t y = 0; y < screen_height; ++y) {
            for (std::uint32_t x = 0; x < screen_width; ++x) {
                std::memcpy(old_screen_pixels.pixels + y * old_screen_pixels.pitch + x * 4u,
                    &initial_screen_pixel, sizeof(initial_screen_pixel));
                std::memcpy(new_screen_pixels.pixels + y * new_screen_pixels.pitch + x * 4u,
                    &initial_screen_pixel, sizeof(initial_screen_pixel));
            }
        }
        unlock(old_screen);
        replacement_unlock(new_screen);

        auto* old_screen_region = old_create_region();
        auto* new_screen_region = new_create_region();
        if (!old_screen_region || !new_screen_region) return 112;
        const RECT screen_clip{0, 0, screen_width, screen_height};
        old_add(&screen_clip);
        new_add(&screen_clip);
        auto* old_prior_screen_region = old_set_region(old_screen_region);
        auto* new_prior_screen_region = new_set_region(new_screen_region);
        clip_mode(3);
        replacement_clip_mode(3);
        clip_rect(screen_clip.left, screen_clip.top, screen_clip.right, screen_clip.bottom);
        replacement_clip_rect(screen_clip.left, screen_clip.top, screen_clip.right, screen_clip.bottom);
        old_set_color(0xffffffffu);
        replacement_constant(0xffffffffu);
        old_blend(1);
        new_blend(1);
        set_source(wide_target);
        replacement_source(new_wide_target);
        set_target(old_screen);
        replacement_target(new_screen);
        draw(0, 0, stretch_fixture.screen_x, stretch_fixture.screen_y, wide_target_width, wide_target_height);
        replacement_draw(0, 0, stretch_fixture.screen_x, stretch_fixture.screen_y, wide_target_width, wide_target_height);
        set_target(nullptr);
        replacement_target(nullptr);

        if (!lock(old_screen, &old_screen_pixels, 1) || !replacement_lock(new_screen, &new_screen_pixels, 1)) return 113;
        wide_blit_matches = true;
        for (std::uint32_t y = 0; y < screen_height && wide_blit_matches; ++y) {
            for (std::uint32_t x = 0; x < screen_width; ++x) {
                std::memcpy(&original_pixel, old_screen_pixels.pixels + y * old_screen_pixels.pitch + x * 4u, sizeof(original_pixel));
                std::memcpy(&replacement_pixel, new_screen_pixels.pixels + y * new_screen_pixels.pitch + x * 4u, sizeof(replacement_pixel));
                if (original_pixel != replacement_pixel) {
                    mismatch_x = x;
                    mismatch_y = y;
                    wide_blit_matches = false;
                    break;
                }
            }
        }
        unlock(old_screen);
        replacement_unlock(new_screen);
        set_source(nullptr);
        replacement_source(nullptr);
        old_set_region(old_prior_screen_region);
        new_set_region(new_prior_screen_region);
        old_free_region(old_screen_region);
        new_free_region(new_screen_region);
        free_image(old_screen);
        replacement_free(new_screen);
    }
    set_source(nullptr);
    replacement_source(nullptr);
    old_set_region(wide_old_prior_region);
    new_set_region(wide_new_prior_region);
    old_free_region(wide_old_region);
    new_free_region(wide_new_region);
    free_image(wide_source);
    free_image(wide_target);
    replacement_free(new_wide_source);
    replacement_free(new_wide_target);
    if (!wide_stretch_matches) {
        std::fprintf(stderr, "Large clipped stretch differs at (%u,%u): original=%08x replacement=%08x.\n",
            mismatch_x, mismatch_y, original_pixel, replacement_pixel);
        return 107;
    }
    if (!captured_stretch_matches) {
        std::fprintf(stderr, "Captured 996x982 stretch result differs from reference SGL at (%u,%u).\n",
            capture_mismatch_x, capture_mismatch_y);
        return 118;
    }
    if (!wide_blit_matches) {
        std::fprintf(stderr, "Captured intermediate-surface blit differs at (%u,%u): original=%08x replacement=%08x.\n",
            mismatch_x, mismatch_y, original_pixel, replacement_pixel);
        return 114;
    }

    const auto original_draw_line = resolve<void (__cdecl*)(float, float, float, float)>(library, "sgl_DrawLine");
    const auto replacement_draw_line = resolve<void (__cdecl*)(float, float, float, float)>(replacement, "sgl_DrawLine");
    if (!original_draw_line || !replacement_draw_line) return 120;
    constexpr std::uint32_t line_background = 0xff29435du;
    constexpr std::uint32_t line_color = 0xffc0712bu;
    void* old_line_target = create_memory(32, 24, 0);
    void* new_line_target = replacement_create_memory(32, 24, 0);
    if (!old_line_target || !new_line_target) return 121;
    LockedTexture old_line_pixels = {}, new_line_pixels = {};
    if (!lock(old_line_target, &old_line_pixels, 2) || !replacement_lock(new_line_target, &new_line_pixels, 2) ||
        old_line_pixels.width != 32 || old_line_pixels.height != 24 ||
        new_line_pixels.width != 32 || new_line_pixels.height != 24 ||
        old_line_pixels.pitch < 32 * 4u || new_line_pixels.pitch < 32 * 4u) return 122;
    for (std::uint32_t y = 0; y < 24; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            std::memcpy(old_line_pixels.pixels + y * old_line_pixels.pitch + x * 4u, &line_background, 4);
            std::memcpy(new_line_pixels.pixels + y * new_line_pixels.pitch + x * 4u, &line_background, 4);
        }
    }
    unlock(old_line_target);
    replacement_unlock(new_line_target);
    set_target(old_line_target);
    replacement_target(new_line_target);
    clip_mode(1);
    replacement_clip_mode(1);
    clip_rect(2, 2, 30, 22);
    replacement_clip_rect(2, 2, 30, 22);
    old_set_color(line_color);
    replacement_constant(line_color);
    for (const auto& line : std::array<std::array<int, 4>, 6>{
            std::array<int, 4>{1, 2, 23, 9}, {23, 10, 1, 3}, {3, 21, 11, 1},
            {-7, 5, 39, 18}, {31, 23, 0, 0}, {16, 12, 16, 12}}) {
        original_draw_line(static_cast<float>(line[0]), static_cast<float>(line[1]),
            static_cast<float>(line[2]), static_cast<float>(line[3]));
        replacement_draw_line(static_cast<float>(line[0]), static_cast<float>(line[1]),
            static_cast<float>(line[2]), static_cast<float>(line[3]));
    }
    if (!lock(old_line_target, &old_line_pixels, 1)) return 125;
    unlock(old_line_target);
    set_target(nullptr);
    replacement_target(nullptr);
    clip_mode(0);
    replacement_clip_mode(0);
    if (!lock(old_line_target, &old_line_pixels, 1) || !replacement_lock(new_line_target, &new_line_pixels, 1)) return 123;
    for (std::uint32_t y = 0; y < 24; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            std::uint32_t old_pixel = 0, new_pixel = 0;
            std::memcpy(&old_pixel, old_line_pixels.pixels + y * old_line_pixels.pitch + x * 4u, 4);
            std::memcpy(&new_pixel, new_line_pixels.pixels + y * new_line_pixels.pitch + x * 4u, 4);
            if (new_pixel != old_pixel) {
                std::fprintf(stderr, "SGL DrawLine differs at (%u,%u): original=%08x replacement=%08x.\n",
                    x, y, old_pixel, new_pixel);
                return 124;
            }
        }
    }
    unlock(old_line_target);
    replacement_unlock(new_line_target);
    free_image(old_line_target);
    replacement_free(new_line_target);
    std::puts("SGL DrawLine: original and replacement match for six clipped line cases.");
    std::printf("%s %ux%u source stretch to %ux%u: original and D3D11 replacement match byte-for-byte.\n",
        captured_stretch_source.empty() ? "Synthetic" : "Captured", wide_source_width, wide_source_height,
        wide_target_width, wide_target_height);
    std::printf("Intermediate surface blit to %dx%d at (%d,%d): original and D3D11 replacement match byte-for-byte.\n",
        stretch_fixture.screen_width, stretch_fixture.screen_height, stretch_fixture.screen_x, stretch_fixture.screen_y);
    return 0;
}
