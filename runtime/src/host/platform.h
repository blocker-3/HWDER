// Narrow host services used by the guest memory manager and CPU contexts.
// No guest data, renderer API, or OS handle types cross this boundary.
#pragma once
#include <cstddef>
#include <cstdint>

namespace host {
void* aligned_allocate(std::size_t size, std::size_t alignment);
void aligned_release(void* allocation);
// Reservations never replace existing mappings. A non-null address must match exactly.
// write_watch requests the Windows optimization; Linux uses the renderer's hash fallback.
void* memory_reserve(void* address, std::size_t size, bool write_watch = false);
bool memory_commit(void* address, std::size_t size);
bool memory_decommit(void* address, std::size_t size);
bool memory_release(void* address, std::size_t size);
unsigned long memory_error();

// Auto-reset events retain one pending wake (including a wake before a wait).
void* event_create();
void event_release(void* event);
void event_signal(void* event);
bool event_wait(void* event, std::int64_t timeout_ns = -1);
// Create dormant: the callback cannot run until thread_start. Handles are opaque;
// Windows keeps real HANDLE values so existing Windows-only diagnostics still work.
using ThreadEntry = void (*)(void*);
void* thread_create(ThreadEntry entry, void* argument, std::size_t stack_size);
bool thread_start(void* thread);
void thread_join_release(void* thread);
[[noreturn]] void thread_exit();
// Runtime workers live until process termination; do not run global destructors concurrently.
[[noreturn]] void process_exit(int code);
std::uint32_t thread_id();
void thread_yield();
std::uint64_t monotonic_ns();
}  // namespace host
