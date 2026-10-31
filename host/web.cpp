#include "ps2/disc.hpp"
#include "ps2/hd.hpp"
#include "ps2/state_request.hpp"
#include "ps2/system.hpp"
#include "ps2/video.hpp"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include "ps2/mp.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void register_generated_code();
void register_generated_vu();

EM_JS_DEPS(ps2web, "$stringToNewUTF8,$UTF8ToString");

EM_JS(void, ps2web_fetch, (const char *url, double offset, int size, void *out, std::atomic<std::uint32_t> *state), {
    const target = UTF8ToString(url);
    const finish = (value) => {
        Atomics.store(HEAP32, state >> 2, value);
        Atomics.notify(HEAP32, state >> 2);
    };
    if (Module.discRead) {
        // the page supplies the disc (the player's own ISO kept in browser storage, or the game server)
        Module.discRead(offset, size).then((bytes) => {
            HEAPU8.set(bytes.subarray(0, Math.min(bytes.length, size)), out);
            finish(3);
        }).catch((e) => {
            console.warn('disc read failed', e);
            finish(4);
        });
        return;
    }
    fetch(target, {headers: {Range: 'bytes=' + offset + '-' + (offset + size - 1)}})
        .then((response) => {
            if (response.status !== 206 && response.status !== 200) throw new Error('status ' + response.status);
            return response.arrayBuffer().then((buffer) => {
                let bytes = new Uint8Array(buffer);
                if (response.status === 200) bytes = bytes.subarray(offset, offset + size);
                HEAPU8.set(bytes.subarray(0, Math.min(bytes.length, size)), out);
                finish(3);
            });
        })
        .catch((e) => {
            console.warn('disc read failed', e);
            finish(4);
        });
});

// A VU1 program the build lacks: keep a copy on the page (main thread) for the performance report,
// so it can be baked into the next build. No network, no waiting.
int ps2web_save_program(const char *name, const std::uint8_t *code, int size) {
    return MAIN_THREAD_EM_ASM_INT({
        try {
            const list = Module.newVuPrograms || (Module.newVuPrograms = []);
            if (list.length >= 100) return 201;
            const bytes = HEAPU8.slice($1, $1 + $2);
            let bin = '';
            for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
            list.push({ name: UTF8ToString($0), data: btoa(bin) });
            return 201;
        } catch (e) { return 0; }
    }, name, code, size);
}

EM_JS(double, ps2web_disc_size, (), { return Module.discSize || 0; });
EM_JS(char *, ps2web_state_url, (), { return Module.stateUrl ? stringToNewUTF8(new URL(Module.stateUrl, location.href).href) : 0; });
EM_JS(double, ps2web_url_size, (const char *url), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open('HEAD', UTF8ToString(url), false);
        xhr.send();
        return xhr.status === 200 ? Number(xhr.getResponseHeader('Content-Length') || 0) : 0;
    } catch (e) {
        return 0;
    }
});
EM_JS(double, ps2web_url_read, (const char *url, std::uint8_t *out, double size), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open('GET', UTF8ToString(url), false);
        xhr.responseType = 'arraybuffer';
        xhr.send();
        if (xhr.status !== 200) return 0;
        const bytes = new Uint8Array(xhr.response);
        HEAPU8.set(bytes.subarray(0, size), out);
        return bytes.length;
    } catch (e) {
        return 0;
    }
});
EM_JS(char *, ps2web_disc_url, (), { return stringToNewUTF8(new URL(Module.discUrl, location.href).href); });
EM_JS(void, ps2web_frame_done, (), {
    if (!Module.shownChannel) {
        Module.shownChannel = new MessageChannel();
        Module.shownWaiters = [];
        Module.shownChannel.port1.onmessage = () => {
            Module.shownPending = false;
            const waiters = Module.shownWaiters;
            Module.shownWaiters = [];
            for (const resolve of waiters) resolve();
        };
    }
    Module.shownPending = true;
    requestAnimationFrame(() => Module.shownChannel.port2.postMessage(0));
    if (Module.onFrame) Module.onFrame();
});
EM_ASYNC_JS(void, ps2web_wait_shown, (), {
    if (Module.shownPending) await new Promise((resolve) => Module.shownWaiters.push(resolve));
});
EM_JS(void, ps2web_stream_frame, (const std::uint32_t *rgba, int width, int height), {
    if (Module.onStreamFrame) Module.onStreamFrame(HEAPU8.subarray(rgba, rgba + width * height * 4), width, height);
});

