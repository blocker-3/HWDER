// Host timing boundary. Windows retains QPC and high-resolution waitable timers;
// Linux uses a nanosecond CLOCK_MONOTONIC timebase and EINTR-safe nanosleep.
#pragma once
#include <cstdint>
namespace host {
struct Counter { std::int64_t QuadPart = 0; };
void counter(Counter* value);
void counter_frequency(Counter* value);
std::uint64_t ticks_ms();
void sleep_ns(std::int64_t nanoseconds);
void sleep_ms(std::uint32_t milliseconds);
void sleep_counter_ticks(std::int64_t ticks, std::int64_t frequency);
void name_thread(const wchar_t* name);
}  // namespace host
