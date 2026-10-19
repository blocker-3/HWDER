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
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "hwder/runtime.h"
#include "interp/camera.h"
#include "kernel/kernel.h"

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


// HWDER_MAT_PROBE=1 (with the census): find the per-object uniform blocks. Each object's block holds
// mW2P, which equals the camera's viewProj; scan the pages Draw wrote for it (4 layouts), log the
// matrix stored just before it (mL2W) and search the heap for those bytes (the object's own matrix).
void matrix_scan(u32 n) {
    static const bool on = getenv("HWDER_MAT_PROBE") != nullptr;
    static int done = 0;
    if (!on || done++ >= 2) return;
    // The update threads keep running while this scan holds Draw up, so match any of the recent ticks'
    // cameras: view and viewProj, as stored (4x4 / first 3 rows) and transposed.
    interp::CamSnap snaps[8];
    int ns = interp::snapshots(snaps, 8);
    if (ns < 1) return;
    struct Pat { float m[16]; int bytes; const char* name; int tick; };
    std::vector<Pat> pats;
    for (int i = 0; i < ns; i++)
        for (int which = 0; which < 2; which++) {
            const float* src = which ? snaps[i].viewproj : snaps[i].view;
            Pat p{}, t{};
            memcpy(p.m, src, 64);
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) t.m[r * 4 + k] = src[k * 4 + r];
            p.bytes = t.bytes = 48;
            p.name = which ? "viewProj" : "view";
            t.name = which ? "viewProjT" : "viewT";
            p.tick = t.tick = i;
            pats.push_back(p);
            pats.push_back(t);
        }
    std::map<u64, std::string> pages;  // draw-thread pages and their writer
    for (u32 i = 0; i < n; i++)
        if (g_hits[i].tid == g_draw_tid) pages[g_hits[i].page & ~(kPage - 1)] = sym(g_hits[i].rip);
    std::map<std::string, int> count;
    std::map<u64, int> per_page;  // pages holding viewProj copies (object blocks?)
    int logged = 0;
    std::vector<std::pair<u64, const Pat*>> found;
    for (auto& rg : g_ranges)
        for (u64 a = rg.a; a + 64 <= rg.a + rg.n; a += 4) {
            u32 w = *(const u32*)a;
            for (auto& p : pats) {
                if (w != *(const u32*)p.m || memcmp((void*)a, p.m, p.bytes)) continue;
                count[p.name]++;
                if (p.name[4] == 'P') {
                    per_page[a & ~(kPage - 1)]++;
                    found.push_back({a, &p});
                }
                if (logged++ < 40) {
                    const float* f = (const float*)(a - 64);
                    auto pw = pages.find(a & ~(kPage - 1));
                    hw_log("matprobe: %s (tick -%d) at %llx (draw page writer %s); 64 bytes before: %g %g %g %g | %g %g %g %g | "
                           "%g %g %g %g | %g %g %g %g", p.name, p.tick, (unsigned long long)a,
                           pw == pages.end() ? "-" : pw->second.c_str(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9],
                           f[10], f[11], f[12], f[13], f[14], f[15]);
                }
                break;
            }
        }
    std::string sum;
    for (auto& [k, v] : count) sum += " " + k + "=" + std::to_string(v);
    hw_log("matprobe: copies in all read-write memory:%s; viewProj copies on %zu pages", sum.c_str(), per_page.size());
    // Sources of the matrices stored just before viewProj copies (mL2W, 3x4 rows): search all memory
    // for their 48 bytes, outside the pages holding viewProj copies.
    int searched = 0;
    for (auto& [a, p] : found) {
        if (searched >= 8) break;
        const float* m = (const float*)(a - 64);
        if (!(std::fabs(m[3]) > 1.0f)) continue;  // want a translation in the 4th column
        searched++;
        int src = 0;
        for (auto& rg : g_ranges)
            for (u64 q = rg.a; q + 48 <= rg.a + rg.n; q += 4) {
                if (*(const u32*)q != *(const u32*)m || memcmp((void*)q, m, 48)) continue;
                if (per_page.count(q & ~(kPage - 1))) continue;
                if (src++ < 6) {
                    const float* f = (const float*)q;
                    hw_log("matprobe:   mL2W before %llx found at %llx; 16 bytes after: %g %g %g %g; 32 before: %g %g %g %g %g %g %g %g",
                           (unsigned long long)a, (unsigned long long)q, f[12], f[13], f[14], f[15], f[-8], f[-7], f[-6], f[-5],
                           f[-4], f[-3], f[-2], f[-1]);
                }
            }
        hw_log("matprobe:   mL2W before %llx: %d copies elsewhere", (unsigned long long)a, src);
    }
    int shown = 0;
    for (auto& [pg, k] : per_page)
        if (k > 2 && shown++ < 10) {
            auto pw = pages.find(pg);
            hw_log("matprobe:   page %llx holds %d viewProj copies (draw page writer %s)", (unsigned long long)pg, k,
                   pw == pages.end() ? "-" : pw->second.c_str());
        }
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
    matrix_scan(g_nhits.load() > (sizeof g_hits / sizeof g_hits[0]) ? (u32)(sizeof g_hits / sizeof g_hits[0]) : g_nhits.load());
}

// HWDER_MAT_PROBE=1 without the census: right after one Draw, with every other guest thread suspended
// (so the matrices Draw just copied still match their sources), scan memory as above.
extern "C" void hw_mat_probe_after_draw() {
    static const bool on = getenv("HWDER_MAT_PROBE") != nullptr && budget() <= 0;
    static int done = 0;
    if (!on || done++ != 120) return;  // two seconds after the probe arms
    std::vector<HANDLE> held;
    for (auto& t : kern::all_threads()) {
        if (t.get() == kern::current_thread() || !t->started || t->exited || !t->host_handle) continue;
        if (SuspendThread(t->host_handle) != (DWORD)-1) held.push_back(t->host_handle);
    }
    g_ranges.clear();
    collect(0x2000000000ull, 0x180000000ull);
    collect(0x3000000000ull, 0x1000000000ull);
    collect(0x1000000000ull, 0x100000000ull);
    g_nhits = 0;
    g_draw_tid = GetCurrentThreadId();
    hw_log("matprobe: %zu guest threads suspended", held.size());
    matrix_scan(0);
    for (HANDLE h : held) ResumeThread(h);
}
