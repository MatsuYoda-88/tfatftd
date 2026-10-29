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

}
