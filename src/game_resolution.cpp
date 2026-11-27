#include "game_resolution.hpp"
#include "game_executable.hpp"
#include "unimplemented.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {
std::uint32_t game_width = 0, game_height = 0;
std::uint8_t* game_module = nullptr;
// A patched executable lays out every screen size itself; only the frame limiter is shared with it.
bool game_owned_layout = false;
bool accelerated_map_loaded = false;
sgl::DisplaySize accelerated_map_source{1280, 1024};
std::uint8_t* accelerated_map_layout = nullptr;
std::uint8_t* accelerated_map_frame = nullptr;
sgl::DisplaySize accelerated_map_layout_size{};
HMODULE aim_map_module = nullptr;
std::uint8_t* original_aim_map_decoder = nullptr;
bool aim_map_hook_installed = false;
std::array<std::vector<std::uint8_t>, 4> accelerated_map_assets;
void* after_options = nullptr;
void* before_scene = nullptr;
void* after_scene = nullptr;
void* after_sidebar = nullptr;
void* after_top_bar = nullptr;
void* after_options_begin = nullptr;
void* after_resolution_label = nullptr;
void* after_color_depth_load = nullptr;
void* original_fast_forward_layout = nullptr;
std::uint32_t pending_resolution = 0;

constexpr std::array<sgl::DisplaySize, 4> supported_sizes = {{
    {1280, 1024}, {1600, 900}, {1920, 1080}, {2560, 1440}
}};
// Build identities. The GOG, Steam and p3-lib modloader executables are the same build: the Steam release wraps it in
// a DRM section that decrypts .text at startup, and the modloader adds an import section. Their unchanged .rdata
// section and link time identify the build; every patch still compares its own bytes in memory before writing.
// Steam's AIM.dll differs from GOG's only in one header byte, so AIM.dll is identified by its code section.
constexpr std::uint32_t executable_timestamp = sgl::game::patrician3_timestamp;
constexpr std::array<std::uint8_t, 32> executable_rdata_sha256 = {
    0x1f,0x5b,0x5a,0xb4,0xb9,0x9f,0x30,0x44,0x2a,0x84,0x75,0xb9,0x97,0xe9,0x2a,0x2c,
    0x26,0x9a,0x0d,0xcf,0x1a,0xab,0xad,0x16,0x46,0xf8,0x96,0xd7,0xe4,0x2b,0xc6,0x5a};
constexpr std::uint32_t aim_timestamp = 0x39d35554;
constexpr std::array<std::uint8_t, 32> aim_text_sha256 = {
    0xe7,0xa3,0xbc,0x55,0xa3,0x12,0xc3,0x47,0xfc,0xf9,0xaf,0x09,0xa2,0xd6,0xac,0xec,
    0x22,0x7b,0xe0,0x5e,0xff,0x41,0x97,0x18,0x60,0x73,0x06,0xe0,0xca,0xa7,0x4d,0x8f};
constexpr std::uint32_t aim_map_decode_call_rva = 0x2984;
constexpr std::array<std::uint8_t, 5> aim_map_decode_call_original = {0xe8,0x47,0x16,0,0};
constexpr char resolution_1280[] = "1280 x 1024";
constexpr char resolution_1600[] = "1600 x 900";
constexpr char resolution_1920[] = "1920 x 1080";
constexpr char resolution_2560[] = "2560 x 1440";
const char* const resolution_labels[] = {
    resolution_1280, resolution_1600, resolution_1920, resolution_2560
};

std::uint32_t resolution_index(std::uint32_t width, std::uint32_t height) noexcept {
    for (std::uint32_t index = 0; index < supported_sizes.size(); ++index) {
        if (supported_sizes[index].width == width && supported_sizes[index].height == height) return index;
    }
    return 0;
}

void begin_options_resolution() noexcept {
    pending_resolution = resolution_index(game_width, game_height);
}

void discard_options_resolution() noexcept {
    begin_options_resolution();
}

std::uint32_t __fastcall decrement_options_resolution(void* enumeration, void*) noexcept {
    if (pending_resolution) --pending_resolution;
    return enumeration ? *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(enumeration) + 4) : 0;
}

std::uint32_t __fastcall increment_options_resolution(void* enumeration, void*) noexcept {
    if (pending_resolution + 1 < supported_sizes.size()) ++pending_resolution;
    return enumeration ? *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(enumeration) + 4) : 0;
}

std::uint32_t __fastcall keep_32bit_color_depth(void*, void*) noexcept {
    return 1;
}

