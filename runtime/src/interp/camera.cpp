// HWDER high frame rate: find the game's camera and record it once per game tick.
//
// The game keeps its render cameras triple-buffered in a scene object S: two screens (0x170 bytes
// each) per buffer, three buffers selected by an index at S + 0x710dc (mostly 0 in practice).
// f_10003d45cc(S, slot) runs once per game tick: it copies each active screen's game camera into
// buffer slot % 3 and rebuilds its matrices (Camera::Update, 0x100057cb78). Hooking it gives
// exactly one finished camera per tick, which is what interpolation needs to record. See docs/camera.md for the layout.
//
// Each tick's screen-0 camera goes into a small ring that the renderer reads (camera interpolation,
// HWDER_INTERP_MODE=camera). HWDER_CAM_LOG=<n> logs screen 0's camera every n ticks (1 = every tick).
#include "interp/camera.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hwder/runtime.h"

// The game's float math is separate multiplies and adds (plus explicit FMAs in Camera::Update): never
// let the compiler fuse them, or the rebuilt view no longer matches the tick's bytes.
#pragma clang fp contract(off)

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

float max_abs_diff(const float* a, const float* b, int n, int* exact) {
    float m = 0;
    bool same = true;
    for (int i = 0; i < n; i++) {
        m = std::fmax(m, std::fabs(a[i] - b[i]));
        if (memcmp(&a[i], &b[i], 4) != 0) same = false;
    }
    if (same) (*exact)++;
    return m;
}

// Check build_view/mul_view_proj against the game's own bytes on every tick; log after the first 60
// ticks, then every 3600 (about a minute).
void verify(const CamSnap& s) {
    static float max_view, max_vp;
    static int n, exact_view, exact_vp, rolled;
    float v[16], vp[16];
    build_view(s.eye, s.at, s.up, s.roll, v);
    mul_view_proj(s.view, s.proj, vp);
    max_view = std::fmax(max_view, max_abs_diff(v, s.view, 16, &exact_view));
    max_vp = std::fmax(max_vp, max_abs_diff(vp, s.viewproj, 16, &exact_vp));
    if (s.roll != 0.0f) rolled++;
    n++;
    if (n == 60 || n % 3600 == 0)
        hw_log("cam: BuildView reproduction over %d ticks: view max abs error %g (%d bit-exact), "
               "viewProj max abs error %g (%d bit-exact), %d ticks with roll (host sincos)",
               n, max_view, exact_view, max_vp, exact_vp, rolled);
}

void record(u64 scene, u32 slot) {
    static u64 count;
    u64 n = count++;
    int every = cam_log_every();
    if (!camera_mode() && every <= 0 && !game_interp()) return;
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
    {
        std::lock_guard<std::mutex> l(g_m);
        g_ring[g_count % kRing] = s;
        g_count++;
    }
    verify(s);
    if (every > 0 && n % (u64)every == 0)
        hw_log("cam: tick %llu slot %u t=%.3fms eye (%.2f %.2f %.2f) at (%.2f %.2f %.2f) "
               "up (%.3f %.3f %.3f) fov %.2f aspect %.3f near %.2f far %.1f roll %.3f",
               (unsigned long long)n, slot % 3, s.t_ms, s.eye[0], s.eye[1], s.eye[2], s.at[0], s.at[1], s.at[2],
               s.up[0], s.up[1], s.up[2], s.fov * 57.29578f, s.aspect, s.near_, s.far_, s.roll);
}

// main's rodata used by the inverse below (shuffle tables at 0x10007e2070, signs at 0x100073e0f0),
// at the same offsets from the table base as in the game.
alignas(16) const u8 kRodataE2[160] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 5, 6, 7, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
    8, 9, 10, 11, 8, 9, 10, 11, 4, 5, 6, 7, 4, 5, 6, 7, 12, 13, 14, 15, 12, 13, 14, 15, 12, 13, 14, 15, 8, 9, 10, 11,
};
alignas(16) const u8 kRodata73E[272] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 63, 0, 0, 128, 191, 0, 0, 128, 63, 0, 0, 128, 191,
    0, 0, 128, 191, 0, 0, 128, 63, 0, 0, 128, 191, 0, 0, 128, 63,
};

}  // namespace

