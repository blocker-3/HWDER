// CPU-time sampling of host threads (see sampler.cpp). Diagnostics only.
#pragma once
#include <string>

namespace host {
// Starts sampling the calling thread every `period_us` of its CPU time; `path` receives the counts every 10 s.
// Several threads may each call it (one file per thread).
void sample_current_thread(int period_us, const std::string& path);

// Phase tag recorded with every sample (0 = untagged); the 120 fps mode tags MainThread's battle tick with it.
// Names are given once per tag value; the report splits its per-function counts by phase.
extern volatile unsigned char g_sample_phase;
void name_sample_phase(unsigned char phase, const char* name);

// HWDER_TIMELINE=<seconds>: from that many seconds after start, record timeline events for 0.5 s and write
// them to <log>.timeline.txt (time in us, thread, event, two values). Diagnostics only; a no-op otherwise.
bool timeline_on();
void timeline_event(const char* what, unsigned long long a = 0, unsigned long long b = 0);
inline void timeline(const char* what, unsigned long long a = 0, unsigned long long b = 0) {
    if (timeline_on()) timeline_event(what, a, b);
}
// Sets g_sample_phase and records the change on the timeline.
inline void set_sample_phase(unsigned char phase) {
    g_sample_phase = phase;
    timeline("phase", phase);
}
}  // namespace host