EM_JS(void, ps2web_blit, (const std::uint32_t *rgba, int width, int height, int aspect10k), {
    const canvas = Module.canvas;
    if (!Module.blitContext) Module.blitContext = canvas.getContext('2d');
    if (!Module.blitImage || Module.blitImage.width !== width || Module.blitImage.height !== height) {
        Module.blitImage = new ImageData(width, height);
        Module.blitSource = new OffscreenCanvas(width, height);
    }
    Module.blitImage.data.set(HEAPU8.subarray(rgba, rgba + width * height * 4));
    Module.blitSource.getContext('2d').putImageData(Module.blitImage, 0, 0);
    const ctx = Module.blitContext;
    const across = aspect10k / 10000, down = 1;
    const scale = Math.min(canvas.width / across, canvas.height / down);
    const w = scale * across, h = scale * down;
    const sx = Math.max(1, Math.floor(w / width)), sy = Math.max(1, Math.floor(h / height));
    let source = Module.blitSource;
    if (sx > 1 || sy > 1) {
        if (!Module.blitSharp || Module.blitSharp.width !== width * sx || Module.blitSharp.height !== height * sy)
            Module.blitSharp = new OffscreenCanvas(width * sx, height * sy);
        const sharp = Module.blitSharp.getContext('2d');
        sharp.imageSmoothingEnabled = false;
        sharp.drawImage(Module.blitSource, 0, 0, width * sx, height * sy);
        source = Module.blitSharp;
    }
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(source, (canvas.width - w) / 2, (canvas.height - h) / 2, w, h);
});

