#include "gs.hpp"
#include "hd.hpp"
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <tuple>
#include <functional>
#include <string>

#include "gif.hpp"
#include "queue.hpp"
#include "raster.hpp"
#include "../hw.hpp"
#ifdef PS2_GPU
#include "gpu/gpu.hpp"
#endif

#include "ps2/state.hpp"
#include "ps2/video.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <functional>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::gs {

State g_state;
std::uint32_t g_setup_generation = 0;

void serve_state(const void *payload);

namespace {

alignas(64) std::uint8_t g_memory[kMemorySize];

std::uint64_t g_pmode, g_smode1, g_smode2, g_srfsh, g_synch1, g_synch2, g_syncv;
std::uint64_t g_dispfb[2], g_display[2], g_extbuf, g_extdata, g_extwrite, g_bgcolor;
std::uint64_t g_csr = 0x551B4000ull, g_imr = 0x7F00ull, g_busdir, g_siglblid;
std::uint64_t g_primitives = 0;

#ifdef PS2_GPU
bool gpu_on() { return gpu::mode() != gpu::Mode::off; }
bool cpu_raster() { return gpu::mode() != gpu::Mode::primary; }
#else
constexpr bool gpu_on() { return false; }
constexpr bool cpu_raster() { return true; }
#endif

bool trace_display() {
    static const bool enabled = std::getenv("PS2_TRACE_DISPLAY") != nullptr;
    return enabled;
}
std::uint64_t g_writes[128];
std::vector<std::uint32_t> g_gpu_frame;
bool g_odd_field = false;

struct Transfer {
    bool active = false;
    std::uint32_t x = 0, y = 0;
    std::uint32_t pending_bits = 0, pending = 0;
} g_transfer;

std::uint32_t g_off32[32][64], g_offz32[32][64];
std::uint32_t g_off16[64][64], g_off16s[64][64], g_offz16[64][64], g_offz16s[64][64];
std::uint32_t g_off8[64][128], g_off4[128][128];

const int kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
const int kBlockZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15}, {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
const int kColumn32[2][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}};
const int kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
const int kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
const int kBlockZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23}, {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
const int kBlockZ16S[8][4] = {{24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3}, {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
const int kColumn16[2][16] = {{0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}};
const int kColumnWord[2][2][8] = {{{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}}, {{8, 9, 12, 13, 0, 1, 4, 5}, {10, 11, 14, 15, 2, 3, 6, 7}}};

void build_tables() {
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 7;
            g_off32[y][x] = static_cast<std::uint32_t>(kBlock32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
            g_offz32[y][x] = static_cast<std::uint32_t>(kBlockZ32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 15;
            const int inner = column * 64 + kColumn16[cy][cx] * 2;
            g_off16[y][x] = static_cast<std::uint32_t>(kBlock16[y / 8][x / 16] * 256 + inner);
            g_off16s[y][x] = static_cast<std::uint32_t>(kBlock16S[y / 8][x / 16] * 256 + inner);
            g_offz16[y][x] = static_cast<std::uint32_t>(kBlockZ16[y / 8][x / 16] * 256 + inner);
            g_offz16s[y][x] = static_cast<std::uint32_t>(kBlockZ16S[y / 8][x / 16] * 256 + inner);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock32[(y / 16) & 3][(x / 16) & 7];
            int wx = x % 16, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            const std::uint32_t byte = static_cast<std::uint32_t>(((wx & 8) >> 2) + ((wy & 2) >> 1));
            table ^= static_cast<std::uint32_t>((y / 4) & 1);
            g_off8[y][x] = static_cast<std::uint32_t>(block * 256 + column * 64 + kColumnWord[table][wy & 1][wx & 7] * 4) + byte;
        }
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock16[(y / 16) & 7][(x / 32) & 3];
            int wx = x % 32, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            const std::uint32_t shift = static_cast<std::uint32_t>((wx & 0x18) + ((wy & 2) << 1));
            const std::uint32_t nibble = shift / 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            table ^= static_cast<std::uint32_t>(column & 1);
            g_off4[y][x] = static_cast<std::uint32_t>((column * 64 + block * 256 + kColumnWord[table][wy & 1][wx & 7] * 4) * 2) + nibble;
        }
}

std::uint32_t wrap(std::uint32_t a) { return a & (kMemorySize - 1); }

template <typename T>
T get(std::uint32_t a) {
    T v;
    std::memcpy(&v, g_memory + wrap(a), sizeof v);
    return v;
}

template <typename T>
void put(std::uint32_t a, T v) { std::memcpy(g_memory + wrap(a), &v, sizeof v); }

std::uint32_t page_address(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t pw, std::uint32_t ph) {
    const std::uint32_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    return bp * 256 + ((x / pw) + (y / ph) * per_row) * 8192;
}

void raise(std::uint32_t bit) {
    g_csr |= bit;
    if (!(g_imr & (bit << 8))) hw::intc_raise(0);
}

std::uint32_t bits_per_pixel(std::uint32_t psm) {
    switch (psm) {
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    case PSMT8H: return 8;
    case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

struct DownloadKey {
    std::uint64_t buf, pos, reg;
    std::uint32_t offset, qwords;
    bool operator==(const DownloadKey &) const = default;
};

struct CachedDownload {
    DownloadKey key;
    std::vector<std::uint8_t> data;
    std::uint64_t used;
};

std::mutex g_cache_mutex;
std::vector<CachedDownload> g_download_cache;
std::uint64_t g_cache_clock = 0;
DownloadKey g_shadow{};

void remember(const DownloadKey &key, const std::uint8_t *data, std::size_t bytes) {
    std::lock_guard lock(g_cache_mutex);
    auto it = std::find_if(g_download_cache.begin(), g_download_cache.end(), [&](const CachedDownload &c) { return c.key == key; });
    if (it == g_download_cache.end()) {
        if (g_download_cache.size() >= 16)
            g_download_cache.erase(std::min_element(g_download_cache.begin(), g_download_cache.end(),
                                                    [](const CachedDownload &a, const CachedDownload &b) { return a.used < b.used; }));
        it = g_download_cache.insert(g_download_cache.end(), CachedDownload{key, {}, 0});
    }
    it->data.assign(data, data + bytes);
    it->used = ++g_cache_clock;
}

void forget(const DownloadKey &key) {
    std::lock_guard lock(g_cache_mutex);
    std::erase_if(g_download_cache, [&](const CachedDownload &c) { return c.key == key; });
}

#ifdef PS2_GPU
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_seen_ranges;
std::uint32_t g_transfer_serial = 0, g_stale_first = 0, g_stale_bytes = 0;
bool g_transfer_stale = false;

void download_source(const State &s) {
    const std::uint32_t sbp = static_cast<std::uint32_t>(s.bitbltbuf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(s.bitbltbuf >> 16) & 0x3Fu;
    const std::uint32_t spsm = static_cast<std::uint32_t>(s.bitbltbuf >> 24) & 0x3Fu;
    const std::uint32_t x = static_cast<std::uint32_t>(s.trxpos) & 0x7FFu, y = static_cast<std::uint32_t>(s.trxpos >> 16) & 0x7FFu;
    const std::uint32_t w = static_cast<std::uint32_t>(s.trxreg) & 0xFFFu, h = static_cast<std::uint32_t>(s.trxreg >> 32) & 0xFFFu;
    const bool half = bits_per_pixel(spsm) == 16;
    const std::uint32_t pw = spsm == PSMT8 || spsm == PSMT4 ? 128 : 64, ph = spsm == PSMT4 ? 128 : spsm == PSMT8 || half ? 64 : 32;
    const std::uint32_t first = page_address(sbp, sbw, x, y, pw, ph);
    const std::uint32_t end = page_address(sbp, sbw, x + w - 1, y + h - 1, pw, ph) + 8192;
    ++g_transfer_serial;
    g_transfer_stale = false;
    hd_touch_transfer(s.bitbltbuf, s.trxpos, s.trxreg, false);
    if (!w || !h || x + w > 2048 || y + h > 2048 || end > kMemorySize) {
        gpu::download(g_memory);
        return;
    }
    const std::pair<std::uint32_t, std::uint32_t> range{first, end - first};
    if (queue::active() && std::find(g_seen_ranges.begin(), g_seen_ranges.end(), range) != g_seen_ranges.end()) {
        gpu::download_async(g_memory, first, end - first, g_transfer_serial);
        g_transfer_stale = true;
        g_stale_first = first;
        g_stale_bytes = end - first;
        return;
    }
    gpu::download(g_memory, first, end - first);
    g_seen_ranges.push_back(range);
    if (g_seen_ranges.size() > 8) g_seen_ranges.erase(g_seen_ranges.begin());
}
#endif

void exact_download() {
#ifdef PS2_GPU
    if (!g_transfer_stale) return;
    g_transfer_stale = false;
    if (!gpu::finish_download(g_transfer_serial)) gpu::download(g_memory, g_stale_first, g_stale_bytes);
#endif
}

void transfer_pixel(std::uint32_t value) {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu;
    const std::uint32_t dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    const std::uint32_t dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu;
    const std::uint32_t dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu;
    const std::uint32_t rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!g_transfer.active || !rrw) return;
    write_pixel(dpsm, dbp, dbw, (dsax + g_transfer.x) & 0x7FFu, (dsay + g_transfer.y) & 0x7FFu, value);
    if (++g_transfer.x >= rrw) {
        g_transfer.x = 0;
        if (++g_transfer.y >= rrh) g_transfer.active = false;
    }
}

std::uint64_t g_mirror_bytes = 0;
std::uint8_t g_carry[4];
std::size_t g_carried = 0;

void feed_cpu(const std::uint8_t *data, std::size_t bytes);
bool hd_buffer_upload(const std::uint8_t *data, std::size_t bytes);

void feed_image(const std::uint8_t *data, std::size_t bytes) {
#ifdef PS2_GPU
    if (gpu_on() && (g_state.trxdir & 3u) == 0) gpu::image_data(data, bytes);
#endif
    if (!g_transfer.active || (g_state.trxdir & 3u) != 0) return;
    if (!cpu_raster()) {
        if (hd_enabled()) hd_buffer_upload(data, bytes);
        return;
    }
    feed_cpu(data, bytes);
}

void feed_cpu(const std::uint8_t *data, std::size_t bytes) {
    g_mirror_bytes += bytes;
    const std::uint32_t psm = static_cast<std::uint32_t>(g_state.bitbltbuf >> 56) & 0x3Fu;
    const std::uint32_t bits = bits_per_pixel(psm);
    if (psm == PSMCT32 && (bytes & 3u) == 0 && !g_carried) {
        // Fast path for 32-bit uploads (most texture streaming): one row segment at a time, page base hoisted.
        const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
        const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
        const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
        const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
        const std::uint32_t per_row = std::max<std::uint32_t>(1, dbw);
        std::size_t at = 0;
        while (at + 4 <= bytes && g_transfer.active && rrw) {
            const std::uint32_t y = (dsay + g_transfer.y) & 0x7FFu;
            const std::uint32_t *offs = g_off32[y & 31];
            const std::uint32_t row_base = dbp * 256 + (y / 32) * per_row * 8192;
            while (at + 4 <= bytes && g_transfer.x < rrw) {
                const std::uint32_t x = (dsax + g_transfer.x) & 0x7FFu;
                std::uint32_t v;
                std::memcpy(&v, data + at, 4);
                std::memcpy(g_memory + ((row_base + (x / 64) * 8192 + offs[x & 63]) & (kMemorySize - 1)), &v, 4);
                at += 4;
                ++g_transfer.x;
            }
            if (g_transfer.x >= rrw) {
                g_transfer.x = 0;
                if (++g_transfer.y >= rrh) g_transfer.active = false;
            }
        }
        return;
    }
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && g_transfer.active; ++i) {
            g_carry[g_carried++] = data[i];
            if (g_carried == 3) {
                transfer_pixel(g_carry[0] | (g_carry[1] << 8) | (g_carry[2] << 16));
                g_carried = 0;
            }
        }
        return;
    }
    for (std::size_t bit = 0; bit + bits <= bytes * 8 && g_transfer.active; bit += bits) {
        std::uint32_t v = 0;
        if (bits == 4) v = (data[bit / 8] >> (bit & 4)) & 15u;
        else std::memcpy(&v, data + bit / 8, bits / 8);
        transfer_pixel(v);
    }
}

// HD-only mode (GPU renderer): collect a whole host->GS transfer, then mirror it into g_memory
// unless the exact same bytes already went to the same place (and nothing else has touched those pages).
struct HdUpload {
    bool active = false;
    std::uint64_t key = 0;
    std::size_t expected = 0;
    std::vector<std::uint8_t> data;
};
HdUpload g_hd_upload;
std::uint64_t g_mirror_skipped = 0;

void hd_begin_upload() {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t psm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint64_t w = reg & 0xFFFu, h = (reg >> 32) & 0xFFFu;
    g_hd_upload.active = w && h;
    g_hd_upload.key = (buf >> 32) * 0x9E3779B97F4A7C15ull ^ (pos >> 32) * 0xC2B2AE3D27D4EB4Full ^ reg;
    g_hd_upload.expected = static_cast<std::size_t>((w * h * bits_per_pixel(psm) + 7) / 8);
    g_hd_upload.data.clear();
}

bool hd_buffer_upload(const std::uint8_t *data, std::size_t bytes) {
    HdUpload &u = g_hd_upload;
    if (!u.active) return false;
    u.data.insert(u.data.end(), data, data + bytes);
    if (u.data.size() < u.expected) return false;
    u.active = false;
    if (hd_upload_finish(u.key, u.data.data(), u.expected)) {
        g_mirror_skipped += u.expected;
        return true;
    }
    g_transfer.x = g_transfer.y = 0;
    g_transfer.active = true;
    g_carried = 0;
    feed_cpu(u.data.data(), u.expected);
    return true;
}

void local_copy() {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint32_t ssax = static_cast<std::uint32_t>(pos) & 0x7FFu, ssay = static_cast<std::uint32_t>(pos >> 16) & 0x7FFu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t dir = static_cast<std::uint32_t>(pos >> 59) & 3u;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(rrw) * rrh);
    for (std::uint32_t y = 0; y < rrh; ++y)
        for (std::uint32_t x = 0; x < rrw; ++x)
            pixels[y * rrw + x] = read_pixel(spsm, sbp, sbw, (ssax + x) & 0x7FFu, (ssay + y) & 0x7FFu);
    for (std::uint32_t y = 0; y < rrh; ++y) {
        const std::uint32_t sy = (dir & 2u) ? rrh - 1 - y : y;
        for (std::uint32_t x = 0; x < rrw; ++x) {
            const std::uint32_t sx = (dir & 1u) ? rrw - 1 - x : x;
            write_pixel(dpsm, dbp, dbw, (dsax + x) & 0x7FFu, (dsay + y) & 0x7FFu, pixels[sy * rrw + sx]);
        }
    }
}

void load_clut(std::uint64_t tex0) {
    const std::uint32_t cld = static_cast<std::uint32_t>(tex0 >> 61) & 7u;
    static std::uint32_t cbp0 = ~0u, cbp1 = ~0u;
    const std::uint32_t cbp = static_cast<std::uint32_t>(tex0 >> 37) & 0x3FFFu;
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: cbp0 = cbp; break;
    case 3: cbp1 = cbp; break;
    case 4: if (cbp0 == cbp) return; cbp0 = cbp; break;
    case 5: if (cbp1 == cbp) return; cbp1 = cbp; break;
    default: return;
    }
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t csm = static_cast<std::uint32_t>(tex0 >> 55) & 1u;
    const std::uint32_t csa = static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu;
#ifdef PS2_GPU
    if (gpu_on()) gpu::load_clut(tex0, g_state.texclut);
#endif
    if (!cpu_raster() && !hd_enabled()) return;
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const std::uint32_t entries = eight ? 256u : 16u;
    const std::uint32_t texclut = static_cast<std::uint32_t>(g_state.texclut);
    for (std::uint32_t i = 0; i < entries; ++i) {
        std::uint32_t x, y;
        if (csm) {
            const std::uint32_t cbw = texclut & 0x3Fu, cou = (texclut >> 6) & 0x3Fu, cov = (texclut >> 12) & 0x3FFu;
            x = cou * 16 + i;
            y = cov;
            const std::uint32_t c = read_pixel(cpsm == 0 ? PSMCT32 : cpsm, cbp, cbw, x, y);
            const std::uint32_t slot = (csa * 16 + i) & 511u;
            g_state.clut[slot] = c;
            continue;
        }
        if (eight) {
            const std::uint32_t j = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
            x = j & 15u;
            y = j >> 4;
        } else {
            x = i & 7u;
            y = i >> 3;
        }
        const std::uint32_t c = read_pixel(cpsm == 0 ? PSMCT32 : cpsm, cbp, 1, x, y);
        const std::uint32_t slot = eight ? i : ((csa & 15u) * 16u + i) & 255u;
        g_state.clut[slot] = c;
    }
}

}

