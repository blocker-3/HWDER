// High frame rate, stage 2.1 (docs/high-framerate.md): map the game's frame. Pass-through hooks on
// the scene object's per-tick methods log which thread calls them, in which order and how often.
//
// HWDER_FRAME_PROBE=<n>: log the first n calls once the battle is up (a frame with more than 450
// draws), then nothing. No behaviour change.
#include <windows.h>

#include <atomic>
#include <cstdlib>

#include "hwder/runtime.h"

extern "C" std::atomic<u32> hw_last_frame_draws;  // gpu/vk: draws in the last presented frame

namespace {

int budget() {
    static const int n = getenv("HWDER_FRAME_PROBE") ? atoi(getenv("HWDER_FRAME_PROBE")) : 0;
    return n;
}

void probe(const char* name, const Ctx* c, bool enter) {
    static std::atomic<int> left{-1};
    if (budget() <= 0) return;
    static std::atomic<bool> armed{false};
    if (!armed.load()) {
        if (hw_last_frame_draws.load() <= 450) return;
        armed = true;
        left = budget();
        hw_log("probe: battle up, logging %d calls", budget());
    }
    if (left.fetch_sub(1) <= 0) return;
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    hw_log("probe: %s %s thread %lu t=%.3f x0=%llx", enter ? ">" : "<", name, GetCurrentThreadId(),
           t.QuadPart * 1000.0 / f.QuadPart, (unsigned long long)c->x[0]);
}

}  // namespace

#define PROBE(addr, name)                                  \
    extern "C" void f_##addr##_orig(Ctx* c);               \
    extern "C" void f_##addr(Ctx* c) {                     \
        u64 self = c->x[0];                                \
        probe(name, c, true);                              \
        f_##addr##_orig(c);                                \
        Ctx tmp = *c;                                      \
        tmp.x[0] = self;                                   \
        probe(name, &tmp, false);                          \
    }

// scene class A (vtable 0x1000ba6008): slot 3 publishes the render cameras, slots 5 and 18 build draws
PROBE(10001766a4, "A.slot3 update")
PROBE(10001767e8, "A.slot5 draw?")
PROBE(100017672c, "A.slot18 draw?")
PROBE(1000176490, "A.slot2")
PROBE(10001763bc, "A.slot4")
// scene class B (vtable 0x1000bd36f8)
PROBE(10002d96b4, "B.slot3 update")
PROBE(10002d9878, "B.slot5 draw?")
// camera constants upload (render side)
PROBE(10003d0a74, "cam-constants")
