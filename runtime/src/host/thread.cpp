#include "host/platform.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <new>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csetjmp>
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
[[noreturn]] void process_exit(int code) { ExitProcess(static_cast<UINT>(code)); }
std::uint32_t thread_id() { return GetCurrentThreadId(); }
std::uint32_t thread_os_id(void* thread) { return GetThreadId(thread); }
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
    Event complete;
    ThreadEntry entry;
    void* argument;
    std::atomic<std::uint32_t> tid{0};
};
void signal_complete(void* opaque) {
    event_signal(&static_cast<Thread*>(opaque)->complete);
}
// thread_exit jumps back here, like ExitThread on Windows: no unwinding of the frames in between. pthread_exit's
// forced unwind runs glibc's libgcc_s unwinder through our C++ personality routine, which crashes when the C++
// runtime brings its own unwinder (the Linux release links LLVM's libc++/libunwind statically).
thread_local std::jmp_buf* t_exit_jump = nullptr;
void* thread_entry(void* opaque) {
    auto thread = *static_cast<std::shared_ptr<Thread>*>(opaque);
    delete static_cast<std::shared_ptr<Thread>*>(opaque);
    // pthread_exit also executes this handler. Detached native stacks are reclaimed
    // at exit; the public handle remains waitable through its separate completion event.
    pthread_cleanup_push(signal_complete, thread.get());
    thread->tid = thread_id();
    event_wait(&thread->start);
    std::jmp_buf exit_jump;
    if (!setjmp(exit_jump)) {
        t_exit_jump = &exit_jump;
        thread->entry(thread->argument);
    }
    t_exit_jump = nullptr;
    pthread_cleanup_pop(1);
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
    auto thread = std::make_shared<Thread>();
    thread->entry = entry;
    thread->argument = argument;
    auto* handle = new (std::nothrow) std::shared_ptr<Thread>(thread);
    auto* worker = new (std::nothrow) std::shared_ptr<Thread>(thread);
    if (!handle || !worker) { delete handle; delete worker; return nullptr; }
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr);
    if (error) { delete handle; delete worker; errno = error; return nullptr; }
    error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!error && stack_size) error = pthread_attr_setstacksize(&attr, stack_size);
    if (!error) error = pthread_create(&thread->native, &attr, thread_entry, worker);
    pthread_attr_destroy(&attr);
    if (error) { delete handle; delete worker; errno = error; return nullptr; }
    return handle;
}
bool thread_start(void* opaque) {
    if (!opaque) return false;
    event_signal(&(*static_cast<std::shared_ptr<Thread>*>(opaque))->start);
    return true;
}
void thread_join_release(void* opaque) {
    auto* handle = static_cast<std::shared_ptr<Thread>*>(opaque);
    event_wait(&(*handle)->complete);
    // The worker retains its own shared state until after signal_complete returns.
    delete handle;
}
[[noreturn]] void thread_exit() {
    if (t_exit_jump) std::longjmp(*t_exit_jump, 1);
    pthread_exit(nullptr);  // a thread not started by thread_create
}
[[noreturn]] void process_exit(int code) { std::_Exit(code); }
std::uint32_t thread_id() { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }
std::uint32_t thread_os_id(void* opaque) {
    return opaque ? (*static_cast<std::shared_ptr<Thread>*>(opaque))->tid.load() : 0;
}
void thread_yield() { sched_yield(); }
#endif
}  // namespace host