std::uint8_t *memory() { return g_memory; }

void swizzle_tables(std::vector<std::uint32_t> &out) {
    out.clear();
    for (auto &row : g_off32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off8) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off4) out.insert(out.end(), std::begin(row), std::end(row));
}

std::uint32_t block_base(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    switch (psm) {
    case PSMCT16: return wrap(page_address(bp, bw, x, y, 64, 64) + g_off16[y & 63][x & 63]) & ~255u;
    case PSMCT16S: return wrap(page_address(bp, bw, x, y, 64, 64) + g_off16s[y & 63][x & 63]) & ~255u;
    case PSMT8: return wrap(page_address(bp, bw, x, y, 128, 64) + g_off8[y & 63][x & 127]) & ~255u;
    case PSMT4: return wrap(page_address(bp, bw, x, y, 128, 128) + g_off4[y & 127][x & 127] / 2) & ~255u;
    default: return wrap(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) & ~255u;
    }
}

namespace {
// Validation/debug: with PS2_HD_NAMES=<file of PCSX2 replacement names> and the CPU rasterizer,
// hash every textured draw like PCSX2 and log which ones a replacement exists for.
struct HdLog {
    bool on = false;
    std::unordered_set<hd::Key, hd::KeyHash> names;
    std::unordered_set<hd::Key, hd::KeyHash> seen;
    std::uint64_t hits = 0, misses = 0, skipped = 0;
};
HdLog g_hdlog;

void hd_palette(std::uint64_t tex0, std::uint64_t texa, std::uint32_t *pal) {
    const std::uint32_t psm = (tex0 >> 20) & 0x3Fu, cpsm = (tex0 >> 51) & 0xFu, csm = (tex0 >> 55) & 1u, csa = (tex0 >> 56) & 0x1Fu;
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const std::uint32_t n = eight ? 256u : 16u;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t slot = eight ? i : csm ? ((csa * 16 + i) & 511u) : (((csa & 15u) * 16u + i) & 255u);
        const std::uint32_t raw = g_state.clut[slot];
        pal[i] = (cpsm == PSMCT16 || cpsm == PSMCT16S) ? hd::expand16(raw & 0xFFFFu, texa) : raw;
    }
}

void hd_log_draw(const State &s) {
    const std::uint64_t mode = (s.prmodecont & 1u) ? s.prim : s.prmode;
    if (!((mode >> 4) & 1u)) return;
    const Context &c = s.ctx[(mode >> 9) & 1u];
    const std::uint64_t tex0 = c.tex0;
    const std::uint32_t psm = (tex0 >> 20) & 0x3Fu, tbp = tex0 & 0x3FFFu, tbw = (tex0 >> 14) & 0x3Fu;
    std::uint32_t pal[256] = {};
    hd_palette(tex0, s.texa, pal);
    std::uint32_t bsx = 8, bsy = 8;
    if (psm == PSMT4) bsx = 32, bsy = 16;
    else if (psm == PSMT8) bsx = 16, bsy = 16;
    else if (psm == PSMCT16 || psm == PSMCT16S) bsx = 16, bsy = 8;
    hd::Key k;
    const bool ok = hd::compute(tex0, s.texa, c.clamp,
        [&](std::uint32_t bx, std::uint32_t by) -> const std::uint8_t * { return g_memory + block_base(psm, tbp, tbw, bx * bsx, by * bsy); },
        [&](std::uint32_t x, std::uint32_t y) { return read_pixel(psm, tbp, tbw, x, y); }, pal, k);
    if (!ok) { ++g_hdlog.skipped; return; }
    const bool hit = g_hdlog.names.count(k) > 0;
    if (hit) ++g_hdlog.hits; else ++g_hdlog.misses;
    if (g_hdlog.seen.insert(k).second) std::fprintf(stderr, "[hd] %s %s.png\n", hit ? "HIT " : "miss", hd::name(k).c_str());
}
}

