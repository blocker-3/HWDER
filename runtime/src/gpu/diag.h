// GPU diagnostics in one place: the gfx-audit counters (gpu::audit), the draw-path profile
// (HWDER_PROFILE_MS: ProfScope / g_prof / prof_flush) and the slow-scope stall log (SlowTimer). Folded from
// gpu/graphics_audit.h and gpu/vk/vk_common.h with the same names; vk_common.h re-exports the profile names
// into gpu::vk so renderer call sites are unchanged. What is counted, logged and when is as before.
#pragma once
#include <cstdint>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif
#include "hwder/runtime.h"

// Cumulative diagnostics for graphics-audit-plan step 1. Renderer mutex protects counters, except the fence counter (GPU thread).
namespace gpu::audit {
inline std::atomic<uint64_t> fence_timeouts{0};
inline uint64_t skipped_draws = 0;
inline uint64_t cpu_image_writes = 0;
inline uint64_t unscaled_viewports = 0;
inline uint64_t tex_dynamic_offsets = 0; // translated instructions, not draws
}

#ifdef _WIN32
namespace gpu::diag {

// Draw-path profile (GPU thread): accumulated ms per phase since the last flush; logged when large.
enum ProfPhase {
    kProfProgram, kProfFramebuffer, kProfVertex, kProfIndex, kProfDescriptors, kProfPipeline, kProfRecord,
    kProfCbuf, kProfTexGet, kProfTexView, kProfSampler, kProfDescAlloc, kProfDescUpdate, kProfFbGet, kProfFbView,
    kProfReadable, kProfCreateImage, kProfAllocBind, kProfUpload, kProfStage, kProfDecode,
    kProfGetLayout, kProfGetOverlaps, kProfGetMatch, kProfGetValidate, kProfGetMiss, kProfClear, kProfInvalidate, kProfPresent,
    kProfKeyBuild, kProfDrawTotal, kProfCount
};
static const char* const kProfNames[kProfCount] = {
    "program", "framebuffer", "vertex", "index", "descriptors", "pipeline", "record",
    "|cbuf", "tex-get", "tex-view", "sampler", "desc-alloc", "desc-update", "fb-get", "fb-view",
    "|readable", "create-image", "alloc-bind", "upload", "stage", "decode",
    "|get-layout", "get-overlaps", "get-match", "get-validate", "get-miss", "|clear", "invalidate", "present", "keybuild", "|draw-total"};
extern double g_prof[kProfCount];
extern uint32_t g_prof_draws;
struct ProfScope {
    ProfPhase ph;
    LARGE_INTEGER a;
    explicit ProfScope(ProfPhase p) : ph(p) { QueryPerformanceCounter(&a); }
    ~ProfScope() {
        LARGE_INTEGER b, f;
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        g_prof[ph] += (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    }
};
// Logs the accumulated profile when it exceeds HWDER_PROFILE_MS (default 200 ms), then resets it; returns the total.
double prof_flush();

// Logs scopes that exceed `limit_ms` (stall diagnostics).
struct SlowTimer {
    const char* what;
    double limit_ms;
    LARGE_INTEGER a;
    SlowTimer(const char* w, double limit) : what(w), limit_ms(limit) { QueryPerformanceCounter(&a); }
    ~SlowTimer() {
        LARGE_INTEGER b, f;
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        if (ms > limit_ms) hw_log("vk: slow: %s took %.0f ms", what, ms);
    }
};

}  // namespace gpu::diag
#endif
