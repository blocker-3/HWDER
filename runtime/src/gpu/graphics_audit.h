// Cumulative diagnostics for graphics-audit-plan step 1. Renderer mutex protects counters, except the fence counter (GPU thread).
#pragma once
#include <cstdint>
#include <atomic>
namespace gpu::audit {
inline std::atomic<uint64_t> fence_timeouts{0};
inline uint64_t skipped_draws = 0;
inline uint64_t cpu_image_writes = 0;
inline uint64_t unscaled_viewports = 0;
inline uint64_t tex_dynamic_offsets = 0; // translated instructions, not draws
}