void hd_log_init() {
    const char *path = std::getenv("PS2_HD_NAMES");
    if (!path) return;
    std::FILE *f = std::fopen(path, "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        hd::Key k;
        std::string n(line);
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
        if (hd::parse(n, k)) g_hdlog.names.insert(k);
    }
    std::fclose(f);
    g_hdlog.on = true;
    std::fprintf(stderr, "[hd] %zu replacement names loaded\n", g_hdlog.names.size());
}

void hd_log_summary() {
    if (g_hdlog.on) std::fprintf(stderr, "[hd] draws: %llu hit, %llu miss, %llu skipped (region); unique keys %zu\n", (unsigned long long)g_hdlog.hits,
                                 (unsigned long long)g_hdlog.misses, (unsigned long long)g_hdlog.skipped, g_hdlog.seen.size());
}

// ===== HD texture replacement (GPU renderer) =====
// The GPU renderer keeps GS memory on the GPU only, so when HD textures are on we also mirror
// host->GS uploads into g_memory (and keep the CPU CLUT) purely to hash textures like PCSX2.
// Hashes are cached per texture and invalidated by per-page generations bumped on uploads.
namespace {
struct HdSlot {
    enum State : std::uint8_t { kNone, kRequested, kLoaded } state = kNone;
    std::uint32_t offset = 0, words = 0, shift = 0, id = ~0u;
};
struct HdTexCache {
    std::uint64_t hash = 0;
    std::uint32_t stamp = 0, page_lo = 0, page_hi = 0;
};
struct Hd {
    bool on = false;
    std::unordered_set<hd::Key, hd::KeyHash> names;
    std::unordered_map<hd::Key, HdSlot, hd::KeyHash> slots;
    std::vector<hd::Key> by_id;
    std::unordered_map<std::uint64_t, HdTexCache> tex;
    std::uint32_t page_gen[512] = {};
    std::uint64_t page_writer[512] = {};  // which upload last wrote each 8 KB page (0 = something else)
    std::uint32_t gen = 1, clut_gen = 1;
    std::uint64_t clut_tag = ~0ull, clut_hash = 0;
    std::uint32_t capacity = 0, cursor = 0;
    std::uint64_t lookups = 0, hits = 0, loaded = 0;
    std::function<void(const std::string &, std::uint32_t, std::uint32_t, std::uint32_t)> request;
    std::function<bool(std::uint32_t, const std::uint8_t *, std::size_t)> upload;
};
Hd g_hd;

void hd_touch(std::uint32_t first, std::uint32_t end) {
    if (!g_hd.on) return;
    ++g_hd.gen;
    for (std::uint32_t a = first & ~8191u; a < end && a < kMemorySize; a += 8192) {
        g_hd.page_gen[(a >> 13) & 511u] = g_hd.gen;
        g_hd.page_writer[(a >> 13) & 511u] = 0;
    }
}

void hd_touch_transfer_impl(std::uint64_t buf, std::uint64_t pos, std::uint64_t reg, bool dest) {
    if (!g_hd.on) return;
    const std::uint32_t bp = dest ? static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu : static_cast<std::uint32_t>(buf) & 0x3FFFu;
    const std::uint32_t bw = dest ? static_cast<std::uint32_t>(buf >> 48) & 0x3Fu : static_cast<std::uint32_t>(buf >> 16) & 0x3Fu;
    const std::uint32_t psm = dest ? static_cast<std::uint32_t>(buf >> 56) & 0x3Fu : static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
    const std::uint32_t x = dest ? static_cast<std::uint32_t>(pos >> 32) & 0x7FFu : static_cast<std::uint32_t>(pos) & 0x7FFu;
    const std::uint32_t y = dest ? static_cast<std::uint32_t>(pos >> 48) & 0x7FFu : static_cast<std::uint32_t>(pos >> 16) & 0x7FFu;
    const std::uint32_t w = static_cast<std::uint32_t>(reg) & 0xFFFu, h = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!w || !h) return;
    const bool half = bits_per_pixel(psm) == 16;
    const std::uint32_t pw = psm == PSMT8 || psm == PSMT4 ? 128 : 64, ph = psm == PSMT4 ? 128 : psm == PSMT8 || half ? 64 : 32;
    const std::uint32_t first = page_address(bp, bw, x, y, pw, ph);
    const std::uint32_t end = page_address(bp, bw, x + w - 1, y + h - 1, pw, ph) + 8192;
    if (end > kMemorySize || first >= end) hd_touch(0, kMemorySize);
    else hd_touch(first, end);
}

std::uint32_t hd_alloc(std::uint32_t words) {
    if (words > g_hd.capacity) return ~0u;
    if (g_hd.cursor + words > g_hd.capacity) g_hd.cursor = 0;
    const std::uint32_t lo = g_hd.cursor, hi = lo + words;
    for (auto &[key, slot] : g_hd.slots)
        if (slot.state == HdSlot::kLoaded && slot.offset < hi && slot.offset + slot.words > lo) slot.state = HdSlot::kNone;
    g_hd.cursor = hi;
    return lo;
}
}

bool hd_enabled() { return g_hd.on; }

namespace {
std::unordered_map<std::uint64_t, std::uint64_t> g_upload_print;
}

std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::pair<std::uint64_t, std::uint64_t>> g_upload_hist;
void hd_upload_hist_dump() {
    std::vector<std::pair<std::uint64_t, std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>> v;
    for (auto &[k, c] : g_upload_hist) v.push_back({c.second, k});
    std::sort(v.rbegin(), v.rend());
    for (std::size_t i = 0; i < v.size() && i < 15; ++i)
        std::fprintf(stderr, "[hd] uploads psm %02X %ux%u: %llu bytes, %llu times\n", std::get<0>(v[i].second), std::get<1>(v[i].second), std::get<2>(v[i].second),
                     (unsigned long long)v[i].first, (unsigned long long)g_upload_hist[v[i].second].first);
}

bool hd_upload_finish(std::uint64_t key, const std::uint8_t *data, std::size_t bytes) {
    const State &s = g_state;
    {
        auto &h = g_upload_hist[{static_cast<std::uint32_t>(s.bitbltbuf >> 56) & 0x3Fu, static_cast<std::uint32_t>(s.trxreg) & 0xFFFu, static_cast<std::uint32_t>(s.trxreg >> 32) & 0xFFFu}];
        h.first++;
        h.second += bytes;
    }
    const std::uint64_t print = hd::fingerprint(data, bytes);
    const std::uint64_t buf = s.bitbltbuf, pos = s.trxpos, reg = s.trxreg;
    const std::uint32_t bp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, bw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    const std::uint32_t psm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint32_t x = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, y = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t w = static_cast<std::uint32_t>(reg) & 0xFFFu, h = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    const bool half = bits_per_pixel(psm) == 16;
    const std::uint32_t pw = psm == PSMT8 || psm == PSMT4 ? 128 : 64, ph = psm == PSMT4 ? 128 : psm == PSMT8 || half ? 64 : 32;
    std::uint32_t first = page_address(bp, bw, x, y, pw, ph), end = page_address(bp, bw, x + w - 1, y + h - 1, pw, ph) + 8192;
    if (end > kMemorySize || first >= end) first = 0, end = kMemorySize;
    auto it = g_upload_print.find(key);
    bool same = it != g_upload_print.end() && it->second == print;
    for (std::uint32_t a = first; same && a < end; a += 8192) same = g_hd.page_writer[(a >> 13) & 511u] == key;
#ifdef PS2_GPU
    if (same && gpu::rendered_pages(first, end)) same = false;
#endif
    if (same) return true;
    g_upload_print[key] = print;
    if (g_upload_print.size() > 4096) g_upload_print.clear();
    hd_touch(first, end);
    for (std::uint32_t a = first; a < end; a += 8192) g_hd.page_writer[(a >> 13) & 511u] = key;
    return false;
}
void hd_touch_transfer(std::uint64_t buf, std::uint64_t pos, std::uint64_t reg, bool dest) { hd_touch_transfer_impl(buf, pos, reg, dest); }

void hd_configure(std::uint32_t capacity_words, std::function<void(const std::string &, std::uint32_t, std::uint32_t, std::uint32_t)> request,
                  std::function<bool(std::uint32_t, const std::uint8_t *, std::size_t)> upload) {
    g_hd.capacity = capacity_words;
    g_hd.request = std::move(request);
    g_hd.upload = std::move(upload);
}

std::size_t hd_add_names(const char *text, std::size_t length) {
    std::size_t added = 0, at = 0;
    while (at < length) {
        std::size_t end = at;
        while (end < length && text[end] != '\n' && text[end] != '\r') ++end;
        hd::Key k;
        if (end > at && hd::parse(std::string(text + at, end - at), k)) added += g_hd.names.insert(k).second ? 1 : 0;
        at = end + 1;
    }
    g_hd.on = !g_hd.names.empty() && g_hd.capacity > 0;
    return added;
}

void hd_loaded(std::uint32_t id, const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, std::uint32_t shift) {
    if (id >= g_hd.by_id.size()) return;
    auto it = g_hd.slots.find(g_hd.by_id[id]);
    if (it == g_hd.slots.end()) return;
    HdSlot &slot = it->second;
    if (!rgba || !width || !height) {
        slot.state = HdSlot::kNone;  // stays native; retried if the texture comes back much later
        return;
    }
    const std::uint32_t words = width * height;
    const std::uint32_t offset = hd_alloc(words);
    if (offset == ~0u || !g_hd.upload || !g_hd.upload(offset, rgba, static_cast<std::size_t>(words) * 4)) {
        slot.state = HdSlot::kNone;
        return;
    }
    slot.state = HdSlot::kLoaded;
    slot.offset = offset;
    slot.words = words;
    slot.shift = shift;
    ++g_hd.loaded;
}

