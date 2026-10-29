#include "hd.hpp"

#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ps2::gs::hd {

namespace {

struct PsmInfo {
    std::uint32_t bsx, bsy, pal, fmsk;
    bool direct_texa;  // PCSX2: pal == 0 && fmt > 0 (CT24 / CT16 / CT16S): TEXA is part of the key
    std::uint32_t bytes;  // expanded bytes per texel when hashing the slow path (1 = index, 4 = RGBA)
};

bool info(std::uint32_t psm, PsmInfo &p) {
    switch (psm) {
    case 0x00: p = {8, 8, 0, 0xFFFFFFFFu, false, 4}; return true;    // CT32
    case 0x01: p = {8, 8, 0, 0x00FFFFFFu, true, 4}; return true;     // CT24
    case 0x02: p = {16, 8, 0, 0x80F8F8F8u, true, 4}; return true;    // CT16
    case 0x0A: p = {16, 8, 0, 0x80F8F8F8u, true, 4}; return true;    // CT16S
    case 0x13: p = {16, 16, 256, 0xFFFFFFFFu, false, 1}; return true; // T8
    case 0x14: p = {32, 16, 16, 0xFFFFFFFFu, false, 1}; return true;  // T4
    case 0x1B: p = {8, 8, 256, 0xFF000000u, false, 1}; return true;   // T8H
    case 0x24: p = {8, 8, 16, 0x0F000000u, false, 1}; return true;    // T4HL
    case 0x2C: p = {8, 8, 16, 0xF0000000u, false, 1}; return true;    // T4HH
    default: return false;
    }
}

bool has_region(std::uint64_t tex0, std::uint64_t clamp) {
    const std::uint32_t tw = 1u << ((tex0 >> 26) & 15u), th = 1u << ((tex0 >> 30) & 15u);
    const std::uint32_t wms = clamp & 3u, wmt = (clamp >> 2) & 3u;
    const std::uint32_t minu = (clamp >> 4) & 0x3FFu, maxu = (clamp >> 14) & 0x3FFu;
    const std::uint32_t minv = (clamp >> 24) & 0x3FFu, maxv = (clamp >> 34) & 0x3FFu;
    const auto axis = [](std::uint32_t wm, std::uint32_t mn, std::uint32_t mx, std::uint32_t size) {
        if (wm == 2 && mx >= mn) {
            const std::uint32_t r = mx - mn + 1;
            return r < size || mx >= size;
        }
        if (wm == 3 && mn != 0) {
            const std::uint32_t r = ((mn | mx) - mx) + 1;
            return r < size || (mx != 0 && r <= size);
        }
        return false;
    };
    return axis(wms, minu, maxu, tw) || axis(wmt, minv, maxv, th);
}

}

std::uint32_t expand16(std::uint32_t c, std::uint64_t texa) {
    const std::uint32_t ta0 = texa & 0xFFu, aem = (texa >> 15) & 1u, ta1 = (texa >> 32) & 0xFFu;
    std::uint32_t a = (c & 0x8000u) ? ta1 : ta0;
    if (aem && (c & 0xFFFFu) == 0) a = 0;
    return ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9) | (a << 24);
}

std::string name(const Key &k) {
    char buf[64];
    if (k.clut) std::snprintf(buf, sizeof buf, "%" PRIx64 "-%" PRIx64 "-%08x", k.tex, k.clut, k.bits);
    else std::snprintf(buf, sizeof buf, "%" PRIx64 "-%08x", k.tex, k.bits);
    return buf;
}

bool parse(const std::string &filename, Key &out) {
    unsigned long long a = 0, b = 0;
    unsigned bits = 0;
    char dot = 0;
    if (filename.find("-r") != std::string::npos) return false;
    if (std::sscanf(filename.c_str(), "%llx-%llx-%08x%c", &a, &b, &bits, &dot) == 4 && dot == '.') {
        out = {a, b, bits};
        return true;
    }
    if (std::sscanf(filename.c_str(), "%llx-%08x%c", &a, &bits, &dot) == 3 && dot == '.') {
        out = {a, 0, bits};
        return true;
    }
    return false;
}

bool compute(std::uint64_t tex0, std::uint64_t texa, std::uint64_t clamp, const BlockReader &blocks, const TexelReader &texels,
             const std::uint32_t *palette32, Key &out) {
    const std::uint32_t psm = (tex0 >> 20) & 0x3Fu, tw_log = (tex0 >> 26) & 15u, th_log = (tex0 >> 30) & 15u;
    PsmInfo p;
    if (!info(psm, p) || tw_log > 10 || th_log > 10) return false;
    if (has_region(tex0, clamp)) return false;
    const std::uint32_t tw = 1u << tw_log, th = 1u << th_log;
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    if (tw >= p.bsx && th >= p.bsy && p.fmsk == 0xFFFFFFFFu) {
        // Fast path: the raw 256-byte blocks in block-raster order (GSOffset::bnMulti).
        for (std::uint32_t by = 0; by < th / p.bsy; ++by)
            for (std::uint32_t bx = 0; bx < tw / p.bsx; ++bx) {
                const std::uint8_t *b = blocks(bx, by);
                if (!b) return false;
                XXH3_64bits_update(&st, b, 256);
            }
    } else {
        // Slow path: texels expanded to indices (palette formats) or RGBA32 with TEXA, row by row.
        std::vector<std::uint8_t> row(static_cast<std::size_t>(tw) * p.bytes);
        for (std::uint32_t y = 0; y < th; ++y) {
            for (std::uint32_t x = 0; x < tw; ++x) {
                std::uint32_t v = texels(x, y);
                if (p.bytes == 1) {
                    row[x] = static_cast<std::uint8_t>(v);
                    continue;
                }
                if (psm == 0x01) {
                    const std::uint32_t ta0 = texa & 0xFFu, aem = (texa >> 15) & 1u;
                    v = (v & 0xFFFFFFu) | ((aem && (v & 0xFFFFFFu) == 0) ? 0u : ta0 << 24);
                } else if (psm == 0x02 || psm == 0x0A) {
                    v = expand16(v, texa);
                }
                std::memcpy(&row[x * 4], &v, 4);
            }
            XXH3_64bits_update(&st, row.data(), row.size());
        }
    }
    out.tex = XXH3_64bits_digest(&st);
    out.clut = (p.pal && palette32) ? XXH3_64bits(palette32, p.pal * 4) : 0;
    out.bits = bits(tex0, texa);
    return true;
}

std::uint32_t bits(std::uint64_t tex0, std::uint64_t texa) {
    const std::uint32_t psm = (tex0 >> 20) & 0x3Fu, tw_log = (tex0 >> 26) & 15u, th_log = (tex0 >> 30) & 15u;
    std::uint32_t b = psm | (tw_log << 6) | (th_log << 10);
    if (psm == 0x01 || psm == 0x02 || psm == 0x0A) {
        const std::uint32_t ta0 = texa & 0xFFu, aem = (texa >> 15) & 1u, ta1 = (texa >> 32) & 0xFFu;
        b |= (ta0 << 15) | (aem << 23) | (ta1 << 24);
    }
    return b;
}

std::uint64_t palette_hash(const std::uint32_t *palette32, std::uint32_t entries) { return XXH3_64bits(palette32, entries * 4); }
std::uint64_t fingerprint(const void *data, std::size_t bytes) { return XXH3_64bits(data, bytes); }

}
