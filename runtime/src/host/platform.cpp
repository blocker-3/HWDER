#include "host/platform.h"

#include <cstdlib>
#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#else
#include <cerrno>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace host {
void* aligned_allocate(std::size_t size, std::size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    void* result = nullptr;
    return posix_memalign(&result, alignment, size) == 0 ? result : nullptr;
#endif
}
void aligned_release(void* allocation) {
#ifdef _WIN32
    _aligned_free(allocation);
#else
    free(allocation);
#endif
}

#ifdef _WIN32
void* memory_reserve(void* address, std::size_t size, bool write_watch) {
    return VirtualAlloc(address, size, MEM_RESERVE | (write_watch ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS);
}
bool memory_commit(void* address, std::size_t size) {
    return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
bool memory_decommit(void* address, std::size_t size) { return VirtualFree(address, size, MEM_DECOMMIT) != 0; }
bool memory_release(void* address, std::size_t) { return VirtualFree(address, 0, MEM_RELEASE) != 0; }
unsigned long memory_error() { return GetLastError(); }
#else
namespace {
std::mutex mappings_lock;
std::map<std::uintptr_t, std::size_t> mappings;
bool page_range(void* address, std::size_t size) {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (!size || start % page || size % page || start + size < start) {
        errno = EINVAL;
        return false;
    }
    return true;
}
bool contains(void* address, std::size_t size) {
    if (!page_range(address, size)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    auto it = mappings.upper_bound(start);
    if (it != mappings.begin()) {
        --it;
        if (start - it->first <= it->second && size <= it->second - (start - it->first)) return true;
    }
    errno = ENOMEM;
    return false;
}
}
void* memory_reserve(void* address, std::size_t size, bool) {
    std::lock_guard<std::mutex> guard(mappings_lock);
    if (!page_range(address, size)) return nullptr;
    // Never use MAP_FIXED: it can silently destroy a host library/stack mapping.
    // NOREPLACE is optional; the exact-address check also handles older kernels
    // which treat that flag as a hint, preserving any colliding host allocation.
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    if (address) flags |= MAP_FIXED_NOREPLACE;
#endif
    void* result = mmap(address, size, PROT_NONE, flags, -1, 0);
    if (result == MAP_FAILED) return nullptr;
    if (address && result != address) {
        munmap(result, size);
        errno = EEXIST;
        return nullptr;
    }
    mappings.emplace(reinterpret_cast<std::uintptr_t>(result), size);
    return result;
}
bool memory_commit(void* address, std::size_t size) {
    std::lock_guard<std::mutex> guard(mappings_lock);
    return contains(address, size) && mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
}
bool memory_decommit(void* address, std::size_t size) {
    std::lock_guard<std::mutex> guard(mappings_lock);
    if (!contains(address, size) || mprotect(address, size, PROT_NONE) != 0) return false;
    // Anonymous private DONTNEED guarantees zero-filled pages on recommit.
    return madvise(address, size, MADV_DONTNEED) == 0;
}
bool memory_release(void* address, std::size_t size) {
    std::lock_guard<std::mutex> guard(mappings_lock);
    const auto it = mappings.find(reinterpret_cast<std::uintptr_t>(address));
    if (it == mappings.end() || it->second != size) { errno = EINVAL; return false; }
    if (munmap(address, size) != 0) return false;
    mappings.erase(it);
    return true;
}
unsigned long memory_error() { return static_cast<unsigned long>(errno); }
#endif
std::size_t memory_readable(void* address, std::size_t size) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (!size || size > UINTPTR_MAX - start) return 0;
    std::uintptr_t position = start;
#ifdef _WIN32
    while (position < start + size) {
        MEMORY_BASIC_INFORMATION information;
        if (!VirtualQuery(reinterpret_cast<void*>(position), &information, sizeof(information)) ||
            information.State != MEM_COMMIT || (information.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        position = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
    }
#else
    // Used only when the guest's region map cannot answer (host allocations).
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[1024];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long low = 0, high = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &low, &high, permissions) != 3) continue;
        if (high <= position) continue;
        if (low > position || permissions[0] != 'r') break;
        position = static_cast<std::uintptr_t>(high);
        if (position >= start + size) break;
    }
    std::fclose(maps);
#endif
    return std::min(size, static_cast<std::size_t>(position - start));
}
}  // namespace host
