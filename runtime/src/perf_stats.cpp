// Per-thread CPU and wait statistics, see perf_stats.h.
#include "perf_stats.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "debug_flags.h"
#include "host/platform.h"
#include "host/timing.h"
#include "hwder/runtime.h"
#include "kernel/kernel.h"

namespace perf {

namespace {

struct ThreadRec {
    uint32_t tid = 0;
    std::atomic<int64_t> wait[kWaitCount]{};
    std::atomic<uint64_t> samples{0}, ecore{0};
    std::atomic<int64_t> sleeps{0}, sleep_req{0}, sleep_over{0};
};

std::mutex g_m;
std::vector<ThreadRec*> g_recs;  // never freed: a few dozen threads per run
thread_local ThreadRec* t_rec;
std::atomic<uint64_t> g_presents{0};
std::atomic<uint32_t> g_presenter{0};
bool g_ecore[256];  // logical processor index -> efficiency core
bool g_hybrid;

ThreadRec* rec() {
    if (!t_rec) {
        t_rec = new ThreadRec;
        t_rec->tid = host::thread_id();
        std::lock_guard<std::mutex> l(g_m);
        g_recs.push_back(t_rec);
    }
    return t_rec;
}

#ifdef _WIN32
void load_core_classes() {
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<char> buf(len);
    if (!len || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0)) return;
    BYTE max_class = 0;
    for (ULONG o = 0; o < len;) {
        auto* s = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + o);
        if (s->Type == CpuSetInformation) max_class = std::max(max_class, s->CpuSet.EfficiencyClass);
        o += s->Size;
    }
    int p = 0, e = 0;
    for (ULONG o = 0; o < len;) {
        auto* s = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + o);
        if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < 256) {
            bool ec = s->CpuSet.EfficiencyClass < max_class;
            g_ecore[s->CpuSet.LogicalProcessorIndex] = ec;
            (ec ? e : p)++;
        }
        o += s->Size;
    }
    g_hybrid = e > 0;
    hw_log("perf: %d performance and %d efficiency logical processors", p, e);
}

double ft_ms(const FILETIME& f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10000.0; }

struct Row {
    std::string name;
    double cpu_pct, wait_ms[kWaitCount], ecore_pct;
    int64_t sleeps, sleep_req, sleep_over;
    DWORD tid;
};