bool resize_accelerated_map_layout() noexcept {
    if (!game_module || !accelerated_map_loaded) return false;
    auto* const map = *reinterpret_cast<std::uint8_t**>(game_module + 0x2cba64);
    if (!map) return false;

    auto** const rectangles = *reinterpret_cast<std::uint8_t***>(map + 0x164 + 0xa0);
    if (!rectangles || !rectangles[0]) return false;

    auto* const frame = rectangles[0];
    const sgl::DisplaySize size{game_width, game_height};
    const sgl::FastForwardMapLayoutState state{
        *reinterpret_cast<std::int32_t*>(map + 0x14),
        *reinterpret_cast<std::int32_t*>(map + 0x18),
        *reinterpret_cast<std::uint32_t*>(map + 0x2c),
        *reinterpret_cast<std::uint32_t*>(map + 0x30),
        *reinterpret_cast<std::uint32_t*>(frame + 8),
        *reinterpret_cast<std::uint32_t*>(frame + 0xc)
    };
    const auto* clip = reinterpret_cast<const RECT*>(map + 0x4c);
    const bool clip_active = clip->right > clip->left && clip->bottom > clip->top;
    const bool clip_matches = !clip_active || (clip->left == 0 && clip->top == 0 &&
        clip->right == static_cast<LONG>(size.width) && clip->bottom == static_cast<LONG>(size.height));
    if (map == accelerated_map_layout && frame == accelerated_map_frame &&
        size.width == accelerated_map_layout_size.width &&
        size.height == accelerated_map_layout_size.height &&
        clip_matches && sgl::fast_forward_map_layout_matches(state, size, accelerated_map_source)) return true;

    *reinterpret_cast<std::int32_t*>(map + 0x14) = 0;
    *reinterpret_cast<std::int32_t*>(map + 0x18) = 0;
    *reinterpret_cast<std::uint32_t*>(map + 0x2c) = game_width;
    *reinterpret_cast<std::uint32_t*>(map + 0x30) = game_height;
    *reinterpret_cast<std::uint32_t*>(frame + 8) = accelerated_map_source.width;
    *reinterpret_cast<std::uint32_t*>(frame + 0xc) = accelerated_map_source.height;
    if (clip_active && !clip_matches) {
        const RECT bounds{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
        using SetClip = void (__thiscall*)(void*, const RECT*);
        reinterpret_cast<SetClip>(game_module + 0xb1ad0)(map, &bounds);
    }
    accelerated_map_layout = map;
    accelerated_map_frame = frame;
    accelerated_map_layout_size = size;
    return true;
}

void __fastcall place_fast_forward_map_ui_element(void* element, void*, std::uint32_t zero, void* animation) {
    using Original = void (__thiscall*)(void*, std::uint32_t, void*);
    if (!original_fast_forward_layout) sgl::unimplemented("fast-forward map layout continuation");
    resize_accelerated_map_layout();
    reinterpret_cast<Original>(original_fast_forward_layout)(element, zero, animation);
    resize_accelerated_map_layout();
}

void __fastcall load_accelerated_map(void* loader, void*, const char* path) {
    using Original = void (__thiscall*)(void*, const char*);
    reinterpret_cast<Original>(game_module + 0x24f2c1)(loader, path);
    if (path && std::strcmp(path, "./scripts/accelMap.ini") == 0) {
        accelerated_map_loaded = true;
        accelerated_map_layout = nullptr;
        accelerated_map_frame = nullptr;
        accelerated_map_layout_size = {};
        resize_accelerated_map_layout();
    }
}

std::uint32_t __fastcall options_button_hit_test(void* button, void* options) noexcept {
    if (!game_module || !button) return 0;
    if (options) {
        auto* const options_bytes = static_cast<std::uint8_t*>(options);
        const bool is_decrement = button == options_bytes + 0x2a8;
        const bool is_increment = button == options_bytes + 0x390;
        if (is_decrement || is_increment) {
            const auto enabled = static_cast<std::uint8_t>(is_decrement ? pending_resolution != 0 :
                pending_resolution + 1 < supported_sizes.size());
            auto* const enabled_state = static_cast<std::uint8_t*>(button) + 0x41;
            if (*enabled_state != enabled) {
                using SetEnabled = char (__thiscall*)(void*, char);
                reinterpret_cast<SetEnabled>(game_module + 0xb17f0)(button, enabled);
            }
        }
    }

    using Original = std::uint32_t (__thiscall*)(void*);
    return reinterpret_cast<Original>(game_module + 0xc78b0)(button);
}

__declspec(naked) void options_button_hit_test_hook() {
    __asm {
        mov edx, ebp
        jmp options_button_hit_test
    }
}

void __fastcall cancel_options_resolution(void* options, void*) {
    discard_options_resolution();
    using Original = void (__thiscall*)(void*);
    reinterpret_cast<Original>(game_module + 0x60250)(options);
}

void __fastcall apply_options_resolution(void* options, void*) noexcept {
    using Original = void (__thiscall*)(void*);
    try {
        reinterpret_cast<Original>(game_module + 0x5df10)(options);
        std::string error;
        switch (sgl::apply_options_display_resolution(error)) {
        case sgl::DisplayApplyResult::Failed:
            discard_options_resolution();
            MessageBoxA(GetForegroundWindow(), error.empty() ?
                "The selected resolution could not be applied; the current size was kept." : error.c_str(),
                "Patrician III", MB_OK | MB_ICONWARNING);
            break;
        case sgl::DisplayApplyResult::AppliedNotSaved:
            MessageBoxA(GetForegroundWindow(), error.empty() ?
                "The resolution changed for this session, but high_res.ini could not be saved." : error.c_str(),
                "Patrician III", MB_OK | MB_ICONWARNING);
            break;
        case sgl::DisplayApplyResult::Applied:
            break;
        }
    } catch (...) {
        discard_options_resolution();
        MessageBoxA(GetForegroundWindow(),
            "The selected resolution could not be applied; the current size was kept.",
            "Patrician III", MB_OK | MB_ICONWARNING);
    }
}

__declspec(naked) void options_resolution() {
    __asm {
        push eax
        mov eax, game_width
        mov [esp + 50h], eax
        mov eax, game_height
        mov [esp + 54h], eax
        pop eax
        jmp [after_options]
    }
}

__declspec(naked) void scene_resolution_before() {
    __asm {
        push eax
        mov eax, game_width
        mov [esp + 40h], eax
        mov eax, game_height
        mov [esp + 44h], eax
        pop eax
        jmp [before_scene]
    }
}

__declspec(naked) void scene_resolution_after() {
    __asm {
        push eax
        mov eax, game_width
        mov [esp + 28h], eax
        mov eax, game_height
        mov [esp + 2ch], eax
        pop eax
        jmp [after_scene]
    }
}

std::uint32_t __cdecl layout_width() {
    return std::min(*reinterpret_cast<std::uint32_t*>(game_module + 0x2dcce0), 1280u);
}

std::uint32_t __cdecl position_sidebar() {
    const auto width = *reinterpret_cast<std::uint32_t*>(game_module + 0x2dcce0);
    if (width >= 1280) {
        auto* ui = *reinterpret_cast<std::uint8_t**>(game_module + 0x2cbb40);
        if (!ui) sgl::unimplemented("game resolution: missing city layout");
        auto** rectangles = *reinterpret_cast<std::uint8_t***>(ui + 0xa90 + 0xa0);
        if (!rectangles || !rectangles[0]) sgl::unimplemented("game resolution: missing city frame");
        *reinterpret_cast<std::uint32_t*>(rectangles[0] + 8) = width - 284;
        *reinterpret_cast<std::uint32_t*>(ui + 0xdf0 + 0x20) = width - 284;
        auto** sidebar_rectangles = *reinterpret_cast<std::uint8_t***>(ui + 0xdf0 + 0xa0);
        if (!sidebar_rectangles || !sidebar_rectangles[0])
            sgl::unimplemented("game resolution: missing sidebar frame");
        *reinterpret_cast<std::uint32_t*>(sidebar_rectangles[0] + 12) = game_height - 600;
    }
    return layout_width();
}

__declspec(naked) void sidebar_layout() {
    __asm {
        push ecx
        push edx
        call position_sidebar
        pop edx
        pop ecx
        jmp [after_sidebar]
    }
}

__declspec(naked) void top_bar_layout() {
    __asm {
        push ecx
        push edx
        call layout_width
        pop edx
        pop ecx
        jmp [after_top_bar]
    }
}

__declspec(naked) void begin_options_resolution_hook() {
    __asm {
        pushfd
        pushad
        call begin_options_resolution
        popad
        popfd
        mov [ebp + 1b8h], ecx
        jmp [after_options_begin]
    }
}

__declspec(naked) void options_resolution_label_hook() {
    __asm {
        push eax
        mov eax, pending_resolution
        mov edx, dword ptr [resolution_labels + eax * 4]
        pop eax
        jmp [after_resolution_label]
    }
}

__declspec(naked) void force_32bit_color_depth_on_load() {
    __asm {
        mov dword ptr [esi + 164h], 1
        jmp [after_color_depth_load]
    }
}

// Original GOG instruction ranges, including the branches skipped by each detour.
const std::uint8_t options_bytes[] = {
    0x8a,0x41,0x1f,0x84,0xc0,0x75,0x12,0xc7,0x44,0x24,0x4c,0x20,0x03,0,0,0xc7,
    0x44,0x24,0x50,0x58,0x02,0,0,0xeb,0x2a,0x3c,0x01,0x75,0x12,0xc7,0x44,0x24,
    0x4c,0,0x04,0,0,0xc7,0x44,0x24,0x50,0,0x03,0,0,0xeb,0x14,0x3c,0x02,0x75,
    0x10,0xc7,0x44,0x24,0x4c,0,0x05,0,0,0xc7,0x44,0x24,0x50,0,0x04,0,0};
const std::uint8_t before_bytes[] = {
    0x48,0x74,0x27,0x48,0x74,0x12,0xc7,0x44,0x24,0x3c,0x20,0x03,0,0,0xc7,0x44,
    0x24,0x40,0x58,0x02,0,0,0xeb,0x22,0xc7,0x44,0x24,0x3c,0,0x05,0,0,0xc7,0x44,
    0x24,0x40,0,0x04,0,0,0xeb,0x10,0xc7,0x44,0x24,0x3c,0,0x04,0,0,0xc7,0x44,
    0x24,0x40,0,0x03,0,0};
const std::uint8_t after_bytes[] = {
    0x3b,0xc3,0x75,0x12,0xc7,0x44,0x24,0x24,0x20,0x03,0,0,0xc7,0x44,0x24,0x28,
    0x58,0x02,0,0,0xeb,0x2c,0x83,0xf8,0x01,0x75,0x12,0xc7,0x44,0x24,0x24,0,0x04,
    0,0,0xc7,0x44,0x24,0x28,0,0x03,0,0,0xeb,0x15,0x83,0xf8,0x02,0x75,0x10,0xc7,
    0x44,0x24,0x24,0,0x05,0,0,0xc7,0x44,0x24,0x28,0,0x04,0,0};
const std::uint8_t width_bytes[] = {0xa1,0xe0,0xcc,0x6d,0};
// Use the existing 1280-wide accelerated-map layout for every supported wider mode.
constexpr std::uint32_t accelerated_map_width_branch_rva = 0x29ae0;
constexpr std::array<std::uint8_t, 6> accelerated_map_width_branch_original = {
    0x0f,0x85,0xff,0x01,0,0};
constexpr std::array<std::uint8_t, 6> accelerated_map_width_branch_replacement = {
    0x0f,0x82,0xff,0x01,0,0};
const std::uint8_t options_begin_bytes[] = {0x89,0x8d,0xb8,0x01,0,0};
const std::uint8_t resolution_label_bytes[] = {0x8b,0x14,0x8d,0x38,0xae,0x69,0};
const std::uint8_t color_depth_load_bytes[] = {0x89,0x86,0x64,0x01,0x00,0x00};
const std::uint8_t fast_forward_layout_call_bytes[] = {0xe8,0xc6,0x39,0x0b,0x00};
const std::uint8_t accelerated_map_load_call_bytes[] = {0xe8,0xf7,0x55,0x22,0x00};
const std::uint8_t decrement_call_bytes[] = {0xe8,0x5d,0x3a,0xfa,0xff};
const std::uint8_t increment_call_bytes[] = {0xe8,0xeb,0x39,0xfa,0xff};
const std::uint8_t color_depth_decrement_call_bytes[] = {0xe8,0xbe,0x3a,0xfa,0xff};
const std::uint8_t color_depth_increment_call_bytes[] = {0xe8,0x71,0x3a,0xfa,0xff};
const std::uint8_t options_hit_test_call_bytes[] = {0xe8,0x3a,0x86,0x06,0x00};
const std::uint8_t cancel_escape_call_bytes[] = {0xe8,0x0b,0x10,0,0};
const std::uint8_t cancel_button_call_bytes[] = {0xe8,0x6e,0x01,0,0};
const std::uint8_t apply_button_call_bytes[] = {0xe8,0x52,0xdd,0xff,0xff};

struct Patch {
    std::uint32_t rva;
    const std::uint8_t* original;
    std::size_t length;
    void (*entry)();
    void** continuation;
    bool relative_call = false;
    bool requires_accelerated_map = false;
};
const Patch patches[] = {
    {0x12c5, fast_forward_layout_call_bytes, sizeof(fast_forward_layout_call_bytes), reinterpret_cast<void (*)()>(place_fast_forward_map_ui_element), nullptr, true, true},
    {0x23bbd, options_bytes, sizeof(options_bytes), options_resolution, &after_options},
    {0x32d1e, before_bytes, sizeof(before_bytes), scene_resolution_before, &before_scene},
    {0x63fbb, after_bytes, sizeof(after_bytes), scene_resolution_after, &after_scene},
    {0x29ac0, width_bytes, sizeof(width_bytes), sidebar_layout, &after_sidebar},
    {0x29fe0, width_bytes, sizeof(width_bytes), top_bar_layout, &after_top_bar},
    {0x5e6e0, options_begin_bytes, sizeof(options_begin_bytes), begin_options_resolution_hook, &after_options_begin},
    {0x5eec9, resolution_label_bytes, sizeof(resolution_label_bytes), options_resolution_label_hook, &after_resolution_label},
    {0x5cf6e, color_depth_load_bytes, sizeof(color_depth_load_bytes), force_32bit_color_depth_on_load, &after_color_depth_load},
    {0x29cc5, accelerated_map_load_call_bytes, sizeof(accelerated_map_load_call_bytes), reinterpret_cast<void (*)()>(load_accelerated_map), nullptr, true, true},
    {0x5f271, options_hit_test_call_bytes, sizeof(options_hit_test_call_bytes), reinterpret_cast<void (*)()>(options_button_hit_test_hook), nullptr, true},
    {0x5f2ae, decrement_call_bytes, sizeof(decrement_call_bytes), reinterpret_cast<void (*)()>(decrement_options_resolution), nullptr, true},
    {0x5f2d0, increment_call_bytes, sizeof(increment_call_bytes), reinterpret_cast<void (*)()>(increment_options_resolution), nullptr, true},
    {0x5f2dd, color_depth_decrement_call_bytes, sizeof(color_depth_decrement_call_bytes), reinterpret_cast<void (*)()>(keep_32bit_color_depth), nullptr, true},
    {0x5f2ea, color_depth_increment_call_bytes, sizeof(color_depth_increment_call_bytes), reinterpret_cast<void (*)()>(keep_32bit_color_depth), nullptr, true},
    {0x5f240, cancel_escape_call_bytes, sizeof(cancel_escape_call_bytes), reinterpret_cast<void (*)()>(cancel_options_resolution), nullptr, true},
    {0x600dd, cancel_button_call_bytes, sizeof(cancel_button_call_bytes), reinterpret_cast<void (*)()>(cancel_options_resolution), nullptr, true},
    {0x601b9, apply_button_call_bytes, sizeof(apply_button_call_bytes), reinterpret_cast<void (*)()>(apply_options_resolution), nullptr, true}
};
constexpr std::size_t patch_start = 0x1000;
constexpr std::size_t patch_end = 0x63ffd;
constexpr std::size_t patch_span = patch_end - patch_start;

bool sha256_matches(const unsigned char* data, std::size_t size, const std::array<std::uint8_t, 32>& expected) {
    if (size > std::numeric_limits<ULONG>::max()) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    ULONG object_size = 0, written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    bool success = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written, 0) >= 0;
    std::vector<unsigned char> object(success ? object_size : 0);
    success = success && BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
    success = success && BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0;
    success = success && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success && digest == expected;
}

