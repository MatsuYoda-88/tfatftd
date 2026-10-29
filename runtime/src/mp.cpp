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
#include <cstring>
#include <mutex>
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
bool car_update(Cpu &c) {
    const std::uint32_t self = c.r[4].w[0];
    const std::uint32_t ra = c.r[31].w[0];
    call_original(c, kCarUpdate, 0);
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
void debug_init() {
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

}