namespace {

constexpr std::size_t kChunk = 256u << 10;
constexpr std::uint32_t kSlots = 256;
constexpr std::uint64_t kReadAhead = 16;
constexpr std::uint32_t kAudioFrames = 1u << 14;

enum : std::uint32_t { kEmpty, kWanted, kLoading, kReady, kFailed };

struct Slot {
    std::atomic<std::uint32_t> state{kEmpty};
    std::uint64_t index = 0, used = 0;
    std::uint32_t size = 0;
    std::uint8_t *data = nullptr;
};

struct AudioRing {
    std::atomic<std::uint32_t> write{0}, read{0};
    std::uint32_t capacity = kAudioFrames;
    std::atomic<std::uint32_t> active{0};
    std::int16_t samples[kAudioFrames * 2];
};

AudioRing g_audio;
Slot g_slots[kSlots];
std::uint64_t g_disc_size = 0, g_use_clock = 0, g_last_chunk = ~0ull, g_run = 0;
char *g_disc_url = nullptr;
char *g_state_url = nullptr;

Slot *find_slot(std::uint64_t index) {
    for (Slot &s : g_slots)
        if (s.state.load(std::memory_order_acquire) != kEmpty && s.index == index) return &s;
    return nullptr;
}

Slot *want(std::uint64_t index) {
    if (index * kChunk >= g_disc_size) return nullptr;
    if (Slot *s = find_slot(index)) return s;
    Slot *victim = nullptr;
    for (Slot &s : g_slots) {
        const std::uint32_t state = s.state.load(std::memory_order_acquire);
        if (state == kEmpty) {
            victim = &s;
            break;
        }
        if ((state == kReady || state == kFailed) && (!victim || s.used < victim->used)) victim = &s;
    }
    if (!victim) return nullptr;
    if (!victim->data) victim->data = new std::uint8_t[kChunk];
    victim->index = index;
    victim->size = static_cast<std::uint32_t>(std::min<std::uint64_t>(kChunk, g_disc_size - index * kChunk));
    victim->used = ++g_use_clock;
    victim->state.store(kWanted, std::memory_order_release);
    ps2::wake_render_thread();
    return victim;
}

void advance(std::uint64_t index) {
    if (index <= g_last_chunk && g_last_chunk - index <= kReadAhead * 4) return;
    g_run = index == g_last_chunk + 1 ? g_run + 1 : 0;
    g_last_chunk = index;
}

void read_ahead(std::uint64_t index) {
    for (std::uint64_t k = 1; k <= std::min(g_run * 2, kReadAhead); ++k) want(index + k);
}

const Slot *chunk(std::uint64_t index) {
    Slot *s = want(index);
    advance(index);
    read_ahead(std::max(index, g_last_chunk));
    for (int attempt = 0; s && attempt < 4; ++attempt) {
        for (std::uint32_t state = s->state.load(); state != kReady && state != kFailed; state = s->state.load())
            emscripten_futex_wait(&s->state, state, INFINITY);
        if (s->state.load() == kReady) {
            s->used = ++g_use_clock;
            return s;
        }
        usleep(100000);
        s->state.store(kWanted, std::memory_order_release);
        ps2::wake_render_thread();
    }
    return nullptr;
}

bool disc_ready(std::uint64_t offset, std::size_t size) {
    if (!size) return true;
    const std::uint64_t last = (offset + size - 1) / kChunk;
    bool ready = true;
    for (std::uint64_t index = offset / kChunk; index <= last; ++index) {
        Slot *s = want(index);
        if (s && s->state.load(std::memory_order_acquire) == kFailed) {
            s->state.store(kWanted, std::memory_order_release);
            ps2::wake_render_thread();
        }
        ready = ready && s && s->state.load(std::memory_order_acquire) == kReady;
        advance(index);
    }
    read_ahead(std::max(last, g_last_chunk));
    return ready;
}

std::size_t read_disc(std::uint64_t offset, void *out, std::size_t size) {
    std::size_t done = 0;
    auto *to = static_cast<std::uint8_t *>(out);
    while (done < size && offset + done < g_disc_size) {
        const std::uint64_t at = offset + done;
        const Slot *c = chunk(at / kChunk);
        if (!c) break;
        const std::size_t within = static_cast<std::size_t>(at % kChunk);
        const std::size_t take = std::min<std::size_t>(size - done, c->size - within);
        std::memcpy(to + done, c->data + within, take);
        done += take;
    }
    return done;
}

void service_disc() {
    for (Slot &s : g_slots) {
        std::uint32_t expected = kWanted;
        if (s.state.compare_exchange_strong(expected, kLoading, std::memory_order_acq_rel))
            ps2web_fetch(g_disc_url, static_cast<double>(s.index * kChunk), static_cast<int>(s.size), s.data, &s.state);
    }
}

void push_audio(const std::int16_t *samples, std::size_t frames) {
    if (ps2::intro_turbo()) return;  // fast-forwarded intro: silence instead of chipmunk noise
    std::uint32_t write = g_audio.write.load(std::memory_order_relaxed);
    const std::uint32_t read = g_audio.read.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < frames && write - read < kAudioFrames; ++i, ++write) {
        const std::uint32_t at = (write & (kAudioFrames - 1)) * 2;
        g_audio.samples[at] = samples[i * 2];
        g_audio.samples[at + 1] = samples[i * 2 + 1];
    }
    g_audio.write.store(write, std::memory_order_release);
}

double audio_queued_ms() { return static_cast<double>(g_audio.write.load() - g_audio.read.load()) / 48.0; }

double g_next_frame = 0.0;
std::atomic<double> g_emulation_idle{0.0}, g_render_idle{0.0};

void pace() {
    ps2::post_present();
    if (g_audio.active.load()) {
        while (audio_queued_ms() > 80.0) usleep(1000);
        g_next_frame = 0.0;
        return;
    }
    const double period = ps2::g_options.pal ? 20.0 : 1000.0 / 59.94;
    const double now = emscripten_get_now();
    if (g_next_frame == 0.0 || now - g_next_frame > 200.0) g_next_frame = now;
    g_next_frame += period;
    if (g_next_frame > now) usleep(static_cast<useconds_t>((g_next_frame - now) * 1000.0));
}

std::atomic<int> g_paused{0};

void on_vblank() {
    const double start = emscripten_get_now();
    // in-game settings menu open: hold the game at the end of this frame (pace() re-syncs afterwards)
    while (g_paused.load(std::memory_order_relaxed)) usleep(16000);
    if (!ps2::intro_turbo()) pace();
    g_emulation_idle.store(g_emulation_idle.load() + emscripten_get_now() - start);
}

struct Launch {
    std::vector<std::string> args;
};