// True when the PE file at path has the given link time and its named section's raw data has the expected SHA-256.
bool verified_section(const std::filesystem::path& path, std::uint32_t timestamp, const char* section_name,
    const std::array<std::uint8_t, 32>& expected) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (file.bad() || bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    const auto headers = static_cast<std::size_t>(static_cast<std::uint32_t>(dos.e_lfanew));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || headers > bytes.size() ||
        bytes.size() - headers < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) return false;
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    std::memcpy(&signature, bytes.data() + headers, sizeof(signature));
    std::memcpy(&header, bytes.data() + headers + sizeof(signature), sizeof(header));
    if (signature != IMAGE_NT_SIGNATURE || header.Machine != IMAGE_FILE_MACHINE_I386 || header.TimeDateStamp != timestamp)
        return false;
    const std::size_t table = headers + sizeof(signature) + sizeof(header) + header.SizeOfOptionalHeader;
    for (std::size_t index = 0; index < header.NumberOfSections; ++index) {
        const std::size_t offset = table + index * sizeof(IMAGE_SECTION_HEADER);
        if (offset > bytes.size() || bytes.size() - offset < sizeof(IMAGE_SECTION_HEADER)) return false;
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, bytes.data() + offset, sizeof(section));
        if (std::strncmp(reinterpret_cast<const char*>(section.Name), section_name, IMAGE_SIZEOF_SHORT_NAME) != 0)
            continue;
        if (section.PointerToRawData > bytes.size() || bytes.size() - section.PointerToRawData < section.SizeOfRawData)
            return false;
        return sha256_matches(bytes.data() + section.PointerToRawData, section.SizeOfRawData, expected);
    }
    return false;
}