bool hd_lookup(const State &s, const Context &c, std::uint32_t &offset, std::uint32_t &shift) {
    if (!g_hd.on) return false;
    ++g_hd.lookups;
    const std::uint64_t tex0 = c.tex0;
    const std::uint32_t psm = (tex0 >> 20) & 0x3Fu, tbp = tex0 & 0x3FFFu, tbw = (tex0 >> 14) & 0x3Fu;
    const std::uint32_t tw = (tex0 >> 26) & 15u, th = (tex0 >> 30) & 15u;
    if (tw > 10 || th > 10) return false;
    const bool direct16_24 = psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S;
    // texture hash cache (content of GS memory), keyed by where/how the texture is read
    const std::uint64_t texkey = (tex0 & 0x3FFFFFFFFull) ^ ((c.clamp & 0xFFFFFFFFFFFull) * 0x9E3779B97F4A7C15ull) ^ (direct16_24 ? (s.texa * 0xC2B2AE3D27D4EB4Full) : 0);
    std::uint32_t bsx = 8, bsy = 8, pw = 64, ph = 32;
    if (psm == PSMT4) bsx = 32, bsy = 16, pw = 128, ph = 128;
    else if (psm == PSMT8) bsx = 16, bsy = 16, pw = 128, ph = 64;
    else if (psm == PSMCT16 || psm == PSMCT16S) bsx = 16, bsy = 8, ph = 64;
    HdTexCache &tc = g_hd.tex[texkey];
    bool valid = tc.stamp != 0;
    for (std::uint32_t p = tc.page_lo; valid && p <= tc.page_hi; ++p) valid = g_hd.page_gen[p & 511u] <= tc.stamp;
    std::uint32_t pal[256] = {};
    const bool palettized = psm == PSMT4 || psm == PSMT8 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
    if (palettized) hd_palette(tex0, s.texa, pal);
    hd::Key k;
    if (!valid) {
        if (!hd::compute(tex0, s.texa, c.clamp,
                         [&](std::uint32_t bx, std::uint32_t by) -> const std::uint8_t * { return g_memory + block_base(psm, tbp, tbw, bx * bsx, by * bsy); },
                         [&](std::uint32_t x, std::uint32_t y) { return read_pixel(psm, tbp, tbw, x, y); }, pal, k)) {
            tc.stamp = ~0u;  // region textures: never valid for HD, don't rehash
            tc.hash = 0;
            tc.page_lo = 1;
            tc.page_hi = 0;
            return false;
        }
        tc.hash = k.tex;
        tc.stamp = g_hd.gen;
        const std::uint32_t lo = page_address(tbp, tbw, 0, 0, pw, ph), hi = page_address(tbp, tbw, (1u << tw) - 1, (1u << th) - 1, pw, ph);
        tc.page_lo = (lo >> 13) & 511u;
        tc.page_hi = std::max(tc.page_lo, std::min<std::uint32_t>((hi >> 13), tc.page_lo + 511u));
    } else {
        if (tc.stamp == ~0u) return false;
        k.bits = hd::bits(tex0, s.texa);
        k.tex = tc.hash;
        k.clut = palettized ? hd::palette_hash(pal, (psm == PSMT8 || psm == PSMT8H) ? 256u : 16u) : 0;
    }
#ifdef PS2_GPU
    if (gpu::rendered_pages(tc.page_lo * 8192u, (tc.page_hi + 1u) * 8192u)) return false;
#endif
    auto found = g_hd.names.find(k);
    if (found == g_hd.names.end()) return false;
    HdSlot &slot = g_hd.slots[k];
    if (slot.state == HdSlot::kLoaded) {
        offset = slot.offset;
        shift = slot.shift;
        ++g_hd.hits;
        return true;
    }
    if (slot.state == HdSlot::kNone && g_hd.request) {
        if (slot.id == ~0u) {
            slot.id = static_cast<std::uint32_t>(g_hd.by_id.size());
            g_hd.by_id.push_back(k);
        }
        slot.state = HdSlot::kRequested;
        g_hd.request(hd::name(k) + ".png", slot.id, 1u << tw, 1u << th);
    }
    return false;
}

void hd_stats() {
    hd_upload_hist_dump();
    if (g_hd.on) std::fprintf(stderr, "[hd] %zu names, %llu lookups, %llu HD draws, %llu loaded, %llu bytes mirrored, %llu bytes skipped as repeats\n", g_hd.names.size(), (unsigned long long)g_hd.lookups,
                              (unsigned long long)g_hd.hits, (unsigned long long)g_hd.loaded, (unsigned long long)g_mirror_bytes, (unsigned long long)g_mirror_skipped);
}

std::uint32_t address32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    return wrap(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
}

std::uint32_t read_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    switch (psm) {
    case PSMCT32: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
    case PSMCT24: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) & 0xFFFFFFu;
    case PSMZ32: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63]);
    case PSMZ24: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63]) & 0xFFFFFFu;
    case PSMCT16: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16[y & 63][x & 63]);
    case PSMCT16S: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16s[y & 63][x & 63]);
    case PSMZ16: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16[y & 63][x & 63]);
    case PSMZ16S: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16s[y & 63][x & 63]);
    case PSMT8: return get<std::uint8_t>(page_address(bp, bw, x, y, 128, 64) + g_off8[y & 63][x & 127]);
    case PSMT4: {
        const std::uint32_t n = g_off4[y & 127][x & 127];
        const std::uint8_t b = get<std::uint8_t>(page_address(bp, bw, x, y, 128, 128) + n / 2);
        return (n & 1u) ? b >> 4 : b & 15u;
    }
    case PSMT8H: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 24;
    case PSMT4HL: return (get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 24) & 15u;
    case PSMT4HH: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 28;
    default: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
    }
}

void write_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t value) {
    switch (psm) {
    case PSMCT32: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63], value); break;
    case PSMZ32: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63], value); break;
    case PSMCT24: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xFF000000u) | (value & 0xFFFFFFu));
        break;
    }
    case PSMZ24: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xFF000000u) | (value & 0xFFFFFFu));
        break;
    }
    case PSMCT16: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMCT16S: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16s[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMZ16: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMZ16S: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16s[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMT8: put<std::uint8_t>(page_address(bp, bw, x, y, 128, 64) + g_off8[y & 63][x & 127], static_cast<std::uint8_t>(value)); break;
    case PSMT4: {
        const std::uint32_t n = g_off4[y & 127][x & 127];
        const std::uint32_t a = page_address(bp, bw, x, y, 128, 128) + n / 2;
        std::uint8_t b = get<std::uint8_t>(a);
        b = (n & 1u) ? static_cast<std::uint8_t>((b & 0x0Fu) | ((value & 15u) << 4)) : static_cast<std::uint8_t>((b & 0xF0u) | (value & 15u));
        put<std::uint8_t>(a, b);
        break;
    }
    case PSMT8H: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0x00FFFFFFu) | (value << 24));
        break;
    }
    case PSMT4HL: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xF0FFFFFFu) | ((value & 15u) << 24));
        break;
    }
    case PSMT4HH: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0x0FFFFFFFu) | ((value & 15u) << 28));
        break;
    }
    default: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63], value); break;
    }
}

void init() {
    static bool built = false;
    if (built) return;
    build_tables();
    built = true;
    hd_log_init();
#ifdef PS2_GPU
    const char *backend = std::getenv("PS2_GS");
    const gpu::Mode wanted = !backend || std::strcmp(backend, "gpu") == 0 ? gpu::Mode::primary : std::strcmp(backend, "both") == 0 ? gpu::Mode::mirror : gpu::Mode::off;
    if (wanted != gpu::Mode::off && !gpu::init(wanted)) std::fprintf(stderr, "[gs] WebGPU unavailable, using the software renderer\n");
#endif
}

void render_reset() {
#ifdef PS2_GPU
    g_seen_ranges.clear();
    g_transfer_stale = false;
#endif
    init();
    std::memset(&g_state, 0, sizeof g_state);
    std::memset(g_memory, 0, sizeof g_memory);
    ++g_setup_generation;
    g_transfer = Transfer{};
    g_primitives = 0;
    raster::reset();
}

// A reset through CSR clears the drawing environment, not just the vertex queue,
// as PCSX2 does: Downhill Domination's movie player leaves SCANMSK set for it.
void soft_reset() {
    std::uint32_t clut[512];
    std::memcpy(clut, g_state.clut, sizeof clut);
    std::memset(&g_state, 0, sizeof g_state);
    std::memcpy(g_state.clut, clut, sizeof clut);
    ++g_setup_generation;
}

void reset() {
    g_shadow = DownloadKey{};
    {
        std::lock_guard lock(g_cache_mutex);
        g_download_cache.clear();
    }
    g_csr = 0x551B4000ull;
    g_imr = 0x7F00ull;
    if (queue::active()) queue::post(queue::kReset, 0, 0, nullptr, 0);
    else render_reset();
}

namespace {
std::string hd_draw_name(const State &s, const Context &c) {
    const std::uint64_t tex0 = c.tex0;
    const std::uint32_t psm = (tex0 >> 20) & 0x3Fu, tbp = tex0 & 0x3FFFu, tbw = (tex0 >> 14) & 0x3Fu;
    std::uint32_t pal[256] = {};
    hd_palette(tex0, s.texa, pal);
    std::uint32_t bsx = 8, bsy = 8;
    if (psm == PSMT4) bsx = 32, bsy = 16;
    else if (psm == PSMT8) bsx = 16, bsy = 16;
    else if (psm == PSMCT16 || psm == PSMCT16S) bsx = 16, bsy = 8;
    hd::Key k;
    if (!hd::compute(tex0, s.texa, c.clamp,
            [&](std::uint32_t bx, std::uint32_t by) -> const std::uint8_t * { return g_memory + block_base(psm, tbp, tbw, bx * bsx, by * bsy); },
            [&](std::uint32_t x, std::uint32_t y) { return read_pixel(psm, tbp, tbw, x, y); }, pal, k))
        return "region";
    return hd::name(k);
}
}