void *emulate(void *data) {
    const Launch &launch = *static_cast<const Launch *>(data);
    if (g_disc_size && !ps2::disc::open("disc", g_disc_size, read_disc, disc_ready)) std::fprintf(stderr, "[web] cannot read the disc image\n");
    std::uint32_t entry = 0;
    ps2::load_patches("/game/patches.txt");
    if (!ps2::load_elf("/game/core.elf", entry)) {
        std::fprintf(stderr, "cannot load the game executable\n");
        return nullptr;
    }
    ps2::boot(entry, launch.args);
    if (g_state_url) {
        const double size = ps2web_url_size(g_state_url);
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        if (size <= 0 || ps2web_url_read(g_state_url, data.data(), size) != size || !ps2::load_state(data.data(), data.size())) {
            std::fprintf(stderr, "[web] cannot load the state %s\n", g_state_url);
            return nullptr;
        }
    }
    ps2::run();
    EM_ASM({
        if (typeof Module.onHalt === 'function')
            Module.onHalt(UTF8ToString($0));
    }, ps2::halt_reason().c_str());
    return nullptr;
}

}

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_pad(int pad, int buttons, int lx, int ly, int rx, int ry) {
    ps2::set_pad_buttons(static_cast<std::uint16_t>(buttons), pad);
    ps2::set_pad_axis(0, static_cast<std::uint8_t>(rx), pad);
    ps2::set_pad_axis(1, static_cast<std::uint8_t>(ry), pad);
    ps2::set_pad_axis(2, static_cast<std::uint8_t>(lx), pad);
    ps2::set_pad_axis(3, static_cast<std::uint8_t>(ly), pad);
}

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_pad_connected(int pad, int connected) { ps2::set_pad_connected(pad, connected != 0); }

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_pressure(int pad, int a, int b, int c, int d) {
    const int packed[4] = {a, b, c, d};
    for (int i = 0; i < 16; ++i) ps2::set_pad_pressure(i, static_cast<std::uint8_t>(static_cast<unsigned>(packed[i / 4]) >> (i % 4 * 8)), pad);
}

