// HWDER high frame rate: find the game's camera and record it once per game tick.
//
// The game keeps its render cameras triple-buffered in a scene object S: two screens (0x170 bytes
// each) per buffer, three buffers selected by an index at S + 0x710dc (mostly 0 in practice).
// f_10003d45cc(S, slot) runs once per game tick: it copies each active screen's game camera into
// buffer slot % 3 and rebuilds its matrices (Camera::Update, 0x100057cb78). Hooking it gives
// exactly one finished camera per tick, which is what interpolation needs to record. See docs/camera.md for the layout.
//
// Each tick's screen-0 camera goes into a small ring that stage c reads (interp_draw.cpp, vk_renderer.cpp).
#include "interp/camera.h"

#include <windows.h>

#include <cstring>
#include <mutex>

#include "hwder/runtime.h"

namespace interp {
namespace {

constexpr u64 kSlotBase = 0x71360;  // S + kSlotBase + (slot % 3) * kBufStride + screen * 0x170
constexpr u64 kBufStride = 0x2e0;
constexpr int kRing = 8;

std::mutex g_m;
CamSnap g_ring[kRing];
u64 g_count;  // snapshots written (ring index = count % kRing)

double now_ms() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}

void record(u64 scene, u32 slot) {
    static u64 count;
    u64 n = count++;
    if (!game_interp()) return;
    const u8* cam = GUESTP(u8, scene + kSlotBase + (slot % 3) * kBufStride);
    CamSnap s;
    s.tick = n;
    s.addr = scene + kSlotBase + (slot % 3) * kBufStride;
    s.t_ms = now_ms();
    memcpy(s.eye, cam + 0x10, 12);
    memcpy(s.at, cam + 0x20, 12);
    memcpy(s.up, cam + 0x30, 12);
    memcpy(s.view, cam + 0x80, 64);
    memcpy(s.proj, cam + 0xc0, 64);
    memcpy(s.viewproj, cam + 0x100, 64);
    const float* f = (const float*)(cam + 0x140);  // near, far, fovy (rad), aspect, roll, extra
    s.near_ = f[0];
    s.far_ = f[1];
    s.fov = f[2];
    s.aspect = f[3];
    s.roll = f[4];
    s.proj_extra = f[5];
    memcpy(&s.proj_flags, cam + 0x158, 4);
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

extern "C" void f_10003d45cc_orig(Ctx* c);

// Hook (tools/recomp/hooks.txt): x0 = scene object, w1 = render buffer index (S + 0x710dc), used
// mod 3. Must leave Ctx exactly as the original does; only reads guest memory after it returns.
extern "C" void hw_widescreen_tick();  // game/widescreen.cpp

extern "C" void f_10003d45cc(Ctx* c) {
    hw_widescreen_tick();
    u64 scene = c->x[0];
    u32 slot = (u32)c->x[1];
    f_10003d45cc_orig(c);
    interp::record(scene, slot);
}
