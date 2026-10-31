#pragma once
#include <cstdint>

namespace ps2::mp {

struct Pose {
    float pos[3] = {};
    float rot[3][3] = {};   // rows: right, up, forward
    float vel[3] = {};
};

void install();
void set_puppet(std::uint32_t car, const Pose &pose);
void clear_puppet(std::uint32_t car);
bool read_car(std::uint32_t car, Pose &out);
void debug_init();

// ---- online (v0.01) ----
constexpr int kSlots = 7;
// local car's pose; world = which map it is on (1 = Wangan, 0 = elsewhere). false when no car is driving.
bool local_pose(Pose &out, int &world);
// show remote player `slot` as a ghost car at `pose` (present=false releases the car it was using)
void set_remote(int slot, const Pose &pose, bool present);
// the game's view camera: rows right, up, forward, position (12 floats)
bool camera_pose(float out[12]);
void online_debug_log();   // PS2_MP_LOG: print local car / world / camera once a second

}