namespace {
std::atomic<bool> g_trace_draws{false};
struct TraceRun { std::uint64_t prim = ~0ull, frame, tex0, alpha, test, xyoff; std::uint32_t n = 0, x0, y0, x1, y1, rgba, u0, v0, u1, v1; std::string name; std::uint32_t fx = 0; } g_trun;
void trace_flush() {
    TraceRun &t = g_trun;
    if (!t.n) return;
    std::fprintf(stderr, "[draw] n=%u prim=%03llX fbp=%llX fbw=%llu psm=%llX fbmsk=%08llX tex0=%016llX alpha=%010llX test=%05llX box=%u,%u-%u,%u rgba=%08X uv=%u,%u-%u,%u fx=%u tex=%s\n", t.n,
                 (unsigned long long)t.prim, (unsigned long long)(t.frame & 0x1FF), (unsigned long long)((t.frame >> 16) & 63), (unsigned long long)((t.frame >> 24) & 63),
                 (unsigned long long)(t.frame >> 32), (unsigned long long)t.tex0, (unsigned long long)t.alpha, (unsigned long long)t.test, t.x0, t.y0, t.x1, t.y1, t.rgba, t.u0, t.v0, t.u1, t.v1, t.fx, t.name.empty() ? "-" : t.name.c_str());
    t.n = 0;
}
void trace_draw(const State &s, int needed) {
    const std::uint32_t c = (s.prim >> 9) & 1u;
    const Context &ctx = s.ctx[c];
    const std::uint64_t prim = s.prim & 0x7FFu;
    TraceRun &t = g_trun;
    if (!(t.n && t.prim == prim && t.frame == ctx.frame && t.tex0 == ctx.tex0 && t.alpha == ctx.alpha && t.test == ctx.test)) {
        trace_flush();
        t = TraceRun{prim, ctx.frame, ctx.tex0, ctx.alpha, ctx.test, ctx.xyoffset, 0, ~0u, ~0u, 0, 0, 0, ~0u, ~0u, 0, 0, {}};
        const Vertex &v = s.queue[0];
        t.rgba = v.r | (v.g << 8) | (v.b << 16) | (std::uint32_t(v.a) << 24);
        t.name.clear();
        if (((prim >> 4) & 1u) && (hd_enabled() || cpu_raster())) t.name = hd_draw_name(s, ctx);
    }
    const std::uint32_t ox = ctx.xyoffset & 0xFFFFu, oy = (ctx.xyoffset >> 32) & 0xFFFFu;
    for (int i = 0; i < needed; ++i) {
        const Vertex &v = s.queue[i];
        const std::uint32_t x = (v.x - ox) >> 4, y = (v.y - oy) >> 4;
        t.x0 = std::min(t.x0, x); t.y0 = std::min(t.y0, y); t.x1 = std::max(t.x1, x); t.y1 = std::max(t.y1, y);
        std::uint32_t u = v.u >> 4, vv = v.v >> 4;
        if (!((prim >> 8) & 1u)) { const float q = v.q ? v.q : 1.f; const std::uint32_t tw = 1u << ((ctx.tex0 >> 26) & 15u), th = 1u << ((ctx.tex0 >> 30) & 15u); u = std::uint32_t(v.s / q * tw); vv = std::uint32_t(v.t / q * th); }
        t.u0 = std::min(t.u0, u); t.v0 = std::min(t.v0, vv); t.u1 = std::max(t.u1, u); t.v1 = std::max(t.v1, vv);
    }
    ++t.n;
}
}

void trace_draws(bool on) {
    if (!on) trace_flush();
    std::fprintf(stderr, on ? "[draw] ---- frame start\n" : "[draw] ---- frame end\n");
    g_trace_draws = on;
}

// Game post-effect switches. ps2::video_options() bits 8..15 = settings, 16..23 = debug hotkeys;
// a draw is dropped when the bit for its class is set. Classes:
// 1 bloom composite, 2 Z-greater projected light/shadow, 3 wide untextured blended overlay,
// 4 textured read of a recent render target (copies, blur, bloom chain), 5 palette read of a render target.
std::uint32_t g_targets[8] = {};
std::uint32_t g_target_next = 0;
void note_target(std::uint64_t frame) {
    const std::uint32_t base = static_cast<std::uint32_t>(frame & 0x1FFu) * 32u;
    for (std::uint32_t t : g_targets) if (t == base + 1u) return;
    g_targets[g_target_next++ & 7u] = base + 1u;
}
bool is_target(std::uint32_t tbp) {
    for (std::uint32_t t : g_targets) if (t == tbp + 1u) return true;
    return false;
}
std::uint32_t fx_kind(const State &s, int needed) {
    const std::uint64_t prim = s.prim;
    const Context &ctx = s.ctx[(prim >> 9) & 1u];
    const bool textured = (prim >> 4) & 1u, blended = (prim >> 6) & 1u;
    const std::uint32_t type = prim & 7u;
    const std::uint32_t fbw = ((ctx.frame >> 16) & 63u) * 64u;
    if (textured) {
        const std::uint32_t tbp = ctx.tex0 & 0x3FFFu, psm = (ctx.tex0 >> 20) & 0x3Fu;
        if (blended && type == 6 && (ctx.alpha & 0xFFu) == 0x68u && ((ctx.alpha >> 32) & 0xFFu) >= 0x80u && needed == 2) {
            const std::uint32_t ox = ctx.xyoffset & 0xFFFFu;
            const std::uint32_t x0 = (s.queue[0].x - ox) >> 4, x1 = (s.queue[1].x - ox) >> 4;
            const std::uint32_t w = x1 > x0 ? x1 - x0 : x0 - x1;
            const std::uint32_t tbw = ((ctx.tex0 >> 14) & 63u) * 64u;
            if (w + 8 >= fbw && tbw < fbw) return 1;
        }
        if (blended && ((ctx.test >> 16) & 7u) == 7u) return 2;
        if (is_target(tbp)) return (psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C) ? 5 : 4;
        return 0;
    }
    if (blended && type == 6 && needed == 2 && fbw) {
        const std::uint32_t ox = ctx.xyoffset & 0xFFFFu;
        const std::uint32_t x0 = (s.queue[0].x - ox) >> 4, x1 = (s.queue[1].x - ox) >> 4;
        if ((x1 > x0 ? x1 - x0 : x0 - x1) * 4 >= fbw * 3) return 3;
    }
    return 0;
}
std::uint32_t fx_off() {
    static const std::uint32_t env = [] { const char *e = std::getenv("PS2_FX_OFF"); return e ? static_cast<std::uint32_t>(std::strtoul(e, nullptr, 0)) : 0u; }();
    const std::uint32_t v = ps2::video_options();
    return env | ((v >> 8) & 0xFFu) | ((v >> 16) & 0xFFu);
}

// ===== Widescreen helpers =====
// The 3D world is widened by the FOV patch, but the 2D HUD is drawn for 4:3 and gets stretched.
// HUD = everything drawn after the bloom composite until the next 3D draw. Each HUD element is
// squeezed back to its true shape (x * k) and pinned to its edge: left / centre / right, chosen
// per run of same-texture primitives from where the run starts. Frames with no 3D at all
// (menus, loading, movies) are shown 4:3 with black sides instead of stretched.
std::atomic<std::uint32_t> g_3d_draws{0};
bool g_hud_phase = false;
struct HudRun { std::uint64_t key = ~0ull; float minx = 0, maxx = 0, anchor = 320; } g_hud_run;

float hud_k() {
    const float a = ps2::g_options.aspect;
    return a > 1.34f ? (4.0f / 3.0f) / a : 1.0f;
}
// widescreen HUD squeeze switched off in v1.2 (it broke the game); kept for later
bool hud_fix_on() {
    static const bool env_off = std::getenv("PS2_HUDFIX_OFF") != nullptr;
    return ps2::g_options.aspect > 1.34f && !env_off && !(ps2::video_options() & (1u << 30));
}

std::uint64_t g_hud_fbp = ~0ull;
std::atomic<bool> g_saw_composite{false};
void hud_track(const State &s, int needed, std::uint32_t kind) {
    const Context &ctx = s.ctx[(s.prim >> 9) & 1u];
    const std::uint32_t type = s.prim & 7u;
    if (kind == 1) {
        g_hud_phase = true;
        g_hud_fbp = ctx.frame & 0x1FFu;
        g_hud_run.key = ~0ull;
        g_saw_composite.store(true, std::memory_order_relaxed);
        return;
    }
    // the HUD ends when the game starts drawing into another buffer (next frame's copy / clear)
    if (g_hud_phase && (ctx.frame & 0x1FFu) != g_hud_fbp) g_hud_phase = false;
    const bool zwrite = ((ctx.test >> 16) & 1u) && !((ctx.zbuf >> 32) & 1u);
    if (!g_hud_phase && zwrite && (type == 3 || type == 4 || type == 5)) g_3d_draws.fetch_add(1, std::memory_order_relaxed);
    (void)needed;
}

// returns false if this primitive should be drawn as-is
bool hud_squeeze(const State &s, int needed, Vertex *out) {
    if (!g_hud_phase) return false;
    const Context &ctx = s.ctx[(s.prim >> 9) & 1u];
    const std::uint32_t fbw = ((ctx.frame >> 16) & 63u) * 64u;
    if (fbw < 512) return false;  // offscreen buffers
    const float ox = static_cast<float>(ctx.xyoffset & 0xFFFFu);
    float x0 = 1e9f, x1 = -1e9f;
    for (int i = 0; i < needed; ++i) {
        const float x = (static_cast<float>(s.queue[i].x) - ox) / 16.0f;
        x0 = std::min(x0, x); x1 = std::max(x1, x);
    }
    if (x1 - x0 >= 560.0f) return false;  // full-screen backgrounds / fades stay full width
    const std::uint64_t key = ctx.tex0 ^ (s.prim & 0x3FFu) * 0x9E3779B97F4A7C15ull;
    HudRun &r = g_hud_run;
    if (key != r.key || x0 > r.maxx + 48.0f || x1 < r.minx - 48.0f) {
        const float c = (x0 + x1) * 0.5f;
        r.key = key; r.minx = x0; r.maxx = x1;
        r.anchor = c < 160.0f ? 0.0f : c > 480.0f ? 640.0f : 320.0f;
    } else {
        r.minx = std::min(r.minx, x0); r.maxx = std::max(r.maxx, x1);
    }
    const float k = hud_k();
    for (int i = 0; i < needed; ++i) {
        out[i] = s.queue[i];
        const float x = (static_cast<float>(s.queue[i].x) - ox) / 16.0f;
        const float nx = r.anchor + (x - r.anchor) * k;
        out[i].x = static_cast<std::uint32_t>(std::max(0.0f, ox + nx * 16.0f + 0.5f));
    }
    return true;
}

