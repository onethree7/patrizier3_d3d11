#pragma once

// The build variant, chosen by CMake: SGL_DIAGNOSTICS=ON adds the renderer counters to the FPS overlay and writes
// sgl_readbacks.log.
namespace sgl::build {

#ifdef SGL_DIAGNOSTICS
inline constexpr bool diagnostics = true;
#else
inline constexpr bool diagnostics = false;
#endif

}
