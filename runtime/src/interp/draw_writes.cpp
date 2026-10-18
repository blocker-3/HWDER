// High frame rate, stage a (docs/frame-loop.md): which guest memory does the Draw step write?
//
// HWDER_DRAW_WRITES=<n>: once the battle is up, on n sampled ticks (one every 2 s) every
// read-write page of the heap, alias region and main module is made read-only for the duration of
// scene A's slot 5 (the Draw side). A vectored handler catches each first write to a page, makes
// it writable again and records the page, the writing thread and the host code address. After the
// sampled Draw returns, the pages go back to read-write and the writes are logged, grouped by the
// recompiled function that made them (symbol from the PDB). Pages written by other threads while
// Draw runs are counted separately. Diagnostic only: file reads into guest memory during the window
// would fail, so keep n small.
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "hwder/runtime.h"

extern "C" std::atomic<u32> hw_last_frame_draws;

namespace {

struct Range { u64 a, n; };
struct Hit { u64 page, rip; DWORD tid; u64 x0; };

constexpr u64 kPage = 0x1000;
std::vector<Range> g_ranges;
std::atomic<bool> g_active{false};
DWORD g_draw_tid = 0;
Hit g_hits[1 << 16];
std::atomic<u32> g_nhits{0};

int budget() {
    static const int n = getenv("HWDER_DRAW_WRITES") ? atoi(getenv("HWDER_DRAW_WRITES")) : 0;
    return n;
}

bool in_ranges(u64 a) {
    for (auto& r : g_ranges)
        if (a >= r.a && a < r.a + r.n) return true;
    return false;
}

LONG WINAPI handler(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_active.load()) return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;  // writes only
    u64 a = er->ExceptionInformation[1];
    if (!in_ranges(a)) return EXCEPTION_CONTINUE_SEARCH;
    u64 page = a & ~(kPage - 1);
    DWORD old;
    if (!VirtualProtect((void*)page, kPage, PAGE_READWRITE, &old)) return EXCEPTION_CONTINUE_SEARCH;
    u32 i = g_nhits.fetch_add(1);
    if (i < (sizeof g_hits / sizeof g_hits[0]))
        g_hits[i] = {a, (u64)er->ExceptionAddress, GetCurrentThreadId(), 0};
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Read-write committed sub-ranges of [base, base + size).
void collect(u64 base, u64 size) {
    for (u64 a = base; a < base + size;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void*)a, &mi, sizeof mi)) break;
        u64 end = (u64)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE) g_ranges.push_back({a, end - a});
        a = end;
    }
}

void protect(DWORD prot) {
    for (auto& r : g_ranges) {
        DWORD old;
        VirtualProtect((void*)r.a, r.n, prot, &old);
    }
}

std::string sym(u64 pc) {
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* s = (SYMBOL_INFO*)buf;
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 255;
    DWORD64 d = 0;
    return SymFromAddr(GetCurrentProcess(), pc, &d, s) ? s->Name : "?";
}

const char* region(u64 a) {
    if (a >= 0x2000000000ull && a < 0x2180000000ull) return "heap";
    if (a >= 0x3000000000ull && a < 0x4000000000ull) return "alias";
    if (a >= 0x1000000000ull && a < 0x1100000000ull) return "main";
    return "?";
}

void report(int sample, u64 self, double ms) {
    u32 n = g_nhits.load();
    if (n > (sizeof g_hits / sizeof g_hits[0])) n = sizeof g_hits / sizeof g_hits[0];
    struct Agg { u32 pages = 0; u64 first = 0, last = 0; std::map<std::string, u32> regs; };
    std::map<std::string, Agg> draw;   // writer function -> pages
    std::map<DWORD, u32> other;        // other threads -> pages
    for (u32 i = 0; i < n; i++) {
        const Hit& h = g_hits[i];
        if (h.tid != g_draw_tid) { other[h.tid]++; continue; }
        Agg& g = draw[sym(h.rip)];
        if (!g.pages++) g.first = h.page;
        g.last = h.page;
        g.regs[region(h.page)]++;
    }
    hw_log("drawwrites: sample %d (S=%llx, %.2f ms, %zu ranges): %u pages written, %zu writer functions on the draw thread %lu",
           sample, (unsigned long long)self, ms, g_ranges.size(), n, draw.size(), g_draw_tid);
    for (auto& [fn, g] : draw) {
        std::string regs;
        for (auto& [r, k] : g.regs) regs += std::string(" ") + r + "=" + std::to_string(k);
        hw_log("drawwrites:   %-28s %4u pages (%s) first %llx last %llx", fn.c_str(), g.pages, regs.c_str() + 1,
               (unsigned long long)g.first, (unsigned long long)g.last);
    }
    for (auto& [tid, k] : other) hw_log("drawwrites:   other thread %lu: %u pages", tid, k);
}

}  // namespace

// Called by the frame probe's slot 5 hook around the original.
extern "C" bool hw_draw_writes_begin(u64 self) {
    static int done = 0;
    static u64 next = 0;
    if (budget() <= 0 || done >= budget() || hw_last_frame_draws.load() <= 450) return false;
    u64 now = GetTickCount64();
    if (now < next) return false;
    next = now + 2000;
    static bool init = [] {
        AddVectoredExceptionHandler(1, handler);
        return true;
    }();
    (void)init;
    g_ranges.clear();
    collect(0x2000000000ull, 0x180000000ull);
    collect(0x3000000000ull, 0x1000000000ull);
    collect(0x1000000000ull, 0x100000000ull);
    g_nhits = 0;
    g_draw_tid = GetCurrentThreadId();
    done++;
    g_active = true;  // before the pages go read-only: a fault on another thread must find the handler armed
    protect(PAGE_READONLY);
    (void)self;
    return true;
}

extern "C" void hw_draw_writes_end(u64 self, double ms) {
    static int sample = 0;
    protect(PAGE_READWRITE);
    g_active = false;
    report(++sample, self, ms);
}