void kick(bool draw) {
    State &s = g_state;
    const std::uint32_t prim = static_cast<std::uint32_t>(s.prim) & 7u;
    const int needed = prim == 0 ? 1 : (prim == 1 || prim == 2 || prim == 6) ? 2 : prim == 7 ? 1 : 3;
    if (s.count < needed) return;
    const Vertex *verts = s.queue;
    Vertex squeezed[3];
    if (draw) {
        const bool tracing = g_trace_draws.load(std::memory_order_relaxed);
        const bool wide = hud_fix_on();
        if (const std::uint32_t off = fx_off(); off || tracing || wide) {
            const std::uint32_t k = fx_kind(s, needed);
            if (tracing) [[unlikely]] { trace_draw(s, needed); g_trun.fx = k; }
            if (wide) {
                hud_track(s, needed, k);
                if (k != 1 && hud_squeeze(s, needed, squeezed)) verts = squeezed;
            }
            if (k && (off >> (k - 1)) & 1u) draw = false;
        } else {
            // still count 3D for the menu pillarbox
            const Context &ctx = s.ctx[(s.prim >> 9) & 1u];
            const std::uint32_t type = s.prim & 7u;
            if (((ctx.test >> 16) & 1u) && !((ctx.zbuf >> 32) & 1u) && (type == 3 || type == 4 || type == 5)) g_3d_draws.fetch_add(1, std::memory_order_relaxed);
            if (fx_kind(s, needed) == 1) g_saw_composite.store(true, std::memory_order_relaxed);
        }
    }
    if (draw) {
        note_target(s.ctx[(s.prim >> 9) & 1u].frame);
        if (cpu_raster()) {
            if (g_hdlog.on) hd_log_draw(s);
            raster::draw(s, verts, needed);
        }
#ifdef PS2_GPU
        if (gpu_on()) gpu::draw(s, verts, needed);
#endif
        ++g_primitives;
    }
    switch (prim) {
    case 2:
        s.queue[0] = s.queue[1];
        s.count = 1;
        break;
    case 4:
        s.queue[0] = s.queue[1];
        s.queue[1] = s.queue[2];
        s.count = 2;
        break;
    case 5:
        s.queue[1] = s.queue[2];
        s.count = 2;
        break;
    default:
        s.count = 0;
        break;
    }
}

void vertex(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint8_t f, bool draw) {
    State &s = g_state;
    if (s.count >= 3) s.count = 2;
    Vertex &v = s.queue[s.count++];
    v.x = x;
    v.y = y;
    v.z = z;
    v.f = f;
    v.r = static_cast<std::uint8_t>(s.rgbaq);
    v.g = static_cast<std::uint8_t>(s.rgbaq >> 8);
    v.b = static_cast<std::uint8_t>(s.rgbaq >> 16);
    v.a = static_cast<std::uint8_t>(s.rgbaq >> 24);
    std::uint32_t qbits = static_cast<std::uint32_t>(s.rgbaq >> 32), sbits = static_cast<std::uint32_t>(s.st), tbits = static_cast<std::uint32_t>(s.st >> 32);
    std::memcpy(&v.q, &qbits, 4);
    std::memcpy(&v.s, &sbits, 4);
    std::memcpy(&v.t, &tbits, 4);
    v.u = static_cast<std::uint32_t>(s.uv) & 0x3FFFu;
    v.v = static_cast<std::uint32_t>(s.uv >> 16) & 0x3FFFu;
    kick(draw);
}

void write(std::uint32_t reg, std::uint64_t value) {
    State &s = g_state;
    ++g_writes[reg & 127u];
    constexpr std::uint64_t kVertexRegs = (1ull << 0x01) | (1ull << 0x02) | (1ull << 0x03) | (1ull << 0x04) | (1ull << 0x05) | (1ull << 0x0A) | (1ull << 0x0C) | (1ull << 0x0D);
    if (reg >= 64 || !((kVertexRegs >> reg) & 1u)) ++g_setup_generation;
    static const char *seq = std::getenv("PS2_TRACE_GSSEQ");
    if (seq) {
        static std::uint64_t n = 0, start = std::strtoull(seq, nullptr, 10);
        if (++n >= start && n < start + 100000) std::fprintf(stderr, "[gs] w %02X %016llX\n", reg, static_cast<unsigned long long>(value));
    }
    switch (reg) {
    case 0x00:
        s.prim = value;
        s.count = 0;
        break;
    case 0x01: s.rgbaq = value; break;
    case 0x02: s.st = value; break;
    case 0x03: s.uv = value; break;
    case 0x04: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32) & 0xFFFFFFu, static_cast<std::uint8_t>(value >> 56), true); break;
    case 0x05: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32), static_cast<std::uint8_t>(s.fog >> 56), true); break;
    case 0x06: case 0x07:
        s.ctx[reg - 6].tex0 = value;
        load_clut(value);
        break;
    case 0x08: case 0x09: s.ctx[reg - 8].clamp = value; break;
    case 0x0A: s.fog = value; break;
    case 0x0C: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32) & 0xFFFFFFu, static_cast<std::uint8_t>(value >> 56), false); break;
    case 0x0D: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32), static_cast<std::uint8_t>(s.fog >> 56), false); break;
    case 0x14: case 0x15: s.ctx[reg - 0x14].tex1 = value; break;
    case 0x16: case 0x17: {
        Context &c = s.ctx[reg - 0x16];
        const std::uint64_t mask = 0xFFFFFFE0'03F00000ull;
        c.tex0 = (c.tex0 & ~mask) | (value & mask);
        load_clut(c.tex0);
        break;
    }
    case 0x18: case 0x19: s.ctx[reg - 0x18].xyoffset = value; break;
    case 0x1A: s.prmodecont = value; break;
    case 0x1B: s.prmode = value; break;
    case 0x1C: s.texclut = value; break;
    case 0x22: s.scanmsk = value; break;
    case 0x34: case 0x35: s.ctx[reg - 0x34].miptbp1 = value; break;
    case 0x36: case 0x37: s.ctx[reg - 0x36].miptbp2 = value; break;
    case 0x3B: s.texa = value; break;
    case 0x3D: s.fogcol = value; break;
    case 0x3F:
#ifdef PS2_GPU
        if (gpu_on()) gpu::texflush();
#endif
        break;
    case 0x40: case 0x41: s.ctx[reg - 0x40].scissor = value; break;
    case 0x42: case 0x43: s.ctx[reg - 0x42].alpha = value; break;
    case 0x44: s.dimx = value; break;
    case 0x45: s.dthe = value; break;
    case 0x46: s.colclamp = value; break;
    case 0x47: case 0x48: s.ctx[reg - 0x47].test = value; break;
    case 0x49: s.pabe = value; break;
    case 0x4A: case 0x4B: s.ctx[reg - 0x4A].fba = value; break;
    case 0x4C: case 0x4D: s.ctx[reg - 0x4C].frame = value; break;
    case 0x4E: case 0x4F: s.ctx[reg - 0x4E].zbuf = value; break;
    case 0x50: s.bitbltbuf = value; break;
    case 0x51: s.trxpos = value; break;
    case 0x52: s.trxreg = value; break;
    case 0x53:
        if (trace_display()) {
            std::fprintf(stderr, "[gs] trx dir %llu buf 0x%016llX pos 0x%016llX reg 0x%016llX\n", static_cast<unsigned long long>(value & 3u),
                         static_cast<unsigned long long>(s.bitbltbuf), static_cast<unsigned long long>(s.trxpos), static_cast<unsigned long long>(s.trxreg));
        }
        s.trxdir = value;
        g_transfer = Transfer{};
        g_carried = 0;
#ifdef PS2_GPU
        if (gpu_on()) {
            if ((value & 3u) == 0) gpu::image_begin(s);
            else if ((value & 3u) == 2) gpu::local_copy(s);
            else if ((value & 3u) == 1 && !cpu_raster()) download_source(s);
        }
#endif
        if ((value & 3u) == 0) {
            g_transfer.active = true;
            if (cpu_raster()) hd_touch_transfer(s.bitbltbuf, s.trxpos, s.trxreg, true);
            else if (hd_enabled()) hd_begin_upload();
        } else if ((value & 3u) == 2) {
            if (cpu_raster() || hd_enabled()) local_copy();
            hd_touch_transfer(s.bitbltbuf, s.trxpos, s.trxreg, true);
        }
        else if ((value & 3u) == 1) g_transfer.active = true;
        break;
    case 0x54:
        feed_image(reinterpret_cast<const std::uint8_t *>(&value), 8);
        break;
    case 0x60: case 0x61: case 0x62:
        if (!queue::active()) signal(reg, value);
        break;
    default:
        break;
    }
}

void signal(std::uint32_t reg, std::uint64_t value) {
    const std::uint32_t mask = static_cast<std::uint32_t>(value >> 32), id = static_cast<std::uint32_t>(value);
    switch (reg) {
    case 0x60:
        g_siglblid = (g_siglblid & ~static_cast<std::uint64_t>(mask)) | (id & mask);
        raise(1);
        break;
    case 0x61: raise(2); break;
    case 0x62: {
        const std::uint64_t label = (g_siglblid >> 32 & ~static_cast<std::uint64_t>(mask)) | (id & mask);
        g_siglblid = (g_siglblid & 0xFFFFFFFFull) | (label << 32);
        break;
    }
    default: break;
    }
}

void write_packed(std::uint32_t reg, const Reg128 &q) {
    State &s = g_state;
    static const char *const seq = std::getenv("PS2_TRACE_GSSEQ");
    if (seq) [[unlikely]] {
        static std::uint64_t n = 0, start = std::strtoull(seq, nullptr, 10);
        if (++n >= start && n < start + 100000) std::fprintf(stderr, "[gs] w %02X %016llX\n", reg, static_cast<unsigned long long>(q.d[0]));
    }
    switch (reg) {
    case 0x0: write(0x00, q.d[0] & 0x7FFu); break;
    case 0x1: {
        std::uint32_t qbits;
        std::memcpy(&qbits, &s.q, 4);
        s.rgbaq = (q.w[0] & 0xFFu) | ((q.w[1] & 0xFFu) << 8) | ((q.w[2] & 0xFFu) << 16) | ((q.w[3] & 0xFFu) << 24) |
                  (static_cast<std::uint64_t>(qbits) << 32);
        break;
    }
    case 0x2:
        s.st = q.d[0];
        std::memcpy(&s.q, &q.w[2], 4);
        break;
    case 0x3: s.uv = (q.w[0] & 0x3FFFu) | ((q.w[1] & 0x3FFFu) << 16); break;
    case 0x4: {
        const std::uint64_t v = (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) |
                                (static_cast<std::uint64_t>((q.w[2] >> 4) & 0xFFFFFFu) << 32) |
                                (static_cast<std::uint64_t>((q.w[3] >> 4) & 0xFFu) << 56);
        write(((q.d[1] >> 47) & 1u) ? 0x0C : 0x04, v);
        break;
    }
    case 0x5: {
        const std::uint64_t v = (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) | (static_cast<std::uint64_t>(q.w[2]) << 32);
        write(((q.d[1] >> 47) & 1u) ? 0x0D : 0x05, v);
        break;
    }
    case 0xA: s.fog = static_cast<std::uint64_t>((q.w[3] >> 4) & 0xFFu) << 56; break;
    case 0xC: write(0x0C, (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) |
                              (static_cast<std::uint64_t>((q.w[2] >> 4) & 0xFFFFFFu) << 32) |
                              (static_cast<std::uint64_t>((q.w[3] >> 4) & 0xFFu) << 56)); break;
    case 0xD: write(0x0D, (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) | (static_cast<std::uint64_t>(q.w[2]) << 32)); break;
    case 0xE: write(static_cast<std::uint32_t>(q.d[1] & 0xFFu), q.d[0]); break;
    case 0xF: break;
    default: write(reg, q.d[0]); break;
    }
}

