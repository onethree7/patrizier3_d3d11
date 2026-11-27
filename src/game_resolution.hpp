#pragma once

#include "display_config.hpp"
#include "image_decoder.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace sgl {
enum class DisplayApplyResult { Failed, Applied, AppliedNotSaved };

struct FastForwardMapLayoutState {
    std::int32_t x, y;
    std::uint32_t width, height;
    std::uint32_t frame_width, frame_height;
};

constexpr bool fast_forward_map_layout_matches(FastForwardMapLayoutState state,
    DisplaySize display, DisplaySize source) noexcept {
    return state.x == 0 && state.y == 0 &&
        state.width == display.width && state.height == display.height &&
        state.frame_width == source.width && state.frame_height == source.height;
}

// True for Patrician II or III when high_res.ini sets width or height to 0 (or leaves them out): the executable is
// patched to lay out every screen size itself, so the renderer accepts any valid size the game requests and installs
// no layout, options or map hooks.
bool game_owns_layout() noexcept;
bool install_game_resolution(DisplaySize size, std::string& error);
bool update_game_resolution(DisplaySize size) noexcept;
bool refresh_accelerated_map_layout() noexcept;
void remove_game_resolution() noexcept;
// Rewrites the minimum frame time of the game's frame limiter (FUN_004bd180, originally 20 ms). No value restores
// the original limiter. Patrician III requires an installed game resolution binding and is a no-op without one;
// Patrician II rewrites the running executable's limiter (FUN_004a93c0) after checking its bytes.
bool set_game_frame_limit(std::optional<unsigned> max_fps) noexcept;
// True while the verified GOG executable's in-memory bindings are installed.
bool game_binding_active() noexcept;
DisplaySize pending_game_resolution() noexcept;
DisplayApplyResult apply_options_display_resolution(std::string& error);
DisplaySize current_game_resolution() noexcept;
DisplaySize fast_forward_map_source_size() noexcept;
void set_fast_forward_map_source_size(DisplaySize size) noexcept;
void adapt_game_chrome(const char* name, DecodedImage& image);
void adapt_game_chrome(const char* name, DecodedImage& image, DisplaySize size);
}
