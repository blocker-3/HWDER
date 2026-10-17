// HWDER high frame rate, step 1: find the game's camera and record it once per game tick.
//
// The game keeps its render cameras triple-buffered in a scene object S: two screens (0x170 bytes
// each) per buffer, three buffers selected by an index at S + 0x710dc (mostly 0 in practice).
// f_10003d45cc(S, slot) runs once per game tick: it copies each active screen's game camera into
// buffer slot % 3 and rebuilds its matrices (Camera::Update, 0x100057cb78). Hooking it gives
// exactly one finished camera per tick, which is what interpolation needs to record. See docs/camera.md for the layout.
//
// HWDER_CAM_LOG=<n> logs screen 0's camera every n ticks (1 = every tick). No behaviour change.
#include <windows.h>

#include <cstdlib>

#include "hwder/runtime.h"

namespace {

constexpr u64 kSlotBase = 0x71360;  // S + kSlotBase + (slot % 3) * kBufStride + screen * 0x170
constexpr u64 kBufStride = 0x2e0;

struct GuestCamera {  // the prefix of the guest camera the interpolator needs
    float eye[3], pad0;     // +0x10
    float at[3], pad1;      // +0x20
    float up[3], pad2;      // +0x30
};

int log_every() {
    static const int n = [] {
        const char* e = getenv("HWDER_CAM_LOG");
        return e ? atoi(e) : 0;
    }();
    return n;
}

double now_ms() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}

void record(u64 scene, u32 slot) {
    static u64 count;
    int every = log_every();
    if (every <= 0) return;
    u64 n = count++;
    if (n % (u64)every) return;
    u8* cam = GUESTP(u8, scene + kSlotBase + (slot % 3) * kBufStride);
    const GuestCamera* g = (const GuestCamera*)(cam + 0x10);
    const float* f = (const float*)(cam + 0x140);  // near, far, fovy (rad), aspect, roll
    hw_log("cam: tick %llu slot %u t=%.3fms eye (%.2f %.2f %.2f) at (%.2f %.2f %.2f) "
           "up (%.3f %.3f %.3f) fov %.2f aspect %.3f near %.2f far %.1f roll %.3f",
           (unsigned long long)n, slot % 3, now_ms(), g->eye[0], g->eye[1], g->eye[2], g->at[0], g->at[1],
           g->at[2], g->up[0], g->up[1], g->up[2], f[2] * 57.29578f, f[3], f[0], f[1], f[4]);
}

}  // namespace

extern "C" void f_10003d45cc_orig(Ctx* c);

// Hook (tools/recomp/hooks.txt): x0 = scene object, w1 = render buffer index (S + 0x710dc), used
// mod 3. Must leave Ctx exactly as the original does; only reads guest memory after it returns.
extern "C" void f_10003d45cc(Ctx* c) {
    u64 scene = c->x[0];
    u32 slot = (u32)c->x[1];
    f_10003d45cc_orig(c);
    record(scene, slot);
}
