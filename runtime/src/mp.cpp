// Multiplayer puppets for Tokyo Drift (SLUS-21449).
// A puppet is a car in the game world that shows a remote player instead of whatever drove it:
// after the game's own car update (GSRaceCar_c, 0x311F20) writes the car's matrix from physics,
// we overwrite it with the remote player's pose. The physics body is left alone (it is NovodeX's),
// so a puppet is never solid - ghost mode by construction.
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/mp.hpp"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <mutex>
#include "ps2/system.hpp"
#include <unordered_map>

namespace ps2::mp {

namespace {

constexpr std::uint32_t kCarUpdate = 0x00311F20u;  // GSRaceCar_c::Update(this) - copies mover pose into the car matrix
constexpr std::uint32_t kCarVtable = 0x00521480u;  // GSRaceCar_c vptr, stored at object + 8
constexpr std::uint32_t kMatrix = 0x10u;            // 4x4 world matrix (rows right/up/forward/position) at object + 0x10

std::mutex g_lock;
std::unordered_map<std::uint32_t, Pose> g_puppets;  // car object -> pose
std::uint32_t g_follow_car = 0, g_follow_leader = 0;  // debug: puppet that mirrors another car, offset forward
float g_follow_ahead = 0, g_follow_side = 0;
bool g_installed = false;

void stf(std::uint32_t a, float v) { std::uint32_t u; std::memcpy(&u, &v, 4); st32(a, u); }
float ldf(std::uint32_t a) { const std::uint32_t u = ld32(a); float v; std::memcpy(&v, &u, 4); return v; }

}
bool read_car(std::uint32_t car, Pose &out);
namespace {
// ---- online v0.01 state ----
struct Slot { bool want = false; Pose pose; std::uint32_t car = 0; std::uint64_t car_seen = 0; };
Slot g_slots[kSlots];
std::unordered_map<std::uint32_t, std::uint64_t> g_live;   // car object -> last update tick
std::uint64_t g_tick = 0;
std::uint32_t g_local = 0;                                    // the car the player drives (see local_car())
std::uint32_t local_car();
bool slot_car(std::uint32_t car) { for (const Slot &s : g_slots) if (s.car == car) return true; return false; }

bool car_update(Cpu &c) {
    const std::uint32_t self = c.r[4].w[0];
    const std::uint32_t ra = c.r[31].w[0];
    call_original(c, kCarUpdate, 0);
    {
        std::lock_guard lock(g_lock);
        g_live[self] = ++g_tick;
    }
    if (self == g_follow_car && g_follow_leader) {
        Pose lead;
        if (read_car(g_follow_leader, lead)) {
            for (int k = 0; k < 3; ++k) lead.pos[k] += lead.rot[2][k] * g_follow_ahead + lead.rot[0][k] * g_follow_side;
            std::lock_guard lock(g_lock);
            g_puppets[self] = lead;
        }
    }
    Pose p;
    bool puppet = false;
    {
        std::lock_guard lock(g_lock);
        auto it = g_puppets.find(self);
        if (it != g_puppets.end()) { p = it->second; puppet = true; }
    }
    if (puppet) {
        const std::uint32_t m = self + kMatrix;
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) stf(m + r * 16 + k * 4, p.rot[r][k]);
        for (int k = 0; k < 3; ++k) stf(m + 48 + k * 4, p.pos[k]);
    }
    c.pc = ra;
    return true;
}

}

void install() {
    if (g_installed) return;
    g_installed = true;
    set_hook(kCarUpdate, car_update);
}

void set_puppet(std::uint32_t car, const Pose &pose) {
    install();
    std::lock_guard lock(g_lock);
    g_puppets[car] = pose;
}

void clear_puppet(std::uint32_t car) {
    std::lock_guard lock(g_lock);
    g_puppets.erase(car);
}

bool read_car(std::uint32_t car, Pose &out) {
    if (ld32(car + 8) != kCarVtable) return false;
    const std::uint32_t m = car + kMatrix;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) out.rot[r][k] = ldf(m + r * 16 + k * 4);
    for (int k = 0; k < 3; ++k) out.pos[k] = ldf(m + 48 + k * 4);
    return true;
}

