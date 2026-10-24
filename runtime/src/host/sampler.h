// CPU-time sampling of one host thread (see sampler.cpp). Diagnostics only.
#pragma once
#include <string>

namespace host {
// Starts sampling the calling thread every `period_us` of its CPU time; `path` receives the counts every 10 s.
void sample_current_thread(int period_us, const std::string& path);
}  // namespace host