bool verified_executable(const std::filesystem::path& path) {
    return verified_section(path, executable_timestamp, ".rdata", executable_rdata_sha256);
}

bool read_map_asset(const std::filesystem::path& path, sgl::DisplaySize expected_size,
    std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    constexpr std::uintmax_t maximum_asset_size = 32u * 1024u * 1024u;
    if (error || file_size < 54 || file_size > maximum_asset_size || file_size > LONG_MAX) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::vector<std::uint8_t> candidate(static_cast<std::size_t>(file_size));
    if (!input.read(reinterpret_cast<char*>(candidate.data()), static_cast<std::streamsize>(candidate.size())) ||
        static_cast<std::size_t>(input.gcount()) != candidate.size()) return false;

    std::uint32_t dib_size = 0, width = 0, height = 0, pixel_offset = 0;
    std::uint16_t bits_per_pixel = 0;
    std::memcpy(&pixel_offset, candidate.data() + 10, sizeof(pixel_offset));
    std::memcpy(&dib_size, candidate.data() + 14, sizeof(dib_size));
    std::memcpy(&width, candidate.data() + 18, sizeof(width));
    std::memcpy(&height, candidate.data() + 22, sizeof(height));
    std::memcpy(&bits_per_pixel, candidate.data() + 28, sizeof(bits_per_pixel));
    if (candidate[0] != 'B' || candidate[1] != 'M' || dib_size < 40 ||
        width != expected_size.width || height != expected_size.height ||
        (bits_per_pixel != 8 && bits_per_pixel != 24 && bits_per_pixel != 32) ||
        pixel_offset >= candidate.size()) return false;
    bytes = std::move(candidate);
    return true;
}