void reporter(int period_ms) {
    SetThreadDescription(GetCurrentThread(), L"HWDER perf stats");
    std::map<DWORD, double> prev_cpu;
    std::map<DWORD, int64_t> prev_wait[kWaitCount];
    std::map<DWORD, std::pair<uint64_t, uint64_t>> prev_core;
    std::map<DWORD, int64_t> prev_sl[3];
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    uint64_t last_presents = g_presents.load();
    FILETIME pc, pe, pk, pu;
    GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu);
    double last_proc = ft_ms(pk) + ft_ms(pu);
    const DWORD pid = GetCurrentProcessId();
    for (;;) {
        Sleep(period_ms);
        QueryPerformanceCounter(&now);
        const double wall = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        last = now;
        const uint64_t presents = g_presents.load();
        GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu);
        const double proc = ft_ms(pk) + ft_ms(pu);

        // Guest thread labels by host thread id.
        std::map<DWORD, std::string> guest;
        for (auto& t : kern::all_threads()) {
            if (!t->host_handle || t->exited) continue;
            char b[160];
            snprintf(b, sizeof(b), "guest %llu %s prio %d core %d entry %llx", (unsigned long long)t->id, t->name.c_str(), t->priority,
                     t->ideal_core, (unsigned long long)t->entry);
            guest[GetThreadId(t->host_handle)] = b;
        }
        std::map<DWORD, ThreadRec*> recs;
        {
            std::lock_guard<std::mutex> l(g_m);
            for (ThreadRec* r : g_recs) recs[r->tid] = r;
        }

        std::vector<Row> rows;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c, e, k, u;
            double cpu = GetThreadTimes(h, &c, &e, &k, &u) ? ft_ms(k) + ft_ms(u) : 0;
            Row r{};
            r.tid = te.th32ThreadID;
            auto pit = prev_cpu.find(r.tid);
            r.cpu_pct = pit == prev_cpu.end() ? 0 : (cpu - pit->second) * 100.0 / wall;
            prev_cpu[r.tid] = cpu;
            if (auto g = guest.find(r.tid); g != guest.end()) r.name = g->second;
            else {
                PWSTR d = nullptr;
                if (SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0]) {
                    char b[128];
                    WideCharToMultiByte(CP_UTF8, 0, d, -1, b, sizeof(b), nullptr, nullptr);
                    r.name = b;
                } else r.name = "host";
                if (d) LocalFree(d);
            }
            CloseHandle(h);
            if (r.tid == g_presenter.load()) r.name += " [presents]";
            if (auto ri = recs.find(r.tid); ri != recs.end()) {
                ThreadRec* t = ri->second;
                for (int i = 0; i < kWaitCount; i++) {
                    int64_t v = t->wait[i].load();
                    r.wait_ms[i] = (v - prev_wait[i][r.tid]) * 1000.0 / freq.QuadPart;
                    prev_wait[i][r.tid] = v;
                }
                const int64_t sl[3] = {t->sleeps.load(), t->sleep_req.load(), t->sleep_over.load()};
                r.sleeps = sl[0] - prev_sl[0][r.tid], r.sleep_req = sl[1] - prev_sl[1][r.tid], r.sleep_over = sl[2] - prev_sl[2][r.tid];
                for (int i = 0; i < 3; i++) prev_sl[i][r.tid] = sl[i];
                uint64_t s = t->samples.load(), ec = t->ecore.load();
                auto& pcore = prev_core[r.tid];
                r.ecore_pct = s > pcore.first ? (ec - pcore.second) * 100.0 / (s - pcore.first) : -1;
                pcore = {s, ec};
            } else r.ecore_pct = -1;
            rows.push_back(r);
        }
        CloseHandle(snap);
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu_pct > b.cpu_pct; });
        const double frames = (double)(presents - last_presents);
        hw_log("perf: %.0f ms window: %.1f guest fps, process CPU %.2f cores, %zu threads", wall, frames * 1000.0 / wall,
               (proc - last_proc) / wall, rows.size());
        last_presents = presents;
        last_proc = proc;
        int shown = 0;
        for (const Row& r : rows) {
            double waits = 0;
            for (double w : r.wait_ms) waits += w;
            if (r.cpu_pct < 1.0 && waits < 1.0) continue;
            if (shown++ >= 16) break;
            char ec[96] = "";
            int eo = 0;
            if (g_hybrid && r.ecore_pct >= 0) eo = snprintf(ec, sizeof(ec), " ecore %.0f%%", r.ecore_pct);
            if (r.sleeps > 0)
                snprintf(ec + eo, sizeof(ec) - eo, " sleeps %lld (mean %.3f ms, overshoot %.3f ms)", (long long)r.sleeps,
                         r.sleep_req / 1e6 / r.sleeps, r.sleep_over / 1e6 / r.sleeps);
            // Per-frame figures for the presenting thread and anything waiting.
            const double per = frames > 0 ? 1.0 / frames : 0;
            hw_log("perf:   %5u %5.1f%% cpu (%.2f ms/frame) wait/frame kernel %.2f syncpt %.2f dequeue %.2f gpuq %.2f%s | %s", (unsigned)r.tid,
                   r.cpu_pct, r.cpu_pct / 100.0 * wall * per, r.wait_ms[kWaitKernel] * per, r.wait_ms[kWaitSyncpt] * per,
                   r.wait_ms[kWaitDequeue] * per, r.wait_ms[kWaitGpuQueue] * per, ec, r.name.c_str());
        }
    }
}
#endif

int period() {
    static const int p = debug::value(debug::ThreadStats, 0);
    return p;
}

}  // namespace

bool enabled() { return period() > 0; }

void start() {
    static std::once_flag once;
    if (!enabled()) return;
    std::call_once(once, [] {
#ifdef _WIN32
        load_core_classes();
        std::thread(reporter, std::max(500, period())).detach();
#else
        hw_log("perf: HWDER_THREAD_STATS per-thread report is not ported to Linux yet; sleep/wait counters only");
#endif
    });
}

void add_wait(WaitKind k, int64_t qpc_ticks) {
    if (!enabled()) return;
    rec()->wait[k].fetch_add(qpc_ticks, std::memory_order_relaxed);
}

void sample_core() {
    if (!enabled() || !g_hybrid) return;
#ifdef _WIN32
    ThreadRec* r = rec();
    DWORD cpu = GetCurrentProcessorNumber();
    r->samples.fetch_add(1, std::memory_order_relaxed);
    if (cpu < 256 && g_ecore[cpu]) r->ecore.fetch_add(1, std::memory_order_relaxed);
#endif
}

void note_sleep(int64_t req_ns, int64_t actual_ns) {
    if (!enabled()) return;
    ThreadRec* r = rec();
    r->sleeps.fetch_add(1, std::memory_order_relaxed);
    r->sleep_req.fetch_add(req_ns, std::memory_order_relaxed);
    r->sleep_over.fetch_add(actual_ns - req_ns, std::memory_order_relaxed);
}

void note_present() {
    if (!enabled()) return;
    g_presents.fetch_add(1, std::memory_order_relaxed);
    g_presenter.store(host::thread_id(), std::memory_order_relaxed);
}

WaitScope::WaitScope(WaitKind kind) : k(kind) {
    if (!enabled()) return;
    host::Counter a;
    host::counter(&a);
    t0 = a.QuadPart;
}
WaitScope::~WaitScope() {
    if (!t0) return;
    host::Counter b;
    host::counter(&b);
    add_wait(k, b.QuadPart - t0);
}

}  // namespace perf
