#include "host/platform.h"

#include <cstdlib>
#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#else
#include <cerrno>
#include <atomic>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
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
void write_watch_totals(std::uint64_t* calls, std::uint64_t* ns) { *calls = *ns = 0; }
bool write_watch_reset(void* address, std::size_t size, void** pages, std::size_t* count) {
    ULONG_PTR n = *count;
    ULONG granularity = 0;
    const bool ok = GetWriteWatch(WRITE_WATCH_FLAG_RESET, address, size, pages, &n, &granularity) == 0;
    *count = ok ? (std::size_t)n : 0;
    return ok;
}
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

// Write watch: write-watched reservations are registered with one userfaultfd in async write-protect mode, so
// a write just clears the page's protection (no fault reaches us) and PAGEMAP_SCAN reports and re-protects
// written pages in one call. Needs kernel 6.7+; anything missing leaves the texture cache's hash fallback.
#if defined(HWDER_REQUIRE_WRITE_WATCH) && !(defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY))
#error "Release toolchain's Linux headers lack PAGEMAP_SCAN/userfaultfd write protection (need 6.7+ headers)"
#endif
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
#define HWDER_WRITE_WATCH 1
int g_uffd = -2;  // -2 not tried, -1 unavailable
int g_pagemap = -1;
std::map<std::uintptr_t, std::size_t> watched;  // under mappings_lock

bool write_watch_init() {
    if (g_uffd != -2) return g_uffd >= 0;
    g_uffd = -1;
    const int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
    const int pagemap = fd >= 0 ? open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0 || pagemap < 0 || ioctl(fd, UFFDIO_API, &api) != 0) {
        std::fprintf(stderr, "[hwder] mem: no write watch (userfaultfd async write-protect: errno %d); textures are re-hashed\n", errno);
        if (fd >= 0) close(fd);
        if (pagemap >= 0) close(pagemap);
        return false;
    }
    g_uffd = fd;
    g_pagemap = pagemap;
    return true;
}

void write_watch_add(void* address, std::size_t size) {
    if (!write_watch_init()) return;
    uffdio_register reg{};
    reg.range = {reinterpret_cast<std::uintptr_t>(address), size};
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    uffdio_writeprotect wp{};
    wp.range = reg.range;
    wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    if (ioctl(g_uffd, UFFDIO_REGISTER, &reg) != 0 || ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp) != 0) {
        std::fprintf(stderr, "[hwder] mem: write watch registration failed (errno %d)\n", errno);
        return;
    }
    watched.emplace(reinterpret_cast<std::uintptr_t>(address), size);
}

bool watched_range(std::uintptr_t start, std::size_t size) {
    auto it = watched.upper_bound(start);
    if (it == watched.begin()) return false;
    --it;
    return start - it->first < it->second && size <= it->second - (start - it->first);
}
#endif
}
void* memory_reserve(void* address, std::size_t size, bool write_watch) {
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
#ifdef HWDER_WRITE_WATCH
    if (write_watch) write_watch_add(result, size);
#else
    (void)write_watch;
#endif
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
#ifdef HWDER_WRITE_WATCH
    watched.erase(reinterpret_cast<std::uintptr_t>(address));
#endif
    return true;
}
unsigned long memory_error() { return static_cast<unsigned long>(errno); }
namespace {
std::atomic<std::uint64_t> g_watch_calls{0}, g_watch_ns{0};
}
void write_watch_totals(std::uint64_t* calls, std::uint64_t* ns) {
    *calls = g_watch_calls.load();
    *ns = g_watch_ns.load();
}
bool write_watch_reset(void* address, std::size_t size, void** pages, std::size_t* count) {
    const std::size_t capacity = *count;
    *count = 0;
#ifdef HWDER_WRITE_WATCH
    struct Tally {
        std::uint64_t t0 = monotonic_ns();
        ~Tally() {
            g_watch_calls.fetch_add(1, std::memory_order_relaxed);
            g_watch_ns.fetch_add(monotonic_ns() - t0, std::memory_order_relaxed);
        }
    } tally;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    {
        std::lock_guard<std::mutex> guard(mappings_lock);
        if (!page_range(address, size) || !watched_range(start, size)) return false;
    }
    static const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    page_region regions[64];
    pm_scan_arg arg{};
    arg.size = sizeof(arg);
    arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
    arg.start = start;
    arg.end = start + size;
    arg.vec = reinterpret_cast<std::uintptr_t>(regions);
    arg.vec_len = 64;
    arg.category_mask = PAGE_IS_WRITTEN;
    arg.return_mask = PAGE_IS_WRITTEN;
    while (arg.start < arg.end && *count < capacity) {
        arg.max_pages = capacity - *count;  // never re-protect a page that cannot be reported
        const long n = ioctl(g_pagemap, PAGEMAP_SCAN, &arg);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        for (long r = 0; r < n; r++)
            for (std::uintptr_t p = regions[r].start; p < regions[r].end && *count < capacity; p += page)
                pages[(*count)++] = reinterpret_cast<void*>(p);
        if (arg.walk_end <= arg.start) break;
        arg.start = arg.walk_end;
    }
    return true;
#else
    (void)address;
    (void)size;
    (void)pages;
    (void)capacity;
    return false;
#endif
}
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