bool is_original_accelerated_map(const char* path, const std::uint8_t* data, long size) noexcept {
    constexpr char map_name[] = "images/Vollansichtskarte1280.bmp";
    if (!path || !data || size < 54) return false;
    const auto path_length = std::strlen(path);
    if (path_length < sizeof(map_name) - 1 ||
        _stricmp(path + path_length - (sizeof(map_name) - 1), map_name) != 0 ||
        data[0] != 'B' || data[1] != 'M') return false;
    std::uint32_t width = 0, height = 0;
    std::memcpy(&width, data + 18, sizeof(width));
    std::memcpy(&height, data + 22, sizeof(height));
    return width == 1280 && height == 1024;
}

int __cdecl decode_accelerated_map(void* image, const char* path, std::uint8_t* data, long size) noexcept {
    using Decoder = int (__cdecl*)(void*, const char*, std::uint8_t*, long);
    auto original = reinterpret_cast<Decoder>(original_aim_map_decoder);
    if (!original) return 0;
    const auto index = resolution_index(game_width, game_height);
    if (sgl::image_decoder_active() || index == 0 || !is_original_accelerated_map(path, data, size))
        return original(image, path, data, size);

    const auto& replacement = accelerated_map_assets[index];
    if (replacement.empty() || replacement.size() > LONG_MAX) return 0;
    const int result = original(image, path, const_cast<std::uint8_t*>(replacement.data()),
        static_cast<long>(replacement.size()));
    if (result) sgl::set_fast_forward_map_source_size(supported_sizes[index]);
    return result;
}

bool load_accelerated_map_assets(const std::filesystem::path& executable, std::string& error) {
    const auto directory = executable.parent_path() / L"renderer-assets" / L"fast-forward-map";
    for (std::size_t index = 1; index < supported_sizes.size(); ++index) {
        const auto filename = L"Vollansichtskarte" + std::to_wstring(supported_sizes[index].width) + L".bmp";
        if (!read_map_asset(directory / filename, supported_sizes[index], accelerated_map_assets[index])) {
            error = "Fast-forward map asset is missing or invalid: " + (directory / filename).string();
            accelerated_map_assets = {};
            return false;
        }
    }
    return true;
}

