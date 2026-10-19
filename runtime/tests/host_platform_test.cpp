// Public contract tests; no generated sources, game, Vulkan, SDL, keys or saves.
#include "host/platform.h"
#include "host/timing.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (host error %lu)\n", message, host::memory_error());
        std::exit(1);
    }
}
namespace {
struct ThreadProbe { void* event; std::atomic<int> entered{0}; std::uint32_t tid = 0; };
void callback(void* opaque) {
    auto& probe = *static_cast<ThreadProbe*>(opaque);
    probe.tid = host::thread_id();
    probe.entered++;
    host::event_signal(probe.event);
}
}
int main() {
    host::Counter frequency, before, after;
    host::counter_frequency(&frequency); host::counter(&before);
    check(frequency.QuadPart > 0, "positive monotonic frequency");
    host::sleep_counter_ticks(frequency.QuadPart / 1000, frequency.QuadPart);
    host::counter(&after);
    check(after.QuadPart > before.QuadPart, "counter advances across sleep");
    void* event = host::event_create();
    check(event != nullptr, "event creation");
    check(!host::event_wait(event, 0), "unsignaled event timeout");
    host::event_signal(event); host::event_signal(event);
    check(host::event_wait(event, 0), "wake before wait retained");
    check(!host::event_wait(event, 0), "auto reset/coalesced wake");
    ThreadProbe probe{event};
    void* thread = host::thread_create(callback, &probe, 8 << 20);
    check(thread != nullptr, "dormant thread creation");
    check(!host::event_wait(event, 1000000) && !probe.entered, "callback waits for start");
    const auto start = host::monotonic_ns();
    check(host::thread_start(thread), "thread start");
    check(host::event_wait(event, 2000000000), "wait wakes across threads");
    host::thread_join_release(thread);
    check(probe.entered == 1 && probe.tid && probe.tid != host::thread_id(), "callback/thread identity");
    check(host::monotonic_ns() >= start, "monotonic clock");
    host::event_release(event);
    void* context = host::aligned_allocate(888, 64);
    check(context && reinterpret_cast<std::uintptr_t>(context) % 64 == 0, "CPU context alignment");
    std::memset(context, 0, 888);
    host::aligned_release(context);
    constexpr std::size_t page = 4096, size = 65536;
    auto* memory = static_cast<unsigned char*>(host::memory_reserve(nullptr, size, true));
    check(memory != nullptr, "reserve inaccessible guest range");
    check(host::memory_readable(memory, size) == 0, "reserved memory is unreadable");
    check(host::memory_commit(memory, page * 2), "commit subset");
    check(host::memory_readable(memory, size) == page * 2, "readable extent stops at uncommitted boundary");
    for (std::size_t i = 0; i < page * 2; ++i) check(memory[i] == 0, "initial zero memory");
    std::memset(memory, 0x5a, page * 2);
    check(host::memory_reserve(memory, size) == nullptr, "collision rejects replacement");
    check(memory[0] == 0x5a && memory[page] == 0x5a, "collision preserves existing bytes");
    check(host::memory_decommit(memory, page), "decommit subset");
    check(host::memory_readable(memory, page) == 0, "discarded page is unreadable");
    check(host::memory_readable(memory + page, page) == page, "neighbor remains readable");
    check(host::memory_commit(memory, page), "recommit subset");
    for (std::size_t i = 0; i < page; ++i) check(memory[i] == 0, "recommit zeroes discarded guest data");
    check(memory[page] == 0x5a, "neighbor survives decommit");
    check(host::memory_release(memory, size), "release reservation");
    // Exercise guest identity addresses without replacing a potentially occupied mapping.
    auto* fixed = reinterpret_cast<void*>(0x4000000000ull);
    if (host::memory_reserve(fixed, size)) {
        check(host::memory_commit(fixed, page), "fixed guest commit");
        check(host::memory_release(fixed, size), "fixed guest release");
    } else std::puts("fixed identity address occupied: collision safely rejected");
    std::puts("host platform contract checks passed");
}
