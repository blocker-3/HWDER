// GPU diagnostics in one place: the gfx-audit counters (gpu::audit), the draw-path profile
// (HWDER_PROFILE_MS: ProfScope / g_prof / prof_flush) and the slow-scope stall log (SlowTimer). Folded from
// gpu/graphics_audit.h and gpu/vk/vk_common.h with the same names; vk_common.h re-exports the profile names
// into gpu::vk so renderer call sites are unchanged. What is counted, logged and when is as before.
#pragma once
#include <cstdint>
#include <atomic>
#include "hwder/runtime.h"
#include "host/timing.h"

// Cumulative diagnostics for graphics-audit-plan step 1. Renderer mutex protects counters, except the fence counter (GPU thread).
namespace gpu::audit {
inline std::atomic<uint64_t> fence_timeouts{0};
inline uint64_t skipped_draws = 0;
inline uint64_t cpu_image_writes = 0;
inline uint64_t unscaled_viewports = 0;
inline uint64_t tex_dynamic_offsets = 0; // translated instructions, not draws
}

namespace gpu::diag {

// Draw-path profile (GPU thread): accumulated ms per phase since the last flush; logged when large.
enum ProfPhase {
    kProfProgram, kProfFramebuffer, kProfVertex, kProfIndex, kProfDescriptors, kProfPipeline, kProfRecord,
    kProfCbuf, kProfTexGet, kProfTexView, kProfSampler, kProfDescAlloc, kProfDescUpdate, kProfFbGet, kProfFbView,
    kProfReadable, kProfCreateImage, kProfAllocBind, kProfUpload, kProfStage, kProfDecode,
    kProfGetLayout, kProfGetOverlaps, kProfGetMatch, kProfGetValidate, kProfGetMiss, kProfClear, kProfInvalidate, kProfPresent,
    kProfKeyBuild, kProfTexHit, kProfValidate, kProfWatch, kProfHash, kProfTextures, kProfSubmit, kProfFrameBegin, kProfObj, kProfCbufCopy, kProfObjReal, kProfObjExtra, kProfMacro, kProfPushbuf, kProfDrawTotal, kProfCount
};
static const char* const kProfNames[kProfCount] = {
    "program", "framebuffer", "vertex", "index", "descriptors", "pipeline", "record",
    "|cbuf", "tex-get", "tex-view", "sampler", "desc-alloc", "desc-update", "fb-get", "fb-view",
    "|readable", "create-image", "alloc-bind", "upload", "stage", "decode",
    "|get-layout", "get-overlaps", "get-match", "get-validate", "get-miss", "|clear", "invalidate", "present", "keybuild", "tex-hit", "validate", "watch", "hash", "textures", "submit", "frame-begin", "obj", "cbuf-copy", "obj-real", "obj-extra", "macro(incl)", "pushbuf(incl)", "|draw-total"};
extern double g_prof[kProfCount];
extern uint32_t g_prof_draws;
extern uint64_t g_prof_cbuf_bytes, g_prof_cbufs, g_prof_sets, g_prof_sets_reused, g_prof_vtx_bytes, g_prof_vtx_copies, g_prof_idx_bytes, g_prof_idx_scanned;  // constant-buffer bytes/bindings staged, descriptor sets allocated
// The profile costs two clock reads per scope, ~25 scopes per draw: only with HWDER_PROFILE_MS or HWDER_THREAD_STATS.
bool prof_enabled();
inline double prof_tick_ms() {
    static const double ms = [] {
        host::Counter f;
        host::counter_frequency(&f);
        return 1000.0 / f.QuadPart;
    }();
    return ms;
}
struct ProfScope {
    ProfPhase ph;
    host::Counter a;
    explicit ProfScope(ProfPhase p) : ph(p) {
        static const bool on = prof_enabled();
        a.QuadPart = 0;
        if (on) host::counter(&a);
    }
    ~ProfScope() {
        if (!a.QuadPart) return;
        host::Counter b;
        host::counter(&b);
        g_prof[ph] += (b.QuadPart - a.QuadPart) * prof_tick_ms();
    }
};
// Logs the accumulated profile when it exceeds HWDER_PROFILE_MS (default 200 ms), then resets it; returns the total.
double prof_flush();
// Logs the profile accumulated over the last `frames` presents (per frame) with the GPU thread busy time, then resets it.
void prof_window_log(unsigned frames, double busy_ms);

// Logs scopes that exceed `limit_ms` (stall diagnostics).
struct SlowTimer {
    const char* what;
    double limit_ms;
    host::Counter a;
    SlowTimer(const char* w, double limit) : what(w), limit_ms(limit) { host::counter(&a); }
    ~SlowTimer() {
        host::Counter b, f;
        host::counter(&b);
        host::counter_frequency(&f);
        double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        if (ms > limit_ms) hw_log("vk: slow: %s took %.0f ms", what, ms);
    }
};

}  // namespace gpu::diag