bool install_aim_map_hook(const std::filesystem::path& executable, std::string& error) {
    const auto module = GetModuleHandleW(L"AIM.dll");
    if (!module) { error = "Fast-forward map requires the original AIM.dll to be loaded"; return false; }
    std::array<wchar_t, 32768> module_path{};
    const auto path_length = GetModuleFileNameW(module, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (!path_length || path_length == module_path.size()) {
        error = "Fast-forward map cannot resolve the loaded AIM.dll path";
        return false;
    }
    const std::filesystem::path aim_path(module_path.data());
    if (_wcsicmp(aim_path.filename().c_str(), L"AIM.dll") != 0) {
        error = "Fast-forward map loaded an unexpected module name: " + aim_path.filename().string();
        return false;
    }
    if (!verified_section(aim_path, aim_timestamp, ".text", aim_text_sha256)) {
        error = "Fast-forward map AIM.dll code does not match the verified original";
        return false;
    }
    if (!load_accelerated_map_assets(executable, error)) return false;

    auto* const base = reinterpret_cast<std::uint8_t*>(module);
    auto* const call_site = base + aim_map_decode_call_rva;
    if (std::memcmp(call_site, aim_map_decode_call_original.data(), aim_map_decode_call_original.size()) != 0) {
        error = "Fast-forward map: AIM.dll decode call does not match the verified profile";
        accelerated_map_assets = {};
        return false;
    }
    std::int32_t original_displacement = 0;
    std::memcpy(&original_displacement, call_site + 1, sizeof(original_displacement));
    auto* const original = call_site + aim_map_decode_call_original.size() + original_displacement;
    if (original != base + 0x3fd0) {
        error = "Fast-forward map: AIM.dll decoder target does not match the verified profile";
        accelerated_map_assets = {};
        return false;
    }
    const auto displacement = reinterpret_cast<std::intptr_t>(&decode_accelerated_map) -
        reinterpret_cast<std::intptr_t>(call_site + aim_map_decode_call_original.size());
    if (displacement < LONG_MIN || displacement > LONG_MAX) {
        error = "Fast-forward map: AIM.dll hook is outside the x86 relative-call range";
        accelerated_map_assets = {};
        return false;
    }

    DWORD previous_protection = 0;
    if (!VirtualProtect(call_site, aim_map_decode_call_original.size(), PAGE_EXECUTE_READWRITE, &previous_protection)) {
        error = "Fast-forward map: cannot patch the verified AIM.dll decoder call";
        accelerated_map_assets = {};
        return false;
    }
    const std::uint8_t opcode = 0xe8;
    std::memcpy(call_site, &opcode, sizeof(opcode));
    const auto relative = static_cast<std::int32_t>(displacement);
    std::memcpy(call_site + 1, &relative, sizeof(relative));
    DWORD discarded = 0;
    if (!FlushInstructionCache(GetCurrentProcess(), call_site, aim_map_decode_call_original.size()) ||
        !VirtualProtect(call_site, aim_map_decode_call_original.size(), previous_protection, &discarded)) {
        DWORD restore_protection = 0;
        if (VirtualProtect(call_site, aim_map_decode_call_original.size(), PAGE_EXECUTE_READWRITE, &restore_protection)) {
            std::memcpy(call_site, aim_map_decode_call_original.data(), aim_map_decode_call_original.size());
            FlushInstructionCache(GetCurrentProcess(), call_site, aim_map_decode_call_original.size());
            VirtualProtect(call_site, aim_map_decode_call_original.size(), previous_protection, &discarded);
        }
        error = "Fast-forward map: could not finalize the AIM.dll decoder hook";
        accelerated_map_assets = {};
        return false;
    }
    aim_map_module = module;
    original_aim_map_decoder = original;
    aim_map_hook_installed = true;
    return true;
}

void remove_aim_map_hook() noexcept {
    if (aim_map_hook_installed && aim_map_module) {
        auto* const call_site = reinterpret_cast<std::uint8_t*>(aim_map_module) + aim_map_decode_call_rva;
        DWORD protection = 0, discarded = 0;
        if (!VirtualProtect(call_site, aim_map_decode_call_original.size(), PAGE_EXECUTE_READWRITE, &protection))
            sgl::unimplemented("fast-forward map: cannot restore AIM.dll decoder call");
        std::memcpy(call_site, aim_map_decode_call_original.data(), aim_map_decode_call_original.size());
        if (!FlushInstructionCache(GetCurrentProcess(), call_site, aim_map_decode_call_original.size()) ||
            !VirtualProtect(call_site, aim_map_decode_call_original.size(), protection, &discarded))
            sgl::unimplemented("fast-forward map: cannot finalize restored AIM.dll decoder call");
    }
    aim_map_hook_installed = false;
    aim_map_module = nullptr;
    original_aim_map_decoder = nullptr;
    accelerated_map_assets = {};
}
}

namespace {
// FUN_004bd180 (Patrician II: FUN_004a93c0) runs once per frame: if fewer than 20 ms passed since the previous frame
// it sleeps for the rest, otherwise Sleep(1). The accumulated timeGetTime delta drives game time, so the limit sets
// the frame rate only. Bytes: cmp ecx, 20 / jae +0x18 / sub eax, esi / add eax, 20.
std::size_t frame_limiter_rva() noexcept { return sgl::game::patrician2() ? 0xa93e9 : 0xbd1a9; }
constexpr std::array<std::uint8_t, 10> frame_limiter_original{0x83, 0xf9, 0x14, 0x73, 0x18, 0x2b, 0xc6, 0x83, 0xc0, 0x14};
constexpr std::size_t frame_limiter_compare = 2;
constexpr std::size_t frame_limiter_sleep = 9;

bool write_frame_limit(std::uint8_t* base, std::uint8_t milliseconds) noexcept {
    auto* const code = base + frame_limiter_rva();
    std::array<std::uint8_t, 10> current{};
    std::memcpy(current.data(), code, current.size());
    current[frame_limiter_compare] = current[frame_limiter_sleep] = frame_limiter_original[frame_limiter_compare];
    if (current != frame_limiter_original) return false;
    DWORD protection = 0, discarded = 0;
    if (!VirtualProtect(code, frame_limiter_original.size(), PAGE_EXECUTE_READWRITE, &protection)) return false;
    code[frame_limiter_compare] = code[frame_limiter_sleep] = milliseconds;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), code, frame_limiter_original.size()) != FALSE;
    return VirtualProtect(code, frame_limiter_original.size(), protection, &discarded) && flushed;
}
}

bool sgl::game_binding_active() noexcept { return game_module != nullptr; }

