#include "host/platform.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace host {
std::uint64_t monotonic_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
#ifdef _WIN32
void* event_create() { return CreateEventW(nullptr, FALSE, FALSE, nullptr); }
void event_release(void* event) { CloseHandle(event); }
void event_signal(void* event) { SetEvent(event); }
bool event_wait(void* event, std::int64_t timeout_ns) {
    DWORD ms = INFINITE;
    if (timeout_ns >= 0) {
        const auto ns = static_cast<std::uint64_t>(timeout_ns);
        const auto rounded = ns / 1000000 + (ns % 1000000 != 0);
        ms = static_cast<DWORD>(rounded > 0x7fffffff ? 0x7fffffff : rounded);
    }
    return WaitForSingleObject(event, ms) == WAIT_OBJECT_0;
}
namespace {
struct Start { ThreadEntry entry; void* argument; };
DWORD WINAPI thread_entry(void* opaque) {
    auto start = *static_cast<Start*>(opaque);
    delete static_cast<Start*>(opaque);
    start.entry(start.argument);
    return 0;
}
}
void* thread_create(ThreadEntry entry, void* argument, std::size_t stack_size) {
    auto* start = new (std::nothrow) Start{entry, argument};
    if (!start) return nullptr;
    auto handle = CreateThread(nullptr, stack_size, thread_entry, start, CREATE_SUSPENDED, nullptr);
    if (!handle) delete start;
    return handle;
}
bool thread_start(void* thread) { return ResumeThread(thread) != static_cast<DWORD>(-1); }
void thread_join_release(void* thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
[[noreturn]] void thread_exit() { ExitThread(0); }
std::uint32_t thread_id() { return GetCurrentThreadId(); }
void thread_yield() { SwitchToThread(); }
#else
namespace {
struct Event {
    std::mutex mutex;
    std::condition_variable wake;
    bool signaled = false;
};
struct Thread {
    pthread_t native{};
    Event start;
    ThreadEntry entry;
    void* argument;
};
void* thread_entry(void* opaque) {
    auto* thread = static_cast<Thread*>(opaque);
    event_wait(&thread->start);
    thread->entry(thread->argument);
    return nullptr;
}
}
void* event_create() { return new (std::nothrow) Event; }
void event_release(void* event) { delete static_cast<Event*>(event); }
void event_signal(void* opaque) {
    auto& event = *static_cast<Event*>(opaque);
    { std::lock_guard guard(event.mutex); event.signaled = true; }
    event.wake.notify_one();
}
bool event_wait(void* opaque, std::int64_t timeout_ns) {
    auto& event = *static_cast<Event*>(opaque);
    std::unique_lock lock(event.mutex);
    if (timeout_ns < 0) event.wake.wait(lock, [&] { return event.signaled; });
    else if (!event.wake.wait_for(lock, std::chrono::nanoseconds(timeout_ns),
                                [&] { return event.signaled; })) return false;
    event.signaled = false;
    return true;
}
void* thread_create(ThreadEntry entry, void* argument, std::size_t stack_size) {
    auto* thread = new (std::nothrow) Thread{};
    if (!thread) return nullptr;
    thread->entry = entry;
    thread->argument = argument;
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr);
    if (error) { delete thread; errno = error; return nullptr; }
    if (stack_size) error = pthread_attr_setstacksize(&attr, stack_size);
    if (!error) error = pthread_create(&thread->native, &attr, thread_entry, thread);
    pthread_attr_destroy(&attr);
    if (error) { delete thread; errno = error; return nullptr; }
    return thread;
}
bool thread_start(void* opaque) {
    if (!opaque) return false;
    event_signal(&static_cast<Thread*>(opaque)->start);
    return true;
}
void thread_join_release(void* opaque) {
    auto* thread = static_cast<Thread*>(opaque);
    pthread_join(thread->native, nullptr);
    delete thread;
}
[[noreturn]] void thread_exit() { pthread_exit(nullptr); }
std::uint32_t thread_id() { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }
void thread_yield() { sched_yield(); }
#endif
}  // namespace host
