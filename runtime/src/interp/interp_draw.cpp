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
// f_10001df624) are not repeated. Each queued extra/paired-real frame carries a fixed 120 Hz release
// rate, while unpaired frames and menus carry native 60 Hz pacing. HWDER_FPS is ignored in this mode.
//
// Settings > Display enables it; HWDER_GAME_INTERP=0/1 overrides that request for tests.
// HWDER_GI_LOG=1 logs a summary every 5 s.
#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <string>
#include <mutex>
#include <map>

#include "hwder/runtime.h"
#include "gpu/settings.h"
#include "interp/camera.h"

#pragma clang fp contract(off)

extern "C" void f_100051e9c4_orig(Ctx* c);  // the original BeginFrame
extern "C" void f_10002acb54_orig(Ctx* c);  // the original battle tick
extern "C" void f_100057cb78(Ctx* c);      // Camera::Update(cam, view dirty, proj dirty)
extern "C" void f_1000387f9c(Ctx* c);      // Draw
extern "C" void f_100052f6bc(Ctx* c);
extern "C" void f_100051ea20(Ctx* c);      // EndFrame
extern "C" void f_100051e67c(Ctx* c);      // Present

// Tag of the GPU work being submitted: (tick << 1) | 1 during an extra frame (nvdrv copies it into each
// submission; the renderer reads it back while executing).
extern "C" std::atomic<unsigned long long> hw_gi_submit_tag{0};
extern "C" std::atomic<u32> hw_last_frame_draws;  // gpu/vk: draws in the last presented frame

namespace interp {

// Settings > Display > "120 fps" (settings::video().fps120, switchable while playing); HWDER_GAME_INTERP=0/1
// overrides it for tests.
bool game_interp() {
    static const int env = getenv("HWDER_GAME_INTERP") ? (atoi(getenv("HWDER_GAME_INTERP")) != 0) : -1;
    const bool enabled = env >= 0 ? env != 0 : settings::video().fps120.load(std::memory_order_relaxed);
    static std::atomic<bool> announced{false};
    if (enabled && !announced.exchange(true, std::memory_order_relaxed))
        hw_log("gi: game-drawn frames enabled: paired battle frames release at 120 Hz, unpaired screens at native "
               "60 Hz; HWDER_FPS and HWDER_FORCE_INTERVAL1 overrides ignored");
    return enabled;
}

}  // namespace interp

namespace {

constexpr u64 kBeginFrameRet = 0x10002ACD0CULL;  // return address of the BeginFrame call in f_10002acb54

thread_local bool g_in_extra = false;  // recursion guard and same-thread IPC diagnostic scope
thread_local bool g_paired_real = false;  // this tick actually queued an extra present
thread_local bool g_frame_gi = false;     // UI request latched at this thread's non-extra BeginFrame
thread_local bool g_frame_started = false;
u64 g_tick = 0;           // newest published tick when the current frame began

struct Stats {
    u64 extra = 0, begin_failed = 0, no_pair = 0, cut = 0, mismatch = 0, no_present = 0;
    u64 pair_ticks = 0, pair_gap = 0;  // why no pair: tick not consecutive, > 250 ms apart
    u64 pair_addr = 0;                 // pairs whose ticks used different render buffer slots (blended anyway)
    u64 late = 0;                      // skipped: the previous tick ran late
    // Per-tick timing (ms): tick entry -> BeginFrame (update jobs), extra frame's draw part and present
    // part (includes DequeueBuffer's wait), BeginFrame -> tick exit (real frame), tick exit -> next entry.
    double t_update = 0, t_xdraw = 0, t_xpresent = 0, t_real = 0, t_loop = 0, draws = 0;
    double m_update = 0, m_xdraw = 0, m_xpresent = 0, m_real = 0, m_loop = 0;
    u64 ticks = 0;
    double ms = 0;
};
Stats g_st;

void report_ipc();

void report() {
    static const bool on = getenv("HWDER_GI_LOG") != nullptr;
    static u64 last = 0;
    u64 t = GetTickCount64();
    static const u64 every = getenv("HWDER_GI_LOG_MS") ? strtoull(getenv("HWDER_GI_LOG_MS"), nullptr, 10) : 5000;
    if (t - last < every) return;
    last = t;
    if (on)
        hw_log("gi: %llu extra frames (mean %.2f ms each, %llu without a present), skipped: %llu begin failed, "
               "%llu no previous tick (%llu not consecutive, %llu gap), %llu cut/projection change, "
               "%llu camera moved since its tick, %llu behind the 60 Hz grid; %llu pairs across buffer slots",
               g_st.extra, g_st.extra ? g_st.ms / g_st.extra : 0.0, g_st.no_present, g_st.begin_failed, g_st.no_pair,
               g_st.pair_ticks, g_st.pair_gap, g_st.cut, g_st.mismatch, g_st.late, g_st.pair_addr);
    if (on && g_st.ticks)
        hw_log("gi: per tick (mean/max ms): update %.2f/%.1f, extra draw %.2f/%.1f, extra present %.2f/%.1f, "
               "real frame %.2f/%.1f, loop %.2f/%.1f; %.0f draws per frame", g_st.t_update / g_st.ticks, g_st.m_update,
               g_st.t_xdraw / g_st.ticks, g_st.m_xdraw, g_st.t_xpresent / g_st.ticks, g_st.m_xpresent,
               g_st.t_real / g_st.ticks, g_st.m_real, g_st.t_loop / g_st.ticks, g_st.m_loop, g_st.draws / g_st.ticks);
    g_st = Stats{};
    if (on) report_ipc();
}

double now_ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}
void add(double& sum, double& mx, double v) {
    sum += v;
    if (v > mx) mx = v;
}
double g_tick_in = 0, g_tick_out = 0, g_begin = 0, g_xdraw = 0, g_xpresent = 0;

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

    hw_gi_submit_tag = (b.tick << 1) | 1;  // the renderer blends this frame's object transforms (stage c)
    g_in_extra = true;
    Ctx e = *c;
    const u64 scene = c->x[19], state = c->x[20], dev = c->x[21];
    bool ok = call(&e, f_100051e9c4_orig, kBeginFrameRet) && (u8)e.x[0] != 0;
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
    g_xdraw = now_ms();
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
    hw_gi_submit_tag = b.tick << 1;
    g_xpresent = now_ms();

    memcpy(cam, saved, sizeof saved);
    QueryPerformanceCounter(&t1);
    g_st.extra++;
    g_st.ms += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

}  // namespace