bool sgl::set_game_frame_limit(std::optional<unsigned> max_fps) noexcept {
    // Patrician II has no game binding; write_frame_limit checks the limiter bytes of the running executable.
    auto* const base = sgl::game::patrician2() ? reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)) : game_module;
    if (!base) return true;
    std::uint8_t milliseconds = frame_limiter_original[frame_limiter_compare];
    if (max_fps) {
        if (*max_fps != 0 && (*max_fps < 10 || *max_fps > 1000)) return false;
        milliseconds = *max_fps ? static_cast<std::uint8_t>((1000u + *max_fps / 2) / *max_fps) : 0;
    }
    return write_frame_limit(base, milliseconds);
}

bool sgl::game_owns_layout() noexcept {
    static int state = -1;
    if (state < 0) {
        // Read once: the size the game requests stays its own for the whole session.
        bool from_game = false;
        try { from_game = display_size_from_game(display_config_path()); }
        catch (...) { }
        state = sgl::game::executable() != sgl::game::Executable::Other && from_game ? 1 : 0;
    }
    return state == 1;
}

bool sgl::install_game_resolution(DisplaySize size, std::string& error) {
    if (game_module && game_owned_layout) return update_game_resolution(size);
    if (game_module) return game_width == size.width && game_height == size.height;
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) { error = "Cannot identify the game executable"; return false; }
    const std::filesystem::path executable(path.data());
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (game_owns_layout()) {
        if (!is_valid_display_size(size)) {
            error = "Unsupported display resolution " + std::to_string(size.width) + "x" + std::to_string(size.height);
            return false;
        }
        game_width = size.width;
        game_height = size.height;
        accelerated_map_source = size;
        game_owned_layout = true;
        game_module = base;
        return true;
    }
    if (!verified_executable(executable) || base != reinterpret_cast<void*>(0x400000)) {
        // Offscreen SGL comparison programs do not contain the game's layout code.
        if (_wcsnicmp(executable.filename().c_str(), L"Patrician3", 10) != 0) return true;
        error = "Game resolution requires the Patrician III build of the GOG or Steam release";
        return false;
    }
    for (const auto& patch : patches) {
        if (std::memcmp(base + patch.rva, patch.original, patch.length) != 0) {
            // Usually another mod patched the same code, such as the p3-lib high_res mod this renderer replaces.
            char address[16] = {};
            std::snprintf(address, sizeof(address), "0x%06zx", static_cast<std::size_t>(0x400000 + patch.rva));
            error = std::string("Game resolution: executable code at ") + address +
                " does not match the verified profile (changed by another mod?)";
            return false;
        }
    }
    if (std::memcmp(base + accelerated_map_width_branch_rva,
            accelerated_map_width_branch_original.data(), accelerated_map_width_branch_original.size()) != 0) {
        error = "Fast-forward map: executable code does not match the verified GOG profile";
        return false;
    }
    std::string accelerated_map_error;
    const bool accelerated_map_available = install_aim_map_hook(executable, accelerated_map_error);
    if (!accelerated_map_available) {
        const auto aim_module = GetModuleHandleW(L"AIM.dll");
        if (aim_module && std::memcmp(
            reinterpret_cast<std::uint8_t*>(aim_module) + aim_map_decode_call_rva,
            aim_map_decode_call_original.data(), aim_map_decode_call_original.size()) != 0) {
            error = "Fast-forward map hook could not be installed or safely restored: " + accelerated_map_error;
            return false;
        }
        OutputDebugStringA("Patrician III D3D11: fast-forward map override unavailable: ");
        OutputDebugStringA(accelerated_map_error.c_str());
        OutputDebugStringA("\n");
    }
    std::int32_t fast_forward_layout_displacement = 0;
    std::memcpy(&fast_forward_layout_displacement, base + 0x12c6, sizeof(fast_forward_layout_displacement));
    original_fast_forward_layout = reinterpret_cast<void*>(
        reinterpret_cast<std::intptr_t>(base + 0x12ca) + fast_forward_layout_displacement);
    DWORD code_protection = 0;
    if (!VirtualProtect(base + patch_start, patch_span, PAGE_EXECUTE_READWRITE, &code_protection)) {
        remove_aim_map_hook();
        error = "Cannot make game resolution instructions writable";
        return false;
    }
    game_width = size.width;
    game_height = size.height;
    accelerated_map_source = size;
    game_module = base;
    for (const auto& patch : patches) {
        if (patch.requires_accelerated_map && !accelerated_map_available) continue;
        if (patch.continuation) *patch.continuation = base + patch.rva + patch.length;
        const auto relative = reinterpret_cast<std::uintptr_t>(patch.entry) -
            reinterpret_cast<std::uintptr_t>(base + patch.rva + 5);
        base[patch.rva] = patch.relative_call ? 0xe8 : 0xe9;
        std::memcpy(base + patch.rva + 1, &relative, 4);
    }
    if (accelerated_map_available) {
        std::memcpy(base + accelerated_map_width_branch_rva,
            accelerated_map_width_branch_replacement.data(), accelerated_map_width_branch_replacement.size());
    }
    DWORD discarded = 0;
    if (!FlushInstructionCache(GetCurrentProcess(), base + patch_start, patch_span) ||
        !VirtualProtect(base + patch_start, patch_span, code_protection, &discarded))
        sgl::unimplemented("game resolution: cannot finalize instruction patches");
    return true;
}

bool sgl::update_game_resolution(DisplaySize size) noexcept {
    // game_owns_layout, not game_owned_layout: Patrician II never installs the binding that sets the latter.
    if (game_owns_layout() ? !is_valid_display_size(size) : !is_supported_display_size(size)) return false;
    if (game_module) {
        game_width = size.width;
        game_height = size.height;
        accelerated_map_source = size;
        resize_accelerated_map_layout();
    }
    return true;
}

bool sgl::refresh_accelerated_map_layout() noexcept {
    return resize_accelerated_map_layout();
}

