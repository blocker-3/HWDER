// Storage and flush of the draw-path profile (see diag.h). Moved from vk/texture_cache.cpp and vk/vk_debug.cpp.
#include <cstring>
#include <cstdio>

#include "gpu/diag.h"
#include "debug_flags.h"

namespace gpu::diag {

double g_prof[kProfCount];
uint32_t g_prof_draws;

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
    memset(g_prof, 0, sizeof(g_prof));
    g_prof_draws = 0;
    return total;
}

}  // namespace gpu::diag
