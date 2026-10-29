#pragma once

// HD texture replacement with PCSX2-format packs (textures/<serial>/replacements/*.png).
// The host supplies the list of file names and loads PNGs on request; see gs/hd.hpp.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace ps2 {

// request(file name, id, native width, native height): load that PNG and hand it back with hd_loaded(id, ...).
// Returns the number of usable replacement names, 0 if HD textures are unavailable (no GPU buffer, no names).
std::size_t hd_setup(std::function<void(const std::string &, std::uint32_t, std::uint32_t, std::uint32_t)> request, const char *names, std::size_t length);
// rgba: width*height RGBA8 texels at (native size << shift); null = could not load.
void hd_loaded(std::uint32_t id, const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, std::uint32_t shift);

}