// The inverse view the game uploads as matV2W_1..4 (f_10003d0a74, 0x10003d0c14..0x10003d0e04),
// transcribed from the generated code so the result is bit-identical to what the shaders see.
void view_inverse(const float view[16], float res[16]) {
    alignas(16) u8 cambuf[192] = {};
    memcpy(cambuf + 128, view, 64);
    const u64 cam = (u64)(uintptr_t)cambuf, out = (u64)(uintptr_t)res;
    Ctx cc;
    memset(&cc, 0, sizeof(cc));
    Ctx* c = &cc;
      { u64 a_ = cam + 128; c->v[0] = LD128(a_); c->v[1] = LD128(a_ + 16);  }
      { u64 a_ = cam + 160; c->v[2] = LD128(a_); c->v[3] = LD128(a_ + 16);  }
      { V128 a_ = c->v[1], b_ = c->v[3], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[0 + i]; r_.s[2 * i + 1] = b_.s[0 + i]; } c->v[5] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[3], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[2 + i]; r_.s[2 * i + 1] = b_.s[2 + i]; } c->v[1] = r_; }
      { V128 a_ = c->v[0], b_ = c->v[2], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[0 + i]; r_.s[2 * i + 1] = b_.s[0 + i]; } c->v[4] = r_; }
      c->x[8] = (u64)(uintptr_t)kRodataE2;
      { u64 a_ = c->x[8] + 112; c->v[17] = V_ZERO; c->v[17].d[0] = LD64(a_); }
      c->x[8] = (u64)(uintptr_t)kRodataE2;
      { V128 a_ = c->v[0], b_ = c->v[2], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[2 + i]; r_.s[2 * i + 1] = b_.s[2 + i]; } c->v[2] = r_; }
      { V128 a_ = c->v[4], b_ = c->v[5], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[0 + i]; r_.s[2 * i + 1] = b_.s[0 + i]; } c->v[0] = r_; }
      { u64 a_ = c->x[8] + 120; c->v[19] = V_ZERO; c->v[19].d[0] = LD64(a_); }
      c->x[8] = (u64)(uintptr_t)kRodataE2;
      { u64 a_ = c->x[8] + 128; c->v[22] = V_ZERO; c->v[22].d[0] = LD64(a_); }
      c->x[8] = (u64)(uintptr_t)kRodataE2;
      { u64 a_ = c->x[8] + 136; c->v[23] = V_ZERO; c->v[23].d[0] = LD64(a_); }
      c->x[8] = (u64)(uintptr_t)kRodataE2;
      { V128 a_ = c->v[4], b_ = c->v[5], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[2 + i]; r_.s[2 * i + 1] = b_.s[2 + i]; } c->v[7] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[1], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[0 + i]; r_.s[2 * i + 1] = b_.s[0 + i]; } c->v[21] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[1], r_ = V_ZERO; for (int i = 0; i < 2; i++) { r_.s[2 * i] = a_.s[2 + i]; r_.s[2 * i + 1] = b_.s[2 + i]; } c->v[5] = r_; }
      { V128 t_[1] = { c->v[0] }; V128 i_ = c->v[17], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[1] = r_; }
      { V128 t_[1] = { c->v[0] }; V128 i_ = c->v[19], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[2] = r_; }
      { u64 a_ = c->x[8] + 144; c->v[24] = V_ZERO; c->v[24].d[0] = LD64(a_); }
      c->x[8] = (u64)(uintptr_t)kRodataE2;
      { u64 a_ = c->x[8] + 152; c->v[25] = V_ZERO; c->v[25].d[0] = LD64(a_); }
      { V128 t_[1] = { c->v[0] }; V128 i_ = c->v[23], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[3] = r_; }
      c->v[1].d[1] = c->v[2].d[0];
      { V128 t_[1] = { c->v[0] }; V128 i_ = c->v[22], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[2] = r_; }
      c->v[2].d[1] = c->v[3].d[0];
      { V128 t_[1] = { c->v[0] }; V128 i_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[3] = r_; }
      { V128 t_[1] = { c->v[0] }; V128 i_ = c->v[25], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[4] = r_; }
      c->v[3].d[1] = c->v[4].d[0];
      { V128 t_[1] = { c->v[7] }; V128 i_ = c->v[17], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[4] = r_; }
      { V128 t_[1] = { c->v[7] }; V128 i_ = c->v[19], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[6] = r_; }
      { V128 t_[1] = { c->v[7] }; V128 i_ = c->v[23], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[16] = r_; }
      c->v[4].d[1] = c->v[6].d[0];
      { V128 t_[1] = { c->v[7] }; V128 i_ = c->v[22], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[6] = r_; }
      { V128 t_[1] = { c->v[7] }; V128 i_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[18] = r_; }
      { V128 t_[1] = { c->v[7] }; V128 i_ = c->v[25], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[7] = r_; }
      { V128 t_[1] = { c->v[21] }; V128 i_ = c->v[23], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[20] = r_; }
      c->v[6].d[1] = c->v[16].d[0];
      { V128 t_[1] = { c->v[21] }; V128 i_ = c->v[19], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[16] = r_; }
      c->v[18].d[1] = c->v[7].d[0];
      { V128 t_[1] = { c->v[21] }; V128 i_ = c->v[17], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[7] = r_; }
      { V128 t_[1] = { c->v[5] }; V128 i_ = c->v[17], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[17] = r_; }
      c->v[7].d[1] = c->v[16].d[0];
      { V128 t_[1] = { c->v[21] }; V128 i_ = c->v[22], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[16] = r_; }
      c->v[16].d[1] = c->v[20].d[0];
      { V128 t_[1] = { c->v[21] }; V128 i_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[20] = r_; }
      { V128 t_[1] = { c->v[21] }; V128 i_ = c->v[25], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[21] = r_; }
      { V128 t_[1] = { c->v[5] }; V128 i_ = c->v[19], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[19] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[6], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[26] = r_; }
      c->x[8] = (u64)(uintptr_t)kRodata73E;
      c->v[20].d[1] = c->v[21].d[0];
      { V128 t_[1] = { c->v[5] }; V128 i_ = c->v[22], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[21] = r_; }
      { V128 t_[1] = { c->v[5] }; V128 i_ = c->v[23], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[22] = r_; }
      c->v[17].d[1] = c->v[19].d[0];
      { V128 t_[1] = { c->v[5] }; V128 i_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[19] = r_; }
      { V128 t_[1] = { c->v[5] }; V128 i_ = c->v[25], r_ = V_ZERO; for (int i = 0; i < 8; i++) { u32 x_ = i_.b[i]; if (x_ < 16) r_.b[i] = t_[x_ >> 4].b[x_ & 15]; } c->v[5] = r_; }
      { u64 a_ = c->x[8] + 240; c->v[25] = LD128(a_); }
      c->v[21].d[1] = c->v[22].d[0];
      c->x[8] = (u64)(uintptr_t)kRodata73E;
      c->v[19].d[1] = c->v[5].d[0];
      { V128 a_ = c->v[16], b_ = c->v[17], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[24] = r_; }
      { V128 a_ = c->v[20], b_ = c->v[17], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[23] = r_; }
      { V128 a_ = c->v[16], b_ = c->v[19], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[5] = r_; }
      { V128 a_ = c->v[20], b_ = c->v[21], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[22] = r_; }
      { V128 a_ = c->v[5], b_ = c->v[22], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] - b_.f[i]; c->v[5] = r_; }
      { V128 a_ = c->v[7], b_ = c->v[19], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[22] = r_; }
      { V128 a_ = c->v[22], b_ = c->v[23], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] - b_.f[i]; c->v[22] = r_; }
      { V128 a_ = c->v[7], b_ = c->v[21], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[23] = r_; }
      { V128 a_ = c->v[23], b_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] - b_.f[i]; c->v[23] = r_; }
      { V128 a_ = c->v[4], b_ = c->v[5], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[24] = r_; }
      { V128 a_ = c->v[6], b_ = c->v[22], d_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(-a_.f[i], b_.f[i], d_.f[i]); c->v[24] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[5], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[5] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[22], d_ = c->v[5], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(-a_.f[i], b_.f[i], d_.f[i]); c->v[5] = r_; }
      { u64 a_ = c->x[8] + 256; c->v[22] = LD128(a_); }
      { V128 a_ = c->v[23], b_ = c->v[18], d_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(a_.f[i], b_.f[i], d_.f[i]); c->v[24] = r_; }
      { V128 a_ = c->v[23], b_ = c->v[3], d_ = c->v[5], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(a_.f[i], b_.f[i], d_.f[i]); c->v[5] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[18], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[23] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[18], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[18] = r_; }
      { V128 a_ = c->v[24], b_ = c->v[25], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[24] = r_; }
      { V128 a_ = c->v[23], b_ = c->v[26], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] - b_.f[i]; c->v[23] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[4], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[3] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[6], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[1] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[4], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[2] = r_; }
      { V128 a_ = c->v[18], b_ = c->v[3], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] - b_.f[i]; c->v[3] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[2], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] - b_.f[i]; c->v[1] = r_; }
      { V128 a_ = c->v[23], b_ = c->v[17], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[2] = r_; }
      { V128 a_ = c->v[21], b_ = c->v[3], d_ = c->v[2], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(-a_.f[i], b_.f[i], d_.f[i]); c->v[2] = r_; }
      { V128 a_ = c->v[7], b_ = c->v[23], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[4] = r_; }
      { V128 a_ = c->v[16], b_ = c->v[3], d_ = c->v[4], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(-a_.f[i], b_.f[i], d_.f[i]); c->v[4] = r_; }
      { V128 a_ = c->v[0], b_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[0] = r_; }
      { V128 a_ = c->v[0], b_ = c->v[0], r_ = V_ZERO; u8 t_[32]; memcpy(t_, a_.b, 16); memcpy(t_ + 16, b_.b, 16); memcpy(r_.b, t_ + 8, 16); c->v[3] = r_; }
      { V128 a_ = c->v[5], b_ = c->v[22], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[5] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 2; i++) r_.f[i] = a_.f[i] + b_.f[i]; c->v[0] = r_; }
      { V128 a_ = c->v[0], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 1; i++) { r_.f[i] = a_.f[2 * i] + a_.f[2 * i + 1]; r_.f[1 + i] = b_.f[2 * i] + b_.f[2 * i + 1]; } c->v[0] = r_; }
      c->v[0].d[1] = c->v[0].d[0];
      { V128 a_ = c->v[1], b_ = c->v[20], d_ = c->v[4], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(a_.f[i], b_.f[i], d_.f[i]); c->v[4] = r_; }
      { V128 a_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = (float)1.0 / a_.f[i]; c->v[3] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[19], d_ = c->v[2], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FMAF(a_.f[i], b_.f[i], d_.f[i]); c->v[2] = r_; }
      { V128 a_ = c->v[4], b_ = c->v[22], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[1] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FRECPSF(a_.f[i], b_.f[i]); c->v[4] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[25], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[2] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[4], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[3] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = FRECPSF(a_.f[i], b_.f[i]); c->v[4] = r_; }
      { V128 a_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.s[i] = (a_.f[i] == 0) ? (u32)~(u32)0 : 0; c->v[0] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[4], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[3] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[24], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[4] = r_; }
      { V128 a_ = c->v[3], b_ = c->v[5], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[5] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[3], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[2] = r_; }
      { V128 a_ = c->v[2], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 2; i++) r_.d[i] = a_.d[i] & ~b_.d[i]; c->v[2] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[3], r_ = V_ZERO; for (int i = 0; i < 4; i++) r_.f[i] = a_.f[i] * b_.f[i]; c->v[1] = r_; }
      { V128 a_ = c->v[4], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 2; i++) r_.d[i] = a_.d[i] & ~b_.d[i]; c->v[3] = r_; }
      { V128 a_ = c->v[5], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 2; i++) r_.d[i] = a_.d[i] & ~b_.d[i]; c->v[4] = r_; }
      { V128 a_ = c->v[1], b_ = c->v[0], r_ = V_ZERO; for (int i = 0; i < 2; i++) r_.d[i] = a_.d[i] & ~b_.d[i]; c->v[0] = r_; }
      { u64 a_ = out + 0; ST128(a_, c->v[3]); ST128(a_ + 16, c->v[4]);  }
      { u64 a_ = out + 32; ST128(a_, c->v[2]); ST128(a_ + 16, c->v[0]);  }
}

