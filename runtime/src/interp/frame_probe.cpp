// High frame rate, stage a (docs/frame-loop.md): map the game's frame. Pass-through hooks on the
// scene object's per-tick methods log which thread calls them, in which order and how often.
//
// HWDER_FRAME_PROBE=<n>: once the battle has been up (a frame with more than 450 draws) for
// HWDER_FRAME_PROBE_DELAY seconds (default 40), log the next n hook entries/exits in detail, with
// the render camera slots (S + 0x710dc write, S + 0x710e0 read). From then on, every 5 s, log per
// hook its calls per second, mean time and thread, and how many Draws (scene A slot 5) ran between
// consecutive camera publishes (scene A slot 3). No behaviour change.
// HWDER_DRAW_TWICE=1: Draw runs twice per tick (experiment: is a repeated Draw harmless?).
// HWDER_DRAW_WRITES=<n> (draw_writes.cpp) records which guest pages slot 5 writes, on n ticks.
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <set>

#include "hwder/runtime.h"
#include "kernel/kernel.h"

extern "C" std::atomic<u32> hw_last_frame_draws;  // gpu/vk: draws in the last presented frame
extern "C" bool hw_draw_writes_begin(u64 self);
extern "C" void hw_draw_writes_end(u64 self, double ms);

namespace {

enum { kSlot3, kSlot5, kSlot18, kSlot2, kSlot4, kB3, kB5, kCam, kCount };
const char* kNames[kCount] = {"A.slot3", "A.slot5", "A.slot18", "A.slot2", "A.slot4", "B.slot3", "B.slot5", "cam-constants"};

int env_int(const char* k, int d) { return getenv(k) ? atoi(getenv(k)) : d; }
int budget() { static const int n = env_int("HWDER_FRAME_PROBE", 0); return n; }

double now_ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

// Render camera slot indices of the scene object S (docs/camera.md).
void slots(u32* w, u32* r) {
    u64 s = *(u64*)0x1000c2b5e8 ? *(u64*)*(u64*)0x1000c2b5e8 : 0;
    *w = s ? *(u32*)(s + 0x710dc) : ~0u;
    *r = s ? *(u32*)(s + 0x710e0) : ~0u;
}

std::atomic<int> g_state{0};  // 0 waiting for battle, 1 delay, 2 armed
double g_t_arm = 0, g_t_battle = 0;
std::atomic<int> g_left{0};

struct Stat { std::atomic<u64> calls{0}, us{0}; std::atomic<DWORD> tid{0}; };
Stat g_stat[kCount];
std::mutex g_mu;
int g_draws_since_publish = 0;
u64 g_hist[4];  // Draws between consecutive publishes: 0, 1, 2, 3+
double g_t_window = 0;
std::set<DWORD> g_named;

void name_thread(DWORD tid) {
    if (!g_named.insert(tid).second) return;
    auto* t = kern::current_thread();
    if (!t) { hw_log("probe: thread %lu is a host thread", tid); return; }
    hw_log("probe: thread %lu = guest thread %llu '%s' entry %llx prio %d core %d", tid, (unsigned long long)t->id,
           t->name.c_str(), (unsigned long long)t->entry, t->priority, t->ideal_core);
}

bool armed() {
    if (budget() <= 0) return false;
    int s = g_state.load();
    if (s == 2) return true;
    if (s == 0) {
        if (hw_last_frame_draws.load() <= 450) return false;
        g_t_battle = now_ms();
        g_state = 1;
        hw_log("probe: battle up, waiting %d s", env_int("HWDER_FRAME_PROBE_DELAY", 40));
        return false;
    }
    if (now_ms() - g_t_battle < env_int("HWDER_FRAME_PROBE_DELAY", 40) * 1000.0) return false;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_state.load() != 2) {
        g_left = budget();
        g_t_arm = g_t_window = now_ms();
        g_state = 2;
        hw_log("probe: armed, logging %d entries/exits", budget());
    }
    return true;
}

void report_window(double t) {
    double sec = (t - g_t_window) / 1000.0;
    for (int i = 0; i < kCount; i++) {
        u64 n = g_stat[i].calls.exchange(0), us = g_stat[i].us.exchange(0);
        if (n) hw_log("probe: stats %-14s %6.1f calls/s, mean %.3f ms, thread %lu", kNames[i], n / sec, us / 1000.0 / n,
                      g_stat[i].tid.load());
    }
    hw_log("probe: stats Draws between camera publishes: 0 x%llu, 1 x%llu, 2 x%llu, 3+ x%llu", g_hist[0], g_hist[1],
           g_hist[2], g_hist[3]);
    for (auto& h : g_hist) h = 0;
    g_t_window = t;
}