sgl::DisplaySize sgl::current_game_resolution() noexcept {
    return {game_width, game_height};
}

sgl::DisplaySize sgl::fast_forward_map_source_size() noexcept {
    return accelerated_map_source;
}

void sgl::set_fast_forward_map_source_size(DisplaySize size) noexcept {
    if (!size.width || !size.height ||
        (size.width == accelerated_map_source.width && size.height == accelerated_map_source.height)) return;
    accelerated_map_source = size;
    resize_accelerated_map_layout();
}

void sgl::remove_game_resolution() noexcept {
    if (!game_module) return;
    write_frame_limit(game_module, frame_limiter_original[frame_limiter_compare]);
    if (game_owned_layout) {
        // Nothing else was patched; the executable's own wide-screen code must stay intact.
        game_module = nullptr;
        game_owned_layout = false;
        accelerated_map_source = {1280, 1024};
        return;
    }
    remove_aim_map_hook();
    DWORD protection = 0, discarded = 0;
    if (!VirtualProtect(game_module + patch_start, patch_span, PAGE_EXECUTE_READWRITE, &protection))
        sgl::unimplemented("game resolution: cannot restore instructions");
    for (const auto& patch : patches) std::memcpy(game_module + patch.rva, patch.original, 5);
    std::memcpy(game_module + accelerated_map_width_branch_rva,
        accelerated_map_width_branch_original.data(), accelerated_map_width_branch_original.size());
    if (!FlushInstructionCache(GetCurrentProcess(), game_module + patch_start, patch_span) ||
        !VirtualProtect(game_module + patch_start, patch_span, protection, &discarded))
        sgl::unimplemented("game resolution: cannot finalize restored instructions");
    game_module = nullptr;
    accelerated_map_loaded = false;
    original_fast_forward_layout = nullptr;
    accelerated_map_source = {1280, 1024};
    accelerated_map_layout = nullptr;
    accelerated_map_frame = nullptr;
    accelerated_map_layout_size = {};
}

void sgl::adapt_game_chrome(const char* name, DecodedImage& image) {
    adapt_game_chrome(name, image, {game_width, game_height});
}

sgl::DisplaySize sgl::pending_game_resolution() noexcept {
    return supported_sizes[pending_resolution];
}

void sgl::adapt_game_chrome(const char* name, DecodedImage& image, DisplaySize size) {
    if (!game_module || game_owned_layout || !name) return;
    if (_stricmp(name, "images/HauptscreenE1280.bmp") == 0) {
        if (image.width != 284 || image.height != 424)
            sgl::unimplemented("game resolution: unexpected original sidebar dimensions");
        const auto height = size.width > 1280 ? size.height - 600 : 424u;
        if (!image.pixels.empty()) {
            const auto capacity = static_cast<std::uint64_t>(image.pitch) * 840u;
            if (capacity > SIZE_MAX) sgl::unimplemented("game resolution: sidebar image capacity");
            try { image.pixels.reserve(static_cast<std::size_t>(capacity)); }
            catch (const std::bad_alloc&) { sgl::unimplemented("game resolution: sidebar image allocation"); }
            catch (const std::length_error&) { sgl::unimplemented("game resolution: sidebar image allocation"); }
        }
        if (!image.pixels.empty()) {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(image.pitch) * height);
            constexpr unsigned edge = 24;
            for (unsigned y = 0; y < height; ++y) {
                const auto source_y = y < edge ? y : y >= height - edge ? image.height - (height - y) :
                    edge + (y - edge) * (image.height - 2 * edge) / (height - 2 * edge);
                std::memcpy(pixels.data() + static_cast<std::size_t>(y) * image.pitch,
                    image.pixels.data() + static_cast<std::size_t>(source_y) * image.pitch, image.pitch);
            }
            image.pixels.resize(pixels.size());
            std::memcpy(image.pixels.data(), pixels.data(), pixels.size());
        }
        image.height = height;
        return;
    }
    if (_stricmp(name, "images/HauptscreenA1280.bmp") != 0) return;
    if (image.width != 996 || image.height != 42)
        sgl::unimplemented("game resolution: unexpected original top bar dimensions");
    const auto width = size.width > 1280 ? size.width - 284 : 996u;
    const auto pixel_size = bytes_per_pixel(image.format);
    const auto pitch = (width * pixel_size + 3u) & ~3u;
    if (!image.pixels.empty()) {
        const auto maximum_pitch = (2276u * pixel_size + 3u) & ~3u;
        const auto capacity = static_cast<std::uint64_t>(maximum_pitch) * image.height;
        if (capacity > SIZE_MAX) sgl::unimplemented("game resolution: top bar image capacity");
        try { image.pixels.reserve(static_cast<std::size_t>(capacity)); }
        catch (const std::bad_alloc&) { sgl::unimplemented("game resolution: top bar image allocation"); }
        catch (const std::length_error&) { sgl::unimplemented("game resolution: top bar image allocation"); }
    }
    if (!image.pixels.empty()) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(pitch) * image.height);
        constexpr unsigned edge = 24;
        for (unsigned y = 0; y < image.height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto source_x = x < edge ? x : x >= width - edge ? image.width - (width - x) :
                    edge + (x - edge) * (image.width - 2 * edge) / (width - 2 * edge);
                std::memcpy(pixels.data() + static_cast<std::size_t>(y) * pitch + x * pixel_size,
                    image.pixels.data() + static_cast<std::size_t>(y) * image.pitch + source_x * pixel_size,
                    pixel_size);
            }
        }
        image.pixels.resize(pixels.size());
        std::memcpy(image.pixels.data(), pixels.data(), pixels.size());
    }
    image.width = width;
    image.pitch = pitch;
}
