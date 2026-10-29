#pragma once

// HD texture replacement keyed exactly like PCSX2's texture replacements
// (textures/<serial>/replacements/<TEX0hash>-<CLUThash>-<bits>.png), so an existing
// PCSX2 pack works unchanged. See GSTextureCache::HashCacheKey::Create in PCSX2.

#include <cstdint>
#include <functional>
#include <string>

namespace ps2::gs::hd {

struct Key {
    std::uint64_t tex = 0, clut = 0;
    std::uint32_t bits = 0;
    bool operator==(const Key &) const = default;
};

struct KeyHash {
    std::size_t operator()(const Key &k) const { return k.tex ^ (k.clut * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(k.bits) << 17); }
};

std::string name(const Key &k);
bool parse(const std::string &filename, Key &out);

// Texel reader for the texture being hashed: returns the raw value of (x, y) in the
// texture's own format (palette index for T4/T8/T8H/T4HL/T4HH, colour otherwise).
using TexelReader = std::function<std::uint32_t(std::uint32_t x, std::uint32_t y)>;
// Returns the 256-byte GS memory block holding block (bx, by) of the texture, or null.
using BlockReader = std::function<const std::uint8_t *(std::uint32_t bx, std::uint32_t by)>;

// palette32: the palette as PCSX2 holds it in GSClut::m_buff32 (32-bit colours in index
// order, 16-bit CLUT entries expanded with TEXA). Ignored for non-palette formats.
// Returns false for textures PCSX2 would key with a region (not supported yet).
bool compute(std::uint64_t tex0, std::uint64_t texa, std::uint64_t clamp, const BlockReader &blocks, const TexelReader &texels,
             const std::uint32_t *palette32, Key &out);

std::uint32_t expand16(std::uint32_t c, std::uint64_t texa);
std::uint32_t bits(std::uint64_t tex0, std::uint64_t texa);
std::uint64_t palette_hash(const std::uint32_t *palette32, std::uint32_t entries);
std::uint64_t fingerprint(const void *data, std::size_t bytes);

}
