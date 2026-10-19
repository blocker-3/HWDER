// HWDER high frame rate: find the game's camera and record it once per game tick.
//
// The game keeps its render cameras triple-buffered in a scene object S: two screens (game::Camera, 0x170
// bytes each) per buffer, three buffers selected by an index at S + game::scene::kRenderCamWriteSlot (mostly
// 0 in practice). RenderCamera_Sync(S, slot) runs once per game tick: it copies each active screen's game
// camera into buffer slot % 3 and rebuilds its matrices (Camera_Update). Hooking it gives exactly one
// finished camera per tick, which is what interpolation needs to record. See docs/camera.md for the layout.
//
// Each tick's screen-0 camera goes into a small ring that the object blending reads (interp_draw.cpp, vk_renderer.cpp).
#include "interp/camera.h"

#include "host/timing.h"

#include <cstring>
#include <mutex>

#include "game/guest.h"
#include "game/hook.h"
#include "game/structs.h"
#include "hwder/runtime.h"
#include "interp/interp.h"

namespace interp {
namespace {

constexpr int kRing = 8;

std::mutex g_m;
CamSnap g_ring[kRing];
u64 g_count;  // snapshots written (ring index = count % kRing)

double now_ms() {
    static host::Counter freq = [] { host::Counter f; host::counter_frequency(&f); return f; }();
    host::Counter t;
    host::counter(&t);
    return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}

void record(u64 scene, u32 slot) {
    static u64 count;
    u64 n = count++;
    if (!game_interp()) return;
    const u64 addr = game::scene::render_camera(scene, slot, 0);
    const game::Camera* cam = game::at<const game::Camera>(addr);
    CamSnap s;
    s.tick = n;
    s.addr = addr;
    s.t_ms = now_ms();
    memcpy(s.eye, cam->eye, 12);
    memcpy(s.at, cam->at, 12);
    memcpy(s.up, cam->up, 12);
    memcpy(s.view, cam->view, 64);
    memcpy(s.proj, cam->proj, 64);
    memcpy(s.viewproj, cam->viewproj, 64);
    s.near_ = cam->near_;
    s.far_ = cam->far_;
    s.fov = cam->fov;
    s.aspect = cam->aspect;
    s.roll = cam->roll;
    s.proj_extra = cam->proj_extra;
    s.proj_flags = cam->flags;
    std::lock_guard<std::mutex> l(g_m);
    g_ring[g_count % kRing] = s;
    g_count++;
}

}  // namespace

int snapshots(CamSnap* out, int max) {
    std::lock_guard<std::mutex> l(g_m);
    int n = 0;
    for (u64 i = g_count; i > 0 && n < max && n < kRing; i--) out[n++] = g_ring[(i - 1) % kRing];
    return n;
}

}  // namespace interp

// Hook: x0 = scene object, w1 = render buffer index (S + kRenderCamWriteSlot), used mod 3. Must leave Ctx
// exactly as the original does; only reads guest memory after it returns.
HW_HOOK(RenderCamera_Sync) {
    hw_widescreen_tick();
    u64 scene = c->x[0];
    u32 slot = (u32)c->x[1];
    HW_ORIG(RenderCamera_Sync)(c);
    interp::record(scene, slot);
}
