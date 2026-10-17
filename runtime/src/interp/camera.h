// HWDER high frame rate: the game's render camera, recorded once per game tick, and the matrix math
// that rebuilds its view bytes exactly (see camera.cpp and docs/camera.md).
#pragma once

#include "hwder/cpu.h"

namespace interp {

// Screen 0's render camera as Camera::Update left it at the end of one game tick.
struct CamSnap {
    u64 tick = 0;      // hook call count
    double t_ms = 0;   // host QPC time of the hook, ms
    float eye[3], at[3], up[3];
    float roll;
    float near_, far_, fov, aspect, proj_extra;  // +0x140..+0x14c, +0x154
    u32 proj_flags;                              // +0x158
    float view[16], proj[16], viewproj[16];      // +0x80, +0xc0, +0x100, row-major as stored
};

// HWDER_INTERP_MODE=camera: frame interpolation replaces only the camera matrices (default: lerp every
// constant). Read once.
bool camera_mode();

// HWDER_CAM_LOG=<n>: n > 0 enables camera logging (the hook logs every n-th tick; the renderer logs
// match coverage once per second).
int cam_log_every();

// Copy the recorded snapshots, newest first. Returns how many (at most max).
int snapshots(CamSnap* out, int max);

// Camera::BuildView (0x100057cc3c) and the view x proj product of Camera::Update (0x100057cb78),
// with the game's operation order, so a tick's own eye/at/up/roll reproduce its bytes.
void build_view(const float eye[3], const float at[3], const float up[3], float roll, float out[16]);
void mul_view_proj(const float view[16], const float proj[16], float out[16]);
// The inverse view the game uploads as matV2W_1..4 (0x10003d0c14), bit-identical to the game's.
void view_inverse(const float view[16], float out[16]);

// The camera between ticks a (older) and b at alpha in [0, 1]: eye/at/up/roll lerped (up
// renormalised), b's projection kept. False when the pair must not be blended (a cut, a projection
// change, a long gap): show b unblended.
bool lerp_camera(const CamSnap& a, const CamSnap& b, float alpha, float view[16], float viewproj[16]);

}  // namespace interp
