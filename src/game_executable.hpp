#pragma once

#include <windows.h>
#include <cstdint>

// The running game, identified by the link time of the executable loaded at its preferred base. One DLL serves both
// games: Patrician II gets its own addresses and none of the Patrician3.exe hooks; every other program, including the
// SGL test programs, keeps the Patrician III behaviour.
namespace sgl::game {

enum class Executable { Other, Patrician2, Patrician3 };

// Patrician3.exe of the GOG and Steam releases and Patrician 2.exe.
inline constexpr std::uint32_t patrician3_timestamp = 0x3f544d3c;
inline constexpr std::uint32_t patrician2_timestamp = 0x3f1fb3a2;

inline Executable executable() noexcept {
    static const Executable value = [] {
        const auto* const base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        if (reinterpret_cast<std::uintptr_t>(base) != 0x400000) return Executable::Other;
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return Executable::Other;
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
            return Executable::Other;
        if (nt->FileHeader.TimeDateStamp == patrician3_timestamp) return Executable::Patrician3;
        if (nt->FileHeader.TimeDateStamp == patrician2_timestamp) return Executable::Patrician2;
        return Executable::Other;
    }();
    return value;
}

inline bool patrician2() noexcept { return executable() == Executable::Patrician2; }

inline const char* renderer_name() noexcept {
    return patrician2() ? "Patrician II D3D11 renderer" : "Patrician III D3D11 renderer";
}

}