void write_image(const std::uint8_t *data, std::uint32_t qwords) { feed_image(data, static_cast<std::size_t>(qwords) * 16); }

std::atomic<std::uint32_t> g_downloads{0};

void read_image(std::uint8_t *data, std::uint32_t qwords) {
    if (!queue::active()) {
        render_read_image(data, qwords);
        return;
    }
    DownloadKey key = g_shadow;
    key.qwords = qwords;
    g_shadow.offset += qwords * 16;
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    {
        std::lock_guard lock(g_cache_mutex);
        const auto it = std::find_if(g_download_cache.begin(), g_download_cache.end(), [&](const CachedDownload &c) { return c.key == key; });
        if (it != g_download_cache.end() && it->data.size() == bytes) {
            std::memcpy(data, it->data.data(), bytes);
            it->used = ++g_cache_clock;
            queue::post(queue::kDownloadAsync, qwords, 0, &key, sizeof key);
            return;
        }
    }
    const std::uint32_t seen = g_downloads.load();
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(data);
    queue::post(queue::kDownload, qwords, 0, &target, sizeof target);
    while (g_downloads.load() == seen) queue::wait(g_downloads, seen);
    remember(key, data, bytes);
}

void note_transfer(std::uint32_t reg, std::uint64_t value) {
    switch (reg) {
    case 0x50: g_shadow.buf = value; break;
    case 0x51: g_shadow.pos = value; break;
    case 0x52: g_shadow.reg = value; break;
    default: g_shadow.offset = 0; break;
    }
}

void render_read_image(std::uint8_t *data, std::uint32_t qwords) {
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    std::memset(data, 0, bytes);
    if (!g_transfer.active || (g_state.trxdir & 3u) != 1) return;
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu;
    const std::uint32_t spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
    const std::uint32_t ssax = static_cast<std::uint32_t>(pos) & 0x7FFu, ssay = static_cast<std::uint32_t>(pos >> 16) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    const std::uint32_t bits = bits_per_pixel(spsm);
    auto next = [&]() {
        const std::uint32_t v = read_pixel(spsm, sbp, sbw, (ssax + g_transfer.x) & 0x7FFu, (ssay + g_transfer.y) & 0x7FFu);
        if (++g_transfer.x >= rrw) {
            g_transfer.x = 0;
            if (++g_transfer.y >= rrh) g_transfer.active = false;
        }
        return v;
    };
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && (g_transfer.active || g_carried); ++i) {
            if (!g_carried) {
                const std::uint32_t v = next();
                g_carry[0] = static_cast<std::uint8_t>(v);
                g_carry[1] = static_cast<std::uint8_t>(v >> 8);
                g_carry[2] = static_cast<std::uint8_t>(v >> 16);
                g_carried = 3;
            }
            data[i] = g_carry[3 - g_carried--];
        }
        return;
    }
    for (std::size_t bit = 0; bit + bits <= bytes * 8 && g_transfer.active; bit += bits) {
        const std::uint32_t v = next();
        if (bits == 4) data[bit / 8] |= static_cast<std::uint8_t>((v & 15u) << (bit & 4));
        else std::memcpy(data + bit / 8, &v, bits / 8);
    }
}

std::uint64_t read_privileged(std::uint32_t address) {
    switch (address & 0x1FFFu) {
    case 0x1000: return g_csr | (g_odd_field ? 0x2000u : 0u);
    case 0x1010: return g_imr;
    case 0x1040: return g_busdir;
    case 0x1080: return g_siglblid;
    default: return 0;
    }
}

void write_privileged(std::uint32_t address, std::uint64_t value) {
    switch (address & 0x1FF0u) {
    case 0x0000:
        if (trace_display() && value != g_pmode) std::fprintf(stderr, "[gs] pmode 0x%llX\n", static_cast<unsigned long long>(value));
        g_pmode = value;
        break;
    case 0x0010: g_smode1 = value; break;
    case 0x0020: g_smode2 = value; break;
    case 0x0030: g_srfsh = value; break;
    case 0x0040: g_synch1 = value; break;
    case 0x0050: g_synch2 = value; break;
    case 0x0060: g_syncv = value; break;
    case 0x0070:
        if (trace_display() && value != g_dispfb[0]) std::fprintf(stderr, "[gs] dispfb1 0x%llX\n", static_cast<unsigned long long>(value));
        g_dispfb[0] = value;
        break;
    case 0x0080: g_display[0] = value; break;
    case 0x0090:
        if (trace_display() && value != g_dispfb[1]) std::fprintf(stderr, "[gs] dispfb2 0x%llX\n", static_cast<unsigned long long>(value));
        g_dispfb[1] = value;
        break;
    case 0x00A0: g_display[1] = value; break;
    case 0x00B0: g_extbuf = value; break;
    case 0x00C0: g_extdata = value; break;
    case 0x00D0: g_extwrite = value; break;
    case 0x00E0: g_bgcolor = value; break;
    case 0x1000:
        g_csr &= ~(value & 0x1Fu);
        if (value & 0x200u) {
            g_csr = 0x551B4000ull;
            if (queue::active()) queue::post(queue::kSoftReset, 0, 0, nullptr, 0);
            else soft_reset();
        }
        break;
    case 0x1010:
        g_imr = value;
        if (g_csr & ~(g_imr >> 8) & 0x1Fu) hw::intc_raise(0);
        break;
    case 0x1040: g_busdir = value; break;
    case 0x1080: g_siglblid = value; break;
    default: break;
    }
}

void vblank(bool start) {
    if (start) {
        g_odd_field = !g_odd_field;
        raise(8);
        if (queue::active()) queue::post(queue::kVblank, 0, 0, nullptr, 0);
#ifdef PS2_GPU
        else if (gpu_on()) gpu::flush();
#endif
    }
}

bool interlaced_odd_field() { return g_odd_field; }

bool attach_window(int kind, void *display, std::uint64_t window, void *surface, const char *selector, std::uint32_t width, std::uint32_t height) {
#ifdef PS2_GPU
    if (gpu::mode() != gpu::Mode::primary) return false;
    const gpu::SurfaceTarget target{static_cast<gpu::SurfaceTarget::Kind>(kind), display, window, surface, selector, width, height};
    return gpu::attach_surface(target);
#else
    (void)kind, (void)display, (void)window, (void)surface, (void)selector, (void)width, (void)height;
    return false;
#endif
}

void resize_window(std::uint32_t width, std::uint32_t height) {
#ifdef PS2_GPU
    gpu::resize_surface(width, height);
#else
    (void)width, (void)height;
#endif
}

double gpu_busy_ms() {
#ifdef PS2_GPU
    return gpu::busy_ms();
#else
    return 0.0;
#endif
}

DisplayRegs display_regs() { return DisplayRegs{g_pmode, g_smode2, {g_dispfb[0], g_dispfb[1]}, {g_display[0], g_display[1]}}; }

bool present() { return present(display_regs()); }

