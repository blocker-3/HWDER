// High frame rate, stage b (docs/frame-loop.md, docs/high-framerate.md): the game draws and presents
// one extra frame per tick, with the camera halfway between the previous tick and this one.
//
// In battle, MainThread runs one tick in f_10002acb54: it waits for the update jobs (scene slots 2,
// 4 and 3, which publishes the render camera), then
//   f_100051e9c4(dev, w, h, ..)  BeginFrame         (call at 0x10002acd08)
//   f_1000387f9c(scene)          Draw (scene slot 5)
//   vcall +192 / f_100052f6bc     (when the scene asks for it)
//   f_100051ea20(dev, 1)         EndFrame
//   f_100051e67c(dev, 0)         Present: SubmitGPFIFO, QueueBuffer, DequeueBuffer
// The hook on BeginFrame runs that sequence once more first, on a copy of the registers, with screen
// 0's render camera set to lerp(tick n-1, tick n, 0.5) and rebuilt by the game's Camera::Update; then
// it restores the camera bytes and lets the real frame of tick n run unchanged. The profiler markers
// and the end-of-frame calls on the 0x1000c22a40 object (f_10001e2f7c, f_10001e301c, f_10001df06c,
// f_10001df624) are not repeated. With the display released at 120 Hz (HWDER_FPS=120) the two
// presents share each 60 Hz tick.
//
// HWDER_GAME_INTERP=1 turns it on. HWDER_GI_LOG=1 logs a summary every 5 s.
#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <atomic>

#include "hwder/runtime.h"
#include "interp/camera.h"

#pragma clang fp contract(off)

extern "C" void probe_100051e9c4(Ctx* c);  // frame_probe.cpp: the original BeginFrame, probed
extern "C" void f_100057cb78(Ctx* c);      // Camera::Update(cam, view dirty, proj dirty)
extern "C" void f_1000387f9c(Ctx* c);      // Draw
extern "C" void f_100052f6bc(Ctx* c);
extern "C" void f_100051ea20(Ctx* c);      // EndFrame
extern "C" void f_100051e67c(Ctx* c);      // Present

namespace interp {

bool game_interp() {
    static const bool on = getenv("HWDER_GAME_INTERP") && atoi(getenv("HWDER_GAME_INTERP")) != 0;
    return on;
}

}  // namespace interp