extern "C" bool hw_game_interp_on() { return interp::game_interp(); }

// Stage d diagnostic only: count IPC issued synchronously by this thread's extra draw. This does not
// suppress side effects, inspect guest-memory changes, or cover work scheduled on other threads.
namespace {
std::mutex g_ipc_m;
std::map<std::string, u64> g_ipc_extra;
void report_ipc() {
    std::lock_guard<std::mutex> l(g_ipc_m);
    if (g_ipc_extra.empty()) return;
    std::string sum;
    for (auto& [k, n] : g_ipc_extra) sum += " " + k + " x" + std::to_string(n);
    hw_log("gi: service calls during extra frames:%s", sum.c_str());
    g_ipc_extra.clear();
}
}  // namespace
extern "C" void hw_gi_note_ipc(const char* service, u32 command) {
    if (!g_in_extra) return;
    std::lock_guard<std::mutex> l(g_ipc_m);
    g_ipc_extra[std::string(service) + "#" + std::to_string(command)]++;
}

// Snapshot on QueueBuffer's guest calling thread, before asynchronous compositor work. A real frame
// uses 120 only when its tick actually presented an extra; rejected/skipped extras and menus stay at
// their native rate immediately. With GI off, preserve the normal presenter's environment semantics.
extern "C" u32 hw_game_interp_release_hz() {
    // Reaching a valid QueueBuffer is the evidence that this extra actually presents. Present's
    // successful return alone is not enough: the guest can finish it without queuing a buffer.
    if (g_in_extra) g_paired_real = true;
    if (g_in_extra || g_paired_real) return 120;
    const bool enabled = g_frame_started ? g_frame_gi : interp::game_interp();
    return enabled ? 60 : 0;
}

// Hook (tools/recomp/hooks.txt): BeginFrame. Only the battle tick's call (from f_10002acb54) gets
// the extra frame; menus and loading screens draw through other paths and stay at one frame per tick.
extern "C" void f_100051e9c4(Ctx* c) {
    if (!g_in_extra) {
        g_paired_real = false;
        g_frame_gi = interp::game_interp();
        g_frame_started = true;
        // Already queued submissions retain their own tag. Later native/menu work must not inherit
        // the last enabled tick's object-interpolation history after a live UI toggle.
        hw_gi_submit_tag = 0;
    }
    if (g_frame_gi && !g_in_extra && c->x[30] == kBeginFrameRet) {
        // Game speed first: the game is frame-locked, so when ticks fall behind 60 per second the extra
        // frames are what slow it down. Track how far behind the 16.67 ms grid the ticks are (with triple
        // buffering single ticks jitter without the game being late) and skip an extra frame while behind.
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        static LARGE_INTEGER prev;
        static double debt = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = prev.QuadPart ? (now.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart : 0.0;
        prev = now;
        static bool skipped_last = false;
        if (dt > 100.0) debt = 0;  // a pause or load, not lateness
        else if (dt > 0) debt = std::min(50.0, std::max(0.0, debt * 0.95 + dt - 1000.0 / 60.0));
        // Never skip two ticks in a row. Skipping avoids the second draw's cost; its unpaired real
        // frame is released on the native 60 Hz grid rather than advancing game logic at 120 Hz.
        const bool behind = debt > 8.0 && !skipped_last;
        if (behind) debt = std::max(0.0, debt - 1000.0 / 120.0);
        skipped_last = behind;
        g_begin = now_ms();
        g_xdraw = g_xpresent = g_begin;
        {
            interp::CamSnap n[1];
            if (interp::snapshots(n, 1) == 1) hw_gi_submit_tag = n[0].tick << 1;
        }
        if (behind) {
            g_st.late++;
        } else {
            extra_frame(c);
        }
        if (g_tick_in > 0) {
            g_st.ticks++;
            g_st.draws += hw_last_frame_draws.load();
            add(g_st.t_update, g_st.m_update, g_begin - g_tick_in);
            add(g_st.t_xdraw, g_st.m_xdraw, g_xdraw - g_begin);
            add(g_st.t_xpresent, g_st.m_xpresent, g_xpresent - g_xdraw);
        }
        report();
    }
    f_100051e9c4_orig(c);
}

// Hook (tools/recomp/hooks.txt): the battle tick on MainThread, for the per-tick timing above.
extern "C" void f_10002acb54(Ctx* c) {
    if (!interp::game_interp()) {
        f_10002acb54_orig(c);
        return;
    }
    double in = now_ms();
    if (g_tick_out > 0 && in - g_tick_out < 100.0) add(g_st.t_loop, g_st.m_loop, in - g_tick_out);
    g_tick_in = in;
    f_10002acb54_orig(c);
    g_tick_out = now_ms();
    if (g_xpresent > 0 && g_tick_out - g_xpresent < 100.0) add(g_st.t_real, g_st.m_real, g_tick_out - g_xpresent);
}
