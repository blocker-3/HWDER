// Per-thread CPU and wait statistics (HWDER_THREAD_STATS=<ms>): which host threads (guest threads are host
// threads) burn CPU, how long each one blocks and on what, and how often it runs on an efficiency core.
// Experiment branch perf-threads; off unless the variable is set.
#pragma once
#include <cstdint>

namespace perf {

enum WaitKind { kWaitKernel, kWaitSyncpt, kWaitDequeue, kWaitGpuQueue, kWaitCount };

bool enabled();
// Starts the reporter thread when HWDER_THREAD_STATS is set (idempotent).
void start();
// Adds the time the calling thread spent blocked (QPC ticks) to its counters.
void add_wait(WaitKind k, int64_t qpc_ticks);
// Called on svc entry: samples whether the calling thread runs on an efficiency core.
void sample_core();
// Counts a QueueBuffer (one presented guest frame) and remembers the calling thread as the presenter.
void note_present();
// A guest SleepThread: requested and actual duration.
void note_sleep(int64_t req_ns, int64_t actual_ns);

// RAII wait timer; does nothing when disabled.
struct WaitScope {
    WaitKind k;
    int64_t t0 = 0;
    explicit WaitScope(WaitKind kind);
    ~WaitScope();
};

}  // namespace perf
