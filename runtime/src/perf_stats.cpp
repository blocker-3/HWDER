// Per-thread CPU and wait statistics, see perf_stats.h.
#include "perf_stats.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

#endif

// One sample of a host thread: id, CPU time so far and its name.
struct ThreadSample {
    uint32_t tid;
    double cpu_ms;
    std::string name;
};

#ifdef _WIN32
double ft_ms(const FILETIME& f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10000.0; }

double process_cpu_ms() {
    FILETIME pc, pe, pk, pu;
    GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu);
    return ft_ms(pk) + ft_ms(pu);
}

std::vector<ThreadSample> thread_samples() {
    std::vector<ThreadSample> out;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        ThreadSample t{te.th32ThreadID, GetThreadTimes(h, &c, &e, &k, &u) ? ft_ms(k) + ft_ms(u) : 0, "host"};
        PWSTR d = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0]) {
            char b[128];
            WideCharToMultiByte(CP_UTF8, 0, d, -1, b, sizeof(b), nullptr, nullptr);
            t.name = b;
        }
        if (d) LocalFree(d);
        CloseHandle(h);
        out.push_back(t);
    }
    CloseHandle(snap);
    return out;
}
#else
// utime + stime of a /proc stat line, in ms (fields 14 and 15, counted after the parenthesised name).
double stat_cpu_ms(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 0;
    char line[1024];
    const bool got = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    const char* rest = got ? std::strrchr(line, ')') : nullptr;
    unsigned long long utime = 0, stime = 0;
    if (!rest || std::sscanf(rest + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) return 0;
    static const double tick_ms = 1000.0 / (double)sysconf(_SC_CLK_TCK);
    return (double)(utime + stime) * tick_ms;
}

double process_cpu_ms() { return stat_cpu_ms("/proc/self/stat"); }

std::vector<ThreadSample> thread_samples() {
    std::vector<ThreadSample> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        const std::string dir = entry.path().string();
        const uint32_t tid = (uint32_t)std::strtoul(entry.path().filename().c_str(), nullptr, 10);
        if (!tid) continue;
        ThreadSample t{tid, stat_cpu_ms(dir + "/stat"), "host"};
        if (FILE* f = std::fopen((dir + "/comm").c_str(), "r")) {
            char b[64];
            if (std::fgets(b, sizeof(b), f)) {
                b[std::strcspn(b, "\n")] = 0;
                if (b[0]) t.name = b;
            }
            std::fclose(f);
        }
        out.push_back(t);
    }
    return out;
}
#endif

struct Row {
    std::string name;
    double cpu_pct, wait_ms[kWaitCount], ecore_pct;
    int64_t sleeps, sleep_req, sleep_over;
    uint32_t tid;
};

void reporter(int period_ms) {
    host::name_thread(L"HWDER perf stats");
    std::map<uint32_t, double> prev_cpu;
    std::map<uint32_t, int64_t> prev_wait[kWaitCount];
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> prev_core;
    std::map<uint32_t, int64_t> prev_sl[3];
    host::Counter freq, last, now;
    host::counter_frequency(&freq);
    host::counter(&last);
    uint64_t last_presents = g_presents.load();
    double last_proc = process_cpu_ms();
    for (;;) {
        host::sleep_ms((uint32_t)period_ms);
        host::counter(&now);
        const double wall = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        last = now;
        const uint64_t presents = g_presents.load();
        const double proc = process_cpu_ms();

        // Guest thread labels by host thread id.
        std::map<uint32_t, std::string> guest;
        for (auto& t : kern::all_threads()) {
            if (!t->host_handle || t->exited) continue;
            char b[160];
            snprintf(b, sizeof(b), "guest %llu %s prio %d core %d entry %llx", (unsigned long long)t->id, t->name.c_str(), t->priority,
                     t->ideal_core, (unsigned long long)t->entry);
            guest[host::thread_os_id(t->host_handle)] = b;
        }
        std::map<uint32_t, ThreadRec*> recs;
        {
            std::lock_guard<std::mutex> l(g_m);
            for (ThreadRec* r : g_recs) recs[r->tid] = r;
        }

        std::vector<Row> rows;
        for (const ThreadSample& sample : thread_samples()) {
            Row r{};
            r.tid = sample.tid;
            auto pit = prev_cpu.find(r.tid);
            r.cpu_pct = pit == prev_cpu.end() ? 0 : (sample.cpu_ms - pit->second) * 100.0 / wall;
            prev_cpu[r.tid] = sample.cpu_ms;
            if (auto g = guest.find(r.tid); g != guest.end()) r.name = g->second;
            else r.name = sample.name;
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
#endif
        std::thread(reporter, std::max(500, period())).detach();
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
