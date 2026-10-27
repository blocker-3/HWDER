#include "host/timing.h"
#include "host/platform.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#include <pthread.h>
#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif
namespace host {
#if !defined(_WIN32) && defined(__x86_64__)
namespace {
// The counter reads the invariant TSC like QueryPerformanceCounter does: the GPU thread reads it thousands of times
// per frame (draw and stall timers), and clock_gettime cost ~3% of that thread. Calibrated once against
// CLOCK_MONOTONIC; CPUs without an invariant TSC keep the nanosecond clock.
struct Tsc {
    bool ok = false;
    std::int64_t frequency = 1000000000;
};
const Tsc& tsc() {
    static const Tsc t = [] {
        Tsc r;
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!__get_cpuid(0x80000007, &a, &b, &c, &d) || !(d & (1u << 8))) return r;
        const std::uint64_t t0 = monotonic_ns(), c0 = __rdtsc();
        std::uint64_t t1;
        while ((t1 = monotonic_ns()) - t0 < 5000000) {}
        const std::uint64_t c1 = __rdtsc();
        r.frequency = static_cast<std::int64_t>(static_cast<long double>(c1 - c0) * 1e9L / (t1 - t0));
        r.ok = r.frequency > 1000000;
        return r;
    }();
    return t;
}
}  // namespace
#endif
void counter(Counter* value) {
#ifdef _WIN32
    LARGE_INTEGER native; QueryPerformanceCounter(&native); value->QuadPart = native.QuadPart;
#elif defined(__x86_64__)
    value->QuadPart = tsc().ok ? static_cast<std::int64_t>(__rdtsc()) : static_cast<std::int64_t>(monotonic_ns());
#else
    value->QuadPart = static_cast<std::int64_t>(monotonic_ns());
#endif
}
void counter_frequency(Counter* value) {
#ifdef _WIN32
    LARGE_INTEGER native; QueryPerformanceFrequency(&native); value->QuadPart = native.QuadPart;
#elif defined(__x86_64__)
    value->QuadPart = tsc().frequency;
#else
    value->QuadPart = 1000000000;
#endif
}
std::uint64_t ticks_ms() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return monotonic_ns() / 1000000;
#endif
}
void sleep_ns(std::int64_t ns) {
    if (ns <= 0) return;
#ifdef _WIN32
    struct Timer {
        HANDLE value = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Timer() { if (value) CloseHandle(value); }
    };
    static thread_local Timer timer;
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100 + (ns % 100 != 0));
    if (timer.value && SetWaitableTimerEx(timer.value, &due, 0, nullptr, nullptr, nullptr, 0))
        WaitForSingleObject(timer.value, INFINITE);
    else Sleep(static_cast<DWORD>(ns / 1000000 + (ns % 1000000 != 0)));
#else
    timespec request{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
    while (nanosleep(&request, &request) != 0 && errno == EINTR) {}
#endif
}
void sleep_ms(std::uint32_t ms) { sleep_ns(static_cast<std::int64_t>(ms) * 1000000); }
void sleep_counter_ticks(std::int64_t ticks, std::int64_t frequency) {
    if (ticks > 0 && frequency > 0)
        sleep_ns(static_cast<std::int64_t>(static_cast<long double>(ticks) * 1000000000.0L / frequency));
}
void name_thread(const wchar_t* name) {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), name);
#else
    // Existing diagnostic labels are ASCII; Linux thread names allow 15 bytes plus NUL.
    char label[16]{};
    for (unsigned i = 0; i < sizeof(label) - 1 && name[i]; ++i)
        label[i] = name[i] < 128 ? static_cast<char>(name[i]) : '?';
    pthread_setname_np(pthread_self(), label);
#endif
}
}  // namespace host