// Debug: PS2_MP_FOLLOW=car,leader,ahead,side makes car mirror leader's pose, offset (a stand-in for network data).
// Debug: PS2_MP_TEST=car,x,y,z parks that car's picture at a fixed spot (heading kept) to prove puppets work.
namespace { void online_hooks(); }
void debug_init() {
    // called from boot() on the game thread: the only safe place to add hooks
    install();
    online_hooks();
    if (const char *f = std::getenv("PS2_MP_FOLLOW")) {
        unsigned car = 0, lead = 0;
        if (std::sscanf(f, "%x,%x,%f,%f", &car, &lead, &g_follow_ahead, &g_follow_side) == 4) {
            g_follow_car = car;
            g_follow_leader = lead;
            install();
            std::fprintf(stderr, "[mp] follow: car %08X mirrors %08X, %.1f ahead %.1f side\n", car, lead, g_follow_ahead, g_follow_side);
        }
    }
    const char *spec = std::getenv("PS2_MP_TEST");
    if (!spec) return;
    unsigned car = 0;
    float x = 0, y = 0, z = 0;
    if (std::sscanf(spec, "%x,%f,%f,%f", &car, &x, &y, &z) != 4) return;
    Pose p{};
    p.rot[0][0] = 1; p.rot[1][1] = 1; p.rot[2][2] = 1;
    p.pos[0] = x; p.pos[1] = y; p.pos[2] = z;
    set_puppet(car, p);
    std::fprintf(stderr, "[mp] test puppet: car %08X shown at %.1f %.1f %.1f\n", car, x, y, z);
}


// ===== online v0.01 =====
// Found in free roam (career, hub + Wangan): two global GSGameCamera_c objects.
//   0x5C9E40: the view camera, world matrix (rows right/up/forward/position) at +0x10
//   0x5C9F30: the chase camera, the car it follows (= the player's car) at +0x2FC
// Free roam = GSCruiseModeState_c is running (its methods are hooked below); anything else
// (touge / race / garage / menus) counts as "elsewhere", so players there are solo.
namespace {
constexpr std::uint32_t kViewCam = 0x005C9E40u, kChaseCam = 0x005C9F30u, kCamVtable = 0x00513EA0u, kCamTarget = 0x2FCu;
constexpr std::uint32_t kCruiseMethods[] = {0x1E9380u, 0x1E8350u, 0x1E8468u, 0x1E8680u, 0x1E8B08u, 0x1E9F98u, 0x1EA068u};
std::atomic<std::uint64_t> g_cruise_frame{0};
std::atomic<std::uint32_t> g_cruise_hits[7];
bool g_online_hooks = false;
std::uint32_t local_car() {
    if (ld32(kChaseCam + 8) != kCamVtable) return 0;
    const std::uint32_t car = ld32(kChaseCam + kCamTarget);
    return (car && car < 0x02000000u && ld32(car + 8) == kCarVtable) ? car : 0;
}
int current_world() {
    const std::uint64_t f = ps2::frames_run(), seen = g_cruise_frame.load(std::memory_order_relaxed);
    return seen && f - seen < 30 ? 1 : 0;
}
void online_hooks() {
    if (g_online_hooks) return;
    g_online_hooks = true;
    for (int i = 0; i < 7; ++i) {
        set_hook(kCruiseMethods[i], [i](Cpu &) {
            g_cruise_frame.store(ps2::frames_run(), std::memory_order_relaxed);
            g_cruise_hits[i].fetch_add(1, std::memory_order_relaxed);
            return false;
        });
    }
}
}

bool camera_pose(float out[12]) {
    if (ld32(kViewCam + 8) != kCamVtable) return false;
    for (int i = 0; i < 12; ++i) out[i] = ldf(kViewCam + 0x10 + (i / 3) * 16 + (i % 3) * 4);
    return true;
}

void online_debug_log() {
    static const bool on = std::getenv("PS2_MP_LOG") != nullptr;
    if (!on) return;
    if (ps2::frames_run() % 60) return;
    Pose p; int w = 0;
    const bool ok = local_pose(p, w);
    float cam[12] = {};
    camera_pose(cam);
    std::fprintf(stderr, "[mp] frame %llu local %s car %08X pos %.1f %.1f %.1f world %d cam %.1f %.1f %.1f hits",
                 static_cast<unsigned long long>(ps2::frames_run()), ok ? "yes" : "no", local_car(), p.pos[0], p.pos[1], p.pos[2], w, cam[9], cam[10], cam[11]);
    for (auto &h : g_cruise_hits) std::fprintf(stderr, " %u", h.exchange(0));
    std::fprintf(stderr, "\n");
}

bool local_pose(Pose &out, int &world) {
    const std::uint32_t car = local_car();
    if (!car || !read_car(car, out)) return false;
    // velocity from the physics body (mover at +0xF0, body at mover+0x28, velocity at body+0x30)
    const std::uint32_t mover = ld32(car + 0xF0), body = mover && mover < 0x02000000u ? ld32(mover + 0x28) : 0;
    if (body && body < 0x02000000u) for (int k = 0; k < 3; ++k) out.vel[k] = ldf(body + 0x30 + k * 4);
    world = current_world();
    return true;
}

void set_remote(int slot, const Pose &pose, bool present) {
    if (slot < 0 || slot >= kSlots) return;
    std::lock_guard lock(g_lock);
    Slot &s = g_slots[slot];
    s.want = present;
    s.pose = pose;
    if (!present && s.car) { g_puppets.erase(s.car); s.car = 0; }
    // the car it used stopped updating (despawned): find another one
    if (s.car && g_tick - s.car_seen > 200) { g_puppets.erase(s.car); s.car = 0; }
}

}