namespace {

float dist(const float a[3], const float b[3]) {
    float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
}

}  // namespace

bool camera_mode() {
    static const bool on = [] {
        const char* e = getenv("HWDER_INTERP_MODE");
        return e && _stricmp(e, "camera") == 0;
    }();
    return on;
}

int cam_log_every() {
    static const int n = [] {
        const char* e = getenv("HWDER_CAM_LOG");
        return e ? atoi(e) : 0;
    }();
    return n;
}

int snapshots(CamSnap* out, int max) {
    std::lock_guard<std::mutex> l(g_m);
    int n = 0;
    for (u64 i = g_count; i > 0 && n < max && n < kRing; i--) out[n++] = g_ring[(i - 1) % kRing];
    return n;
}

// Camera::BuildView, operation for operation (generated/rc_main_057.c, f_100057cc3c). Row-vector
// convention: the axes are columns 0..2, the translation is row 3.
void build_view(const float eye[3], const float at[3], const float up[3], float roll, float m[16]) {
    float dx = eye[0] - at[0], dy = eye[1] - at[1], dz = eye[2] - at[2];
    float l = dx * dx;
    l = l + dy * dy;
    l = l + dz * dz;
    float inv = 1.0f / std::sqrt(l);
    float fx = dx * inv, fy = dy * inv, fz = dz * inv;
    // right = up x d (d unnormalised), then normalised
    float rx = dz * up[1] - dy * up[2];
    float ry = dx * up[2] - dz * up[0];
    float rz = dy * up[0] - dx * up[1];
    float r2 = rx * rx;
    r2 = r2 + ry * ry;
    r2 = rz * rz + r2;
    float rinv = 1.0f / std::sqrt(r2);
    rx = rx * rinv;
    ry = ry * rinv;
    rz = rz * rinv;
    // true up = f x r
    float ux = rz * fy - ry * fz;
    float uy = rx * fz - rz * fx;
    float uz = ry * fx - rx * fy;
    float fe = fx * eye[0];
    fe = fe + fy * eye[1];
    fe = fe + fz * eye[2];
    float re = rx * eye[0];
    re = re + ry * eye[1];
    re = re + rz * eye[2];
    float ue = ux * eye[0];
    ue = ue + uy * eye[1];
    ue = ue + uz * eye[2];
    const float b[16] = {rx, ux, fx, 0, ry, uy, fy, 0, rz, uz, fz, 0, -re, -ue, -fe, 1.0f};
    if (roll == 0.0f) {
        memcpy(m, b, 64);
        return;
    }
    // The game uses its own sincosf (0x100000fd70); the host's may differ in the last bit.
    float s = std::sin(-roll), c = std::cos(-roll);
    for (int i = 0; i < 4; i++) {
        float b0 = b[i * 4], b1 = b[i * 4 + 1];
        m[i * 4] = b0 * c - b1 * s;
        m[i * 4 + 1] = b1 * c + b0 * s;
        m[i * 4 + 2] = b[i * 4 + 2];
        m[i * 4 + 3] = b[i * 4 + 3];
    }
}

