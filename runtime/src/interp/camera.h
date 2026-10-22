// HWDER high frame rate: the game's render camera, recorded once per game tick (see camera.cpp and
// docs/camera.md).
#pragma once

#include "hwder/cpu.h"

namespace interp {

// Screen 0's render camera as Camera::Update left it at the end of one game tick.
struct CamSnap {
    u64 tick = 0;      // hook call count
    u64 addr = 0;      // guest address of the render camera it was read from (screen 0)
    double t_ms = 0;   // host QPC time of the hook, ms
    float eye[3], at[3], up[3];
    float roll;
    float near_, far_, fov, aspect, proj_extra;  // +0x140..+0x14c, +0x154
    u32 proj_flags;                              // +0x158
    float view[16], proj[16], viewproj[16];      // +0x80, +0xc0, +0x100, row-major as stored
};

// HWDER_GAME_INTERP=1 (the game-drawn extra frame, interp_draw.cpp): the game draws an extra frame per tick with a
// blended camera. Read once.
bool game_interp();

// Copy the recorded snapshots, newest first. Returns how many (at most max).
int snapshots(CamSnap* out, int max);

}  // namespace interp
