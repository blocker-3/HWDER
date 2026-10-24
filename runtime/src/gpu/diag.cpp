// Storage and flush of the draw-path profile (see diag.h). Moved from vk/texture_cache.cpp and vk/vk_debug.cpp.
#include <cstring>
#include <cstdio>

#include "gpu/diag.h"
#include "debug_flags.h"

namespace gpu::diag {

double g_prof[kProfCount];
uint32_t g_prof_draws;
uint64_t g_prof_cbuf_bytes, g_prof_cbufs, g_prof_sets;
static double g_prof_win[kProfCount];
static uint64_t g_prof_win_draws;

void prof_window_log(unsigned frames, double busy_ms) {
    if (!frames) return;
    char line[768];
    int o = snprintf(line, sizeof(line), "vk: thread profile per frame: busy %.2f ms, %.0f draws, ms:", busy_ms / frames,
                     (double)g_prof_win_draws / frames);
    o += snprintf(line + o, sizeof(line) - o, " | %.0f cbufs %.0f KiB, %.0f sets |", (double)g_prof_cbufs / frames,
                  g_prof_cbuf_bytes / 1024.0 / frames, (double)g_prof_sets / frames);
    g_prof_cbufs = g_prof_cbuf_bytes = g_prof_sets = 0;
    for (int i = 0; i < kProfCount; i++)
        if (g_prof_win[i] / frames >= 0.005) o += snprintf(line + o, sizeof(line) - o, " %s %.2f", kProfNames[i], g_prof_win[i] / frames);
    hw_log("%s", line);
    memset(g_prof_win, 0, sizeof(g_prof_win));
    g_prof_win_draws = 0;
}

double prof_flush() {
    double total = 0;
    for (double v : g_prof) total += v;
    static const double limit = debug::value(debug::ProfileMs, 200.0);
    if (total > limit) {
        char line[512];
        int o = snprintf(line, sizeof(line), "vk: profile: %u draws, %.0f ms:", g_prof_draws, total);
        for (int i = 0; i < kProfCount; i++) o += snprintf(line + o, sizeof(line) - o, " %s %.0f", kProfNames[i], g_prof[i]);
        hw_log("%s", line);
    }
    for (int i = 0; i < kProfCount; i++) g_prof_win[i] += g_prof[i];
    g_prof_win_draws += g_prof_draws;
    memset(g_prof, 0, sizeof(g_prof));
    g_prof_draws = 0;
    return total;
}

}  // namespace gpu::diag