// Camera::Update: viewProj row r is an FMA chain over the proj rows with view row r's elements as scalars.
void mul_view_proj(const float v[16], const float p[16], float out[16]) {
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < 4; i++) {
            float acc = p[i] * v[r * 4];
            acc = std::fma(p[4 + i], v[r * 4 + 1], acc);
            acc = std::fma(p[8 + i], v[r * 4 + 2], acc);
            acc = std::fma(p[12 + i], v[r * 4 + 3], acc);
            out[r * 4 + i] = acc;
        }
}

bool lerp_camera(const CamSnap& a, const CamSnap& b, float alpha, float view[16], float viewproj[16]) {
    if (alpha >= 1.0f || b.tick != a.tick + 1 || b.t_ms - a.t_ms > 250.0) return false;
    if (dist(a.eye, b.eye) > 500.0f || dist(a.at, b.at) > 500.0f) return false;  // a cut
    if (a.near_ != b.near_ || a.far_ != b.far_ || a.fov != b.fov || a.aspect != b.aspect ||
        a.proj_extra != b.proj_extra || a.proj_flags != b.proj_flags || memcmp(a.proj, b.proj, 64) != 0)
        return false;
    if (alpha < 0.0f) alpha = 0.0f;
    // b - (b - a) * (1 - alpha): exactly b at alpha = 1
    float w = 1.0f - alpha;
    float eye[3], at[3], up[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = b.eye[i] - (b.eye[i] - a.eye[i]) * w;
        at[i] = b.at[i] - (b.at[i] - a.at[i]) * w;
        up[i] = b.up[i] - (b.up[i] - a.up[i]) * w;
    }
    float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(ul > 0.0f)) return false;
    for (float& u : up) u /= ul;
    float roll = b.roll - (b.roll - a.roll) * w;
    build_view(eye, at, up, roll, view);
    mul_view_proj(view, b.proj, viewproj);
    return true;
}

}  // namespace interp

extern "C" void f_10003d45cc_orig(Ctx* c);

// Hook (tools/recomp/hooks.txt): x0 = scene object, w1 = render buffer index (S + 0x710dc), used
// mod 3. Must leave Ctx exactly as the original does; only reads guest memory after it returns.
extern "C" void f_10003d45cc(Ctx* c) {
    u64 scene = c->x[0];
    u32 slot = (u32)c->x[1];
    f_10003d45cc_orig(c);
    interp::record(scene, slot);
}