namespace {

constexpr u64 kBeginFrameRet = 0x10002ACD0CULL;  // return address of the BeginFrame call in f_10002acb54

bool g_in_extra = false;  // MainThread only
std::atomic<u64> g_last_extra_ms{0};  // GetTickCount64 of the last extra frame

struct Stats {
    u64 extra = 0, begin_failed = 0, no_pair = 0, cut = 0, mismatch = 0, no_present = 0;
    u64 pair_ticks = 0, pair_gap = 0;  // why no pair: tick not consecutive, > 250 ms apart
    u64 pair_addr = 0;                 // pairs whose ticks used different render buffer slots (blended anyway)
    u64 late = 0;                      // skipped: the previous tick ran late
    double ms = 0;
};
Stats g_st;

void report() {
    static const bool on = getenv("HWDER_GI_LOG") != nullptr;
    static u64 last = 0;
    u64 t = GetTickCount64();
    if (t - last < 5000) return;
    last = t;
    if (on)
        hw_log("gi: %llu extra frames (mean %.2f ms each, %llu without a present), skipped: %llu begin failed, "
               "%llu no previous tick (%llu not consecutive, %llu gap), %llu cut/projection change, "
               "%llu camera moved since its tick, %llu previous tick late; %llu pairs across buffer slots",
               g_st.extra, g_st.extra ? g_st.ms / g_st.extra : 0.0, g_st.no_present, g_st.begin_failed, g_st.no_pair,
               g_st.pair_ticks, g_st.pair_gap, g_st.cut, g_st.mismatch, g_st.late, g_st.pair_addr);
    g_st = Stats{};
}

float dist(const float a[3], const float b[3]) {
    float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
}

// Calls a guest function on e as the generated code would from return address ret; false when the
// callee unwound past it (a guest exception), which the caller must treat as "stop".
bool call(Ctx* e, void (*fn)(Ctx*), u64 ret) {
    e->x[30] = ret;
    fn(e);
    return e->x[30] == ret;
}

// The extra frame. c: f_10002acb54's registers at the BeginFrame call (x19 scene, x20 frame state,
// x21 graphics device, x0..x3/v0 BeginFrame's arguments).
void extra_frame(const Ctx* c) {
    interp::CamSnap s[2];
    // The two ticks may sit in different render buffer slots (S + 0x710dc rotates at times): only their
    // values are needed; the camera written is tick n's, checked below to be unchanged.
    if (interp::snapshots(s, 2) < 2 || s[0].tick != s[1].tick + 1 || s[0].t_ms - s[1].t_ms > 250.0) {
        g_st.no_pair++;
        if (s[0].tick != s[1].tick + 1) g_st.pair_ticks++;
        else g_st.pair_gap++;
        return;
    }
    if (s[0].addr != s[1].addr) g_st.pair_addr++;
    const interp::CamSnap &a = s[1], &b = s[0];
    if (dist(a.eye, b.eye) > 500.0f || dist(a.at, b.at) > 500.0f || a.fov != b.fov || a.aspect != b.aspect ||
        a.near_ != b.near_ || a.far_ != b.far_ || a.proj_extra != b.proj_extra || a.proj_flags != b.proj_flags) {
        g_st.cut++;
        return;
    }
    u8* cam = GUESTP(u8, b.addr);
    if (memcmp(cam + 0x10, b.eye, 12) || memcmp(cam + 0x20, b.at, 12) || memcmp(cam + 0x100, b.viewproj, 64)) {
        g_st.mismatch++;
        return;
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    // Screen 0's render camera halfway between the ticks, rebuilt by the game's own Camera::Update.
    alignas(16) u8 saved[0x170];
    memcpy(saved, cam, sizeof saved);
    float eye[3], at[3], up[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * 0.5f;
        at[i] = a.at[i] + (b.at[i] - a.at[i]) * 0.5f;
        up[i] = a.up[i] + (b.up[i] - a.up[i]) * 0.5f;
    }
    float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (ul > 0.0f)
        for (float& u : up) u /= ul;
    float roll = a.roll + (b.roll - a.roll) * 0.5f;
    memcpy(cam + 0x10, eye, 12);
    memcpy(cam + 0x20, at, 12);
    memcpy(cam + 0x30, up, 12);
    memcpy(cam + 0x150, &roll, 4);
    Ctx u = *c;
    u.x[0] = b.addr;
    u.x[1] = 1;
    u.x[2] = 1;
    call(&u, f_100057cb78, 0);

    g_in_extra = true;
    Ctx e = *c;
    const u64 scene = c->x[19], state = c->x[20], dev = c->x[21];
    bool ok = call(&e, probe_100051e9c4, kBeginFrameRet) && (u8)e.x[0] != 0;
    if (!ok) g_st.begin_failed++;
    if (ok) {
        e.x[0] = scene;
        ok = call(&e, f_1000387f9c, 0x10002ACD38ULL);
    }
    if (ok) {
        // vcall scene+192: does the scene want f_100052f6bc this frame
        e.x[0] = scene;
        u64 fn = *GUESTP(u64, *GUESTP(u64, scene) + 192);
        e.x[30] = 0x10002ACDB0ULL;
        hw_call(&e, fn);
        ok = e.x[30] == 0x10002ACDB0ULL;
        if (ok && (u8)e.x[0] != 0) {
            u64 g = *GUESTP(u64, *GUESTP(u64, 0x1000C21DE0ULL));
            e.x[3] = *GUESTP(u64, *GUESTP(u64, g + 64) + 88);
            e.x[0] = dev;
            e.x[1] = 0;
            e.x[2] = 0;
            e.x[4] = 0;
            ok = call(&e, f_100052f6bc, 0x10002ACDE0ULL);
        }
    }
    if (ok) {
        e.x[0] = dev;
        e.x[1] = 1;
        ok = call(&e, f_100051ea20, 0x10002ACDECULL);
    }
    if (ok) {
        u64 skip_present = *GUESTP(u8, *GUESTP(u64, state + 544) + 0x2d420);
        if (skip_present) {
            g_st.no_present++;
        } else {
            e.x[0] = dev;
            e.x[1] = 0;
            call(&e, f_100051e67c, 0x10002ACE0CULL);
        }
    }
    g_in_extra = false;

    memcpy(cam, saved, sizeof saved);
    QueryPerformanceCounter(&t1);
    g_last_extra_ms = GetTickCount64();
    g_st.extra++;
    g_st.ms += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

}  // namespace

// display.cpp's frame limiter: with HWDER_GAME_INTERP on, release frames at 60 Hz whenever the game
// has not drawn an in-between frame for 100 ms (menus, loading, pause), so frame-locked screens keep
// their speed.
extern "C" bool hw_game_interp_hold_60() {
    return interp::game_interp() && GetTickCount64() - g_last_extra_ms.load() > 100;
}

// Hook (tools/recomp/hooks.txt): BeginFrame. Only the battle tick's call (from f_10002acb54) gets
// the extra frame; menus and loading screens draw through other paths and stay at one frame per tick.
extern "C" void f_100051e9c4(Ctx* c) {
    if (interp::game_interp() && !g_in_extra && c->x[30] == kBeginFrameRet) {
        // Game speed first: the game is frame-locked, so when the previous tick (BeginFrame to BeginFrame)
        // took longer than its 16.7 ms the extra frame is what made it late. Skip one, then try again.
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        static LARGE_INTEGER prev;
        static bool skipped_last = false;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = prev.QuadPart ? (now.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart : 0.0;
        prev = now;
        if (dt > 17.5 && dt < 100.0 && !skipped_last) {
            g_st.late++;
            skipped_last = true;
        } else {
            skipped_last = false;
            extra_frame(c);
        }
        report();
    }
    probe_100051e9c4(c);
}