void event(int fn, u64 self, bool enter, double t, double dur) {
    DWORD tid = GetCurrentThreadId();
    std::lock_guard<std::mutex> l(g_mu);
    name_thread(tid);
    if (!enter) {
        g_stat[fn].calls++;
        g_stat[fn].us += (u64)(dur * 1000.0);
        g_stat[fn].tid = tid;
        if (fn == kSlot5) g_draws_since_publish++;
        if (fn == kSlot3) {
            g_hist[g_draws_since_publish > 3 ? 3 : g_draws_since_publish]++;
            g_draws_since_publish = 0;
        }
        if (t - g_t_window >= 5000.0) report_window(t);
    }
    if (g_left.fetch_sub(1) > 0) {
        u32 w, r;
        slots(&w, &r);
        hw_log("probe: %s %-14s thread %5lu t=%9.3f self=%llx slot w=%u r=%u%s", enter ? ">" : "<", kNames[fn], tid,
               t - g_t_arm, (unsigned long long)self, w, r, enter ? "" : (" (" + std::to_string(dur) + " ms)").c_str());
    }
}

}  // namespace

// Marks in the detailed trace from host-side services (vi DequeueBuffer/QueueBuffer, nvdrv SubmitGPFIFO).
extern "C" void hw_frame_probe_mark(const char* what) {
    if (g_state.load() != 2 || g_left.load() <= 0) return;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_left.fetch_sub(1) > 0)
        hw_log("probe: * %-14s thread %5lu t=%9.3f", what, GetCurrentThreadId(), now_ms() - g_t_arm);
}

namespace {

// Once per hook: the guest return address and the frame-record chain (x29 -> [fp, lr]) at entry.
void log_chain(int fn, const Ctx* c) {
    static std::atomic<bool> done[kCount];
    if (done[fn].exchange(true)) return;
    char buf[512];
    int n = snprintf(buf, sizeof buf, "probe: callers of %s: lr %llx", kNames[fn], (unsigned long long)c->x[30]);
    u64 fp = c->x[29];
    for (int i = 0; i < 10 && fp && (fp & 7) == 0 && ((fp >= 0x4000000000ull && fp < 0x4080000000ull) || (fp >= 0x2000000000ull && fp < 0x2180000000ull)); i++) {
        n += snprintf(buf + n, sizeof buf - n, " <- %llx", (unsigned long long)*(u64*)(fp + 8));
        fp = *(u64*)fp;
    }
    hw_log("%s", buf);
}

template <int Fn>
void around(Ctx* c, void (*orig)(Ctx*)) {
    u64 self = c->x[0];
    if (!armed()) { orig(c); return; }
    double t0 = now_ms();
    log_chain(Fn, c);
    event(Fn, self, true, t0, 0);
    bool census = Fn == kSlot5 && hw_draw_writes_begin(self);
    // HWDER_DRAW_TWICE=1 (experiment): run Draw a second time for the same tick, same camera.
    static const bool twice = env_int("HWDER_DRAW_TWICE", 0) != 0;
    Ctx entry;
    if (Fn == kSlot5 && twice) entry = *c;
    orig(c);
    if (Fn == kSlot5 && twice) {
        hw_frame_probe_mark("draw again");
        *c = entry;
        orig(c);
    }
    double t1 = now_ms();
    if (census) hw_draw_writes_end(self, t1 - t0);
    event(Fn, self, false, t1, t1 - t0);
}

}  // namespace

#define PROBE(addr, fn)                                         \
    extern "C" void f_##addr##_orig(Ctx* c);                    \
    extern "C" void f_##addr(Ctx* c) { around<fn>(c, f_##addr##_orig); }

// scene class A (vtable 0x1000ba6008): slot 3 publishes the render cameras, slot 5 draws
PROBE(10001766a4, kSlot3)
PROBE(10001767e8, kSlot5)
PROBE(100017672c, kSlot18)
PROBE(1000176490, kSlot2)
PROBE(10001763bc, kSlot4)
// scene class B (vtable 0x1000bd36f8)
PROBE(10002d96b4, kB3)
PROBE(10002d9878, kB5)
// camera constants upload (render side)
PROBE(10003d0a74, kCam)