extern "C" EMSCRIPTEN_KEEPALIVE int ps2web_rumble(int pad) { return ps2::pad_rumble(pad); }

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_resize(int width, int height) {
    ps2::resize_window(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
}

extern "C" EMSCRIPTEN_KEEPALIVE int ps2web_quick_state(int loading) {
    return ps2::request_state_operation(loading != 0, "/quicksave.state", [](bool load, bool success) {
        MAIN_THREAD_ASYNC_EM_ASM({
            if (Module.onQuickState) Module.onQuickState(!!$0, !!$1);
        }, load, success);
    }) ? 1 : 0;
}

std::atomic<bool> g_streaming{false};

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_stream(int on) { g_streaming.store(on != 0); }

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_trace_frame() { ps2::request_draw_trace(); }
// Online v0.01: 16 floats = pos[3] rot[9] vel[3] world
extern "C" EMSCRIPTEN_KEEPALIVE int ps2web_mp_local(float *out) {
    ps2::mp::Pose p;
    int world = 0;
    if (!ps2::mp::local_pose(p, world)) return 0;
    std::memcpy(out, p.pos, 12); std::memcpy(out + 3, p.rot, 36); std::memcpy(out + 12, p.vel, 12);
    out[15] = static_cast<float>(world);
    return 1;
}
// the game's view camera: 12 floats = right[3] up[3] forward[3] position[3]
extern "C" EMSCRIPTEN_KEEPALIVE int ps2web_mp_camera(float *out) { return ps2::mp::camera_pose(out) ? 1 : 0; }
extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_mp_remote(int slot, const float *in, int present) {
    ps2::mp::Pose p;
    if (present && in) { std::memcpy(p.pos, in, 12); std::memcpy(p.rot, in + 3, 36); std::memcpy(p.vel, in + 12, 12); }
    ps2::mp::set_remote(slot, p, present && in);
}
extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_readbacks(double *out) { ps2::readback_stats(out); }
extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_pause(int on) { g_paused.store(on ? 1 : 0); }
extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_video_options(int flags) { ps2::set_video_options(static_cast<std::uint32_t>(flags)); }

extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_gpu_ms() { return ps2::gpu_busy_ms(); }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_frames() { return static_cast<double>(ps2::frames_run()); }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_primitives() { return static_cast<double>(ps2::primitives_drawn()); }
extern "C" EMSCRIPTEN_KEEPALIVE void *ps2web_audio_ring() { return &g_audio; }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_audio_queued_ms() { return audio_queued_ms(); }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_vu_interpreted() { return static_cast<double>(ps2::vu1_interpreted_calls()); }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_idle_ms(int which) {
    if (which == 2) return ps2::vu_thread_idle_ms();
    return which ? g_render_idle.load() : g_emulation_idle.load();
}

int main(int argc, char **argv) {
    static Launch launch;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) launch.args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--widescreen") ps2::g_options.widescreen = true, ps2::g_options.aspect = 16.0f / 9.0f;
        else if (a == "--aspect" && i + 1 < argc) {
            // --aspect W:H, e.g. 32:9 for a super ultrawide; anything wider than 4:3 also tells the game's OSD config "widescreen"
            float w = 4.0f, h = 3.0f;
            if (std::sscanf(argv[++i], "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f) {
                ps2::g_options.aspect = w / h;
                ps2::g_options.widescreen = w / h > 1.34f;
            }
        } else if (a == "--skip-intro") {
            ps2::g_options.skip_intro = true;
        } else if (a == "--patch" && i + 1 < argc) {
            // --patch place:address:bytes:value (hex address/value), same fields as patches.txt
            unsigned place = 0, bytes = 0;
            unsigned long address = 0;
            unsigned long long value = 0;
            if (std::sscanf(argv[++i], "%u:%lx:%u:%llx", &place, &address, &bytes, &value) == 4)
                ps2::g_options.patches.push_back({static_cast<std::uint8_t>(place), static_cast<std::uint8_t>(bytes), static_cast<std::uint32_t>(address), value});
        }
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (ps2::parse_press(argv[++i], press)) ps2::g_options.presses.push_back(press);
        }
    }
    if (launch.args.empty()) {
        std::string boot = "/game/core.elf";
        if (std::FILE *f = std::fopen("/game/boot.txt", "rb")) {
            char text[256] = {};
            const std::size_t n = std::fread(text, 1, sizeof text - 1, f);
            std::fclose(f);
            std::string line(text, n);
            line.erase(line.find_last_not_of(" \r\n\t") + 1);
            if (!line.empty()) boot = line;
        }
        launch.args.push_back(boot);
    }
    g_disc_size = static_cast<std::uint64_t>(ps2web_disc_size());
    g_disc_url = ps2web_disc_url();
    g_state_url = ps2web_state_url();
    ps2::g_options.on_vblank = on_vblank;
    ps2::g_options.on_vu_program = [](const std::string &name, const std::uint8_t *code, std::size_t size) {
        static bool unsupported = false;
        if (unsupported) return;
        const int status = ps2web_save_program(name.c_str(), code, static_cast<int>(size));
        if (status == 200 || status == 201) std::fprintf(stderr, "[web] new VU1 program %s kept for the performance report\n", name.c_str());
        else unsupported = true;
    };
    if (!EM_ASM_INT({ return Module.noCard ? 1 : 0; })) {
        ps2::g_options.memcard_path = "/memcard/Mcd001.ps2";
        ps2::g_options.on_memcard_saved = [] { MAIN_THREAD_ASYNC_EM_ASM({ if (Module.ps2CardSaved) Module.ps2CardSaved(); }); };
    }
    ps2::set_audio_sink(push_audio);
    register_generated_code();
    register_generated_vu();

    ps2::init_graphics();
    const bool no_surface = EM_ASM_INT({ return Module.noSurface ? 1 : 0; });
    bool copy = EM_ASM_INT({ return Module.presentCopy ? 1 : 0; });
    bool surface = false;
    if (!copy && !no_surface) {
        ps2::WindowTarget target;
        target.kind = ps2::WindowTarget::Canvas;
        target.selector = "#canvas";
        target.width = static_cast<std::uint32_t>(EM_ASM_INT({ return Module.canvas.width; }));
        target.height = static_cast<std::uint32_t>(EM_ASM_INT({ return Module.canvas.height; }));
        surface = ps2::attach_window(target);
    }
    copy = copy || (!surface && !no_surface);
    EM_ASM({
        Module.videoEffectsAvailable = !!$0;
        if (Module.refreshVideoControls) Module.refreshVideoControls();
    }, surface);
    std::fprintf(stderr, "[web] %s\n", surface ? "presenting through WebGPU" : copy ? "presenting through a 2D canvas copy" : "not presenting");

    if (EM_ASM_INT({ return Module.hdNames ? 1 : 0; })) {
        char *names = reinterpret_cast<char *>(EM_ASM_PTR({ return stringToNewUTF8(Module.hdNames); }));
        const std::size_t usable = ps2::hd_setup(
            [](const std::string &file, std::uint32_t id, std::uint32_t tw, std::uint32_t th) {
                EM_ASM({ if (Module.hdRequest) Module.hdRequest(UTF8ToString($0), $1, $2, $3); }, file.c_str(), id, tw, th);
            },
            names, std::strlen(names));
        std::free(names);
        EM_ASM({ if (Module.hdReady) Module.hdReady($0); }, static_cast<int>(usable));
        std::fprintf(stderr, "[web] HD textures: %zu replacements\n", usable);
    }

    ps2::start_render_thread();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        pthread_t vu_thread;
        if (pthread_create(&vu_thread, &attr, [](void *) -> void * {
                ps2::run_vu_thread();
                return nullptr;
            }, nullptr) != 0) {
            std::fprintf(stderr, "[web] cannot start the VU thread\n");
            return 1;
        }
    }
    pthread_t thread;
    if (pthread_create(&thread, &attr, emulate, &launch) != 0) {
        std::fprintf(stderr, "[web] cannot start the emulation thread\n");
        return 1;
    }
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> blit = [](const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) {
        ps2web_blit(rgba, static_cast<int>(width), static_cast<int>(height), static_cast<int>(ps2::g_options.aspect * 10000.0f));
    };
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> streamed = [copy, &blit](const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) {
        if (copy) blit(rgba, width, height);
        if (g_streaming.load()) ps2web_stream_frame(rgba, static_cast<int>(width), static_cast<int>(height));
    };
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> none;
    const std::function<void()> wait_shown = [] {
        static int since_skip = 0;
        static double window_start = 0.0, window_gpu = 0.0, gpu_load = 1.0;
        const double start = emscripten_get_now();
        if (start - window_start > 500.0) {
            const double gpu = ps2::gpu_busy_ms();
            gpu_load = window_start > 0.0 ? (gpu - window_gpu) / (start - window_start) : 1.0;
            window_start = start;
            window_gpu = gpu;
        }
        if (++since_skip > 8 && gpu_load < 0.8 && g_audio.active.load() && audio_queued_ms() < 40.0) {
            since_skip = 0;
            return;
        }
        ps2web_wait_shown();
        g_render_idle.store(g_render_idle.load() + emscripten_get_now() - start);
    };
    for (;;) {
        service_disc();
        for (int guard = 0; guard < 8; ++guard) {
            // Module.hdTake() moves one decoded texture into the heap: [id, ptr, width, height, shift] or null
            int slot[5] = {};
            if (!EM_ASM_INT({
                    const t = Module.hdTake ? Module.hdTake() : null;
                    if (!t) return 0;
                    for (let i = 0; i < 5; ++i) HEAP32[($0 >> 2) + i] = t[i];
                    return 1;
                }, slot))
                break;
            ps2::hd_loaded(static_cast<std::uint32_t>(slot[0]), reinterpret_cast<const std::uint8_t *>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(slot[1]))),
                           static_cast<std::uint32_t>(slot[2]), static_cast<std::uint32_t>(slot[3]), static_cast<std::uint32_t>(slot[4]));
            if (slot[1]) std::free(reinterpret_cast<void *>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(slot[1]))));
        }
        const ps2::Rendered rendered = ps2::render_pending(surface, g_streaming.load() ? streamed : copy ? blit : none, wait_shown, 6.0);
        if (rendered == ps2::Rendered::presented) ps2web_frame_done();
        if (rendered != ps2::Rendered::idle) {
            ps2::browser_yield();
        } else {
            const double start = emscripten_get_now();
            ps2::wait_render_work(50);
            g_render_idle.store(g_render_idle.load() + emscripten_get_now() - start);
        }
    }
}