bool present(const DisplayRegs &r) {
#ifdef PS2_GPU
    if (!gpu::has_surface()) return false;
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    if (circuit < 0) return true;
    const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const std::uint32_t width = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, height = ((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / magv;
    if (!width || !height || width > 2048 || height > 2048) return true;
    const gpu::DisplaySource src{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu, static_cast<std::uint32_t>(fb >> 15) & 0x1Fu,
                                 static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, width, height, (r.smode2 & 3u) == 3u};
    return gpu::present(src);
#else
    return false;
#endif
}

bool display(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return display(display_regs(), rgba, width, height); }

bool display(const DisplayRegs &r, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    if (circuit < 0) return false;
    const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
    const std::uint32_t fbp = static_cast<std::uint32_t>(fb) & 0x1FFu, fbw = static_cast<std::uint32_t>(fb >> 9) & 0x3Fu;
    const std::uint32_t psm = static_cast<std::uint32_t>(fb >> 15) & 0x1Fu;
    const std::uint32_t dbx = static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, dby = static_cast<std::uint32_t>(fb >> 43) & 0x7FFu;
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const std::uint32_t dw = (static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1, dh = (static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1;
    width = dw / magh;
    height = dh / magv;
    if (!width || !height || width > 2048 || height > 2048) return false;
    const bool halved = (r.smode2 & 3u) == 3u;
#ifdef PS2_GPU
    if (gpu_on()) {
        const gpu::DisplaySource src{fbp * 32, fbw, psm, dbx, dby, width, height, halved};
        if (!cpu_raster()) return gpu::display(src, rgba, width, height);
        static const bool compare = std::getenv("PS2_GS_COMPARE") != nullptr;
        std::uint32_t gw = 0, gh = 0;
        if (compare) g_gpu_frame.clear(), gpu::display(src, g_gpu_frame, gw, gh);
    }
#endif
    rgba.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t c = read_pixel(psm, fbp * 32, fbw, dbx + x, dby + (halved ? y / 2 : y));
            if (psm == PSMCT16 || psm == PSMCT16S)
                c = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9) | 0xFF000000u;
            rgba[static_cast<std::size_t>(y) * width + x] = c | 0xFF000000u;
        }
#ifdef PS2_GPU
    if (!g_gpu_frame.empty() && g_gpu_frame.size() == rgba.size()) {
        std::size_t differ = 0;
        int worst = 0;
        for (std::size_t i = 0; i < rgba.size(); ++i) {
            int delta = 0;
            for (int ch = 0; ch < 24; ch += 8)
                delta = std::max(delta, std::abs(static_cast<int>((rgba[i] >> ch) & 0xFFu) - static_cast<int>((g_gpu_frame[i] >> ch) & 0xFFu)));
            if (delta > 2) ++differ;
            worst = std::max(worst, delta);
        }
        std::fprintf(stderr, "[gs] gpu differs in %zu of %zu pixels (max %d)\n", differ, rgba.size(), worst);
        if (std::strcmp(std::getenv("PS2_GS_COMPARE"), "show") == 0) rgba.swap(g_gpu_frame);
    }
#endif
    return true;
}

bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return copy_frame(display_regs(), rgba, width, height); }

bool copy_frame(const DisplayRegs &r, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    static std::vector<std::uint32_t> previous;
    bool got = false;
#ifdef PS2_GPU
    if (gpu_on() && !cpu_raster()) {
        const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
        if (circuit < 0) return false;
        const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
        const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
        const std::uint32_t w = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, h = ((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / magv;
        if (!w || !h || w > 2048 || h > 2048) return false;
        const gpu::DisplaySource src{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu, static_cast<std::uint32_t>(fb >> 15) & 0x1Fu,
                                     static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, w, h, (r.smode2 & 3u) == 3u};
        got = gpu::display_async(src, rgba, width, height);
    } else
#endif
        got = display(r, rgba, width, height);
    if (!got) return false;
    if (previous.size() != rgba.size()) {
        previous = rgba;
        return true;
    }
    static const bool weave_off = std::getenv("PS2_NO_DEINTERLACE") != nullptr;
    const bool blend = (r.smode2 & 3u) == 3u && !weave_off;
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        const std::uint32_t c = rgba[i];
        if (blend) rgba[i] = (c & previous[i]) + (((c ^ previous[i]) & 0xFEFEFEFEu) >> 1);
        previous[i] = c;
    }
    return true;
}

std::atomic<std::uint32_t> g_presented{0};
std::uint32_t g_posted_presents = 0;

void flush_and_wait() {
#ifdef PS2_GPU
    if (gpu_on()) gpu::wait_idle();
#endif
}

void start_thread() { queue::render().start(32u << 20); }

void post_present() {
    const DisplayRegs regs = display_regs();
    queue::post(queue::kPresent, 0, 0, &regs, sizeof regs);
    ++g_posted_presents;
    for (std::uint32_t done = g_presented.load(); g_posted_presents - done > 2; done = g_presented.load()) queue::wait(g_presented, done);
}

Rendered render_pending(const std::function<void(const DisplayRegs &)> &on_present, double budget_ms) {
    queue::role() = queue::Role::Render;
    queue::Header h;
    const std::uint8_t *payload = nullptr;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t n = 1; queue::render().peek(h, payload); ++n) {
        if (!(n & 7u) && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budget_ms) return Rendered::busy;
        bool presented = false;
        switch (h.type) {
        case queue::kGif: gif::render(static_cast<int>(h.arg0), payload, h.arg1); break;
        case queue::kGifReset: gif::render_reset(); break;
        case queue::kReset: render_reset(); break;
        case queue::kSoftReset: soft_reset(); break;
        case queue::kVblank:
#ifdef PS2_GPU
            if (gpu_on()) gpu::flush();
#endif
            break;
        case queue::kPresent: {
            DisplayRegs regs;
            std::memcpy(&regs, payload, sizeof regs);
            on_present(regs);
            presented = true;
            break;
        }
        case queue::kState: serve_state(payload); break;
        case queue::kDownload: {
            std::uintptr_t target;
            std::memcpy(&target, payload, sizeof target);
            exact_download();
            render_read_image(reinterpret_cast<std::uint8_t *>(target), h.arg0);
            g_downloads.fetch_add(1);
            queue::notify(g_downloads);
            break;
        }
        case queue::kDownloadAsync: {
            DownloadKey key;
            std::memcpy(&key, payload, sizeof key);
            std::vector<std::uint8_t> fresh(static_cast<std::size_t>(h.arg0) * 16);
            render_read_image(fresh.data(), h.arg0);
            if (key.buf == g_state.bitbltbuf && key.pos == g_state.trxpos && key.reg == g_state.trxreg) remember(key, fresh.data(), fresh.size());
            else forget(key);
            break;
        }
        default: break;
        }
        queue::render().pop(h);
        if (presented) {
            g_presented.fetch_add(1);
            queue::notify(g_presented);
            return Rendered::presented;
        }
    }
    return Rendered::idle;
}

std::uint64_t primitives_drawn() { return g_primitives; }

void dump_region(const char *spec) {
#ifdef PS2_GPU
    if (gpu_on() && !cpu_raster()) gpu::download(g_memory);
#endif
    unsigned bp = 0, bw = 0, psm = 0, w = 0, h = 0;
    char path[256] = {};
    if (std::sscanf(spec, "%x,%u,%u,%u,%u,%255s", &bp, &bw, &psm, &w, &h, path) != 6) return;
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const std::uint32_t c = read_pixel(psm, bp, bw, x, y);
            const std::uint8_t px[3] = {static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c >> 16)};
            std::fwrite(px, 1, 3, f);
        }
    std::fclose(f);
}

void stats() {
    if (const char *spec = std::getenv("PS2_DUMP_VRAM")) dump_region(spec);
#ifdef PS2_GPU
    gpu::stats();
#endif
    hd_log_summary();
    hd_stats();
    std::fprintf(stderr, "[gs] %llu primitives drawn, register writes:", static_cast<unsigned long long>(g_primitives));
    for (int r = 0; r < 128; ++r)
        if (g_writes[r]) std::fprintf(stderr, " %02X:%llu", r, static_cast<unsigned long long>(g_writes[r]));
    std::fprintf(stderr, "\n");
}

}

namespace ps2::gs {

namespace {

std::atomic<std::uint32_t> g_state_done{0};
std::string g_state_error;

void render_state(ps2::state::Stream &s) {
    flush_and_wait();
    s.pod(g_state);
    ++g_setup_generation;
    s.pod(g_pmode);
    s.pod(g_smode1);
    s.pod(g_smode2);
    s.pod(g_srfsh);
    s.pod(g_synch1);
    s.pod(g_synch2);
    s.pod(g_syncv);
    s.pod(g_dispfb);
    s.pod(g_display);
    s.pod(g_extbuf);
    s.pod(g_extdata);
    s.pod(g_extwrite);
    s.pod(g_bgcolor);
    s.pod(g_csr);
    s.pod(g_imr);
    s.pod(g_busdir);
    s.pod(g_siglblid);
    s.pod(g_primitives);
    s.pod(g_odd_field);
    s.pod(g_transfer);
    s.pod(g_carry);
    s.size(g_carried);
    std::vector<std::uint32_t> vram, clut;
    std::uint32_t scale = 1;
#ifdef PS2_GPU
    if (s.saving() && gpu_on()) {
        gpu::download(g_memory);
        gpu::read_state(vram, scale, clut);
    }
#endif
    s.bytes(g_memory, kMemorySize);
    s.pod(scale);
    s.vec(vram);
    s.vec(clut);
#ifdef PS2_GPU
    if (s.loading() && gpu_on()) {
        if (vram.empty()) {
            vram.resize(kMemorySize / 4);
            std::memcpy(vram.data(), g_memory, kMemorySize);
            clut.assign(g_state.clut, g_state.clut + 512);
            scale = 1;
        }
        gpu::write_state(vram, scale, clut);
    }
#endif
}

}

void serve_state(const void *payload) {
    std::uintptr_t target;
    std::memcpy(&target, payload, sizeof target);
    try {
        render_state(*reinterpret_cast<ps2::state::Stream *>(target));
    } catch (const std::exception &e) {
        g_state_error = e.what();
    }
    g_state_done.fetch_add(1);
    queue::notify(g_state_done);
}

void state(ps2::state::Stream &s) {
    s.tag("gs");
    if (!queue::active()) {
        render_state(s);
        return;
    }
    g_state_error.clear();
    const std::uint32_t seen = g_state_done.load();
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(&s);
    queue::post(queue::kState, 0, 0, &target, sizeof target);
    while (g_state_done.load() == seen) queue::wait(g_state_done, seen);
    if (!g_state_error.empty()) throw std::runtime_error(g_state_error);
}

}

#include "ps2/hd.hpp"

namespace ps2 {

std::size_t hd_setup(std::function<void(const std::string &, std::uint32_t, std::uint32_t, std::uint32_t)> request, const char *names, std::size_t length) {
#ifdef PS2_GPU
    const std::uint32_t words = gs::gpu::hd_capacity_words();
    if (!words) return 0;
    gs::hd_configure(words, std::move(request), [](std::uint32_t offset, const std::uint8_t *rgba, std::size_t bytes) { return gs::gpu::hd_upload(offset, rgba, bytes); });
    return gs::hd_add_names(names, length);
#else
    (void)request;
    (void)names;
    (void)length;
    return 0;
#endif
}

void hd_loaded(std::uint32_t id, const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, std::uint32_t shift) {
    gs::hd_loaded(id, rgba, width, height, shift);
}


// Called once per presented frame: true = show this frame 4:3 (no 3D drawn lately: menus, loading, movies).
bool gs::present_as_43() {
    static int flat = 0, deep = 0;
    static bool as43 = false;
    static unsigned long long calls = 0;
    ++calls;
    if (ps2::g_options.aspect <= 1.34f) return false;
    if (ps2::video_options() & (1u << 29)) return false;
    const bool composite = gs::g_saw_composite.exchange(false, std::memory_order_relaxed);
    if (gs::g_3d_draws.exchange(0, std::memory_order_relaxed) > 300 || composite) { deep++; flat = 0; }
    else { flat++; deep = 0; }
    const bool was = as43;
    if (!as43 && flat >= 8) as43 = true;
    if (as43 && deep >= 2) as43 = false;
    static const bool log43 = std::getenv("PS2_LOG_43") != nullptr;
    if (log43 && was != as43) std::fprintf(stderr, "[43] frame %llu -> %s\n", calls, as43 ? "4:3" : "wide");
    return as43;
}

}
