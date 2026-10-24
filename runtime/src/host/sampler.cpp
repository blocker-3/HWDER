// HWDER_SAMPLE_US: a CPU-time sampling profiler for one host thread (Linux; the GPU command thread). Every that many
// microseconds of the thread's own CPU time a SIGPROF records its instruction address. Every 10 s the counts are
// written beside the log as <log>.samples.txt: one line per address (count, offset in the executable, function),
// so `addr2line -f -i -e hwder <offset>` gives the source line. Windows: not available.
#include "host/sampler.h"

#include "host/platform.h"
#include "hwder/runtime.h"

#ifndef _WIN32
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#include <signal.h>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace host {
namespace {
constexpr std::uint32_t kMaxSamples = 1u << 22;
std::uintptr_t* g_pcs = nullptr;
std::atomic<std::uint32_t> g_count{0};

void on_sample(int, siginfo_t*, void* opaque) {
    const auto* uc = static_cast<const ucontext_t*>(opaque);
    const std::uint32_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxSamples) g_pcs[i] = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
}

void dump(const std::string& path) {
    const std::uint32_t n = std::min(g_count.load(), kMaxSamples);
    std::unordered_map<std::uintptr_t, std::uint32_t> hist;
    for (std::uint32_t i = 0; i < n; i++) hist[g_pcs[i]]++;
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> rows;
    for (auto& [pc, c] : hist) rows.push_back({c, pc});
    std::sort(rows.rbegin(), rows.rend());
    // Per function as well: the leaf address alone splits a hot loop over many lines.
    std::unordered_map<std::string, std::uint32_t> funcs;
    std::vector<std::string> lines;
    for (auto& [c, pc] : rows) {
        Dl_info info{};
        std::string name = "?";
        std::uintptr_t off = pc;
        if (dladdr(reinterpret_cast<void*>(pc), &info)) {
            off = pc - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
            if (info.dli_sname) {
                int status = 0;
                char* d = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                name = status == 0 && d ? d : info.dli_sname;
                std::free(d);
            } else if (info.dli_fname) {
                name = std::string("[") + info.dli_fname + "]";
            }
        }
        funcs[name] += c;
        if (lines.size() < 400) {
            char b[64];
            std::snprintf(b, sizeof(b), "%8u %#12llx ", c, static_cast<unsigned long long>(off));
            lines.push_back(b + name.substr(0, 160));
        }
    }
    std::vector<std::pair<std::uint32_t, std::string>> by_func;
    for (auto& [f, c] : funcs) by_func.push_back({c, f});
    std::sort(by_func.rbegin(), by_func.rend());
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::uint64_t watch_calls = 0, watch_ns = 0;
    write_watch_totals(&watch_calls, &watch_ns);
    std::fprintf(f, "%u samples; write watch: %llu calls, %.1f ms total\n\nby function:\n", n,
                 static_cast<unsigned long long>(watch_calls), watch_ns / 1e6);
    for (std::size_t i = 0; i < by_func.size() && i < 80; i++)
        std::fprintf(f, "%8u %5.1f%% %s\n", by_func[i].first, 100.0 * by_func[i].first / std::max(n, 1u),
                     by_func[i].second.substr(0, 200).c_str());
    std::fprintf(f, "\nby address (count, offset in the executable, function):\n");
    for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
}
}  // namespace

void sample_current_thread(int period_us, const std::string& path) {
    if (period_us <= 0 || g_pcs) return;
    g_pcs = static_cast<std::uintptr_t*>(std::calloc(kMaxSamples, sizeof(std::uintptr_t)));
    if (!g_pcs) return;
    struct sigaction sa{};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
    timer_t timer;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer) != 0) {
        hw_log("sampler: timer_create failed");
        return;
    }
    itimerspec spec{};
    spec.it_interval.tv_nsec = spec.it_value.tv_nsec = static_cast<long>(period_us) * 1000;
    timer_settime(timer, 0, &spec, nullptr);
    hw_log("sampler: sampling this thread every %d us of its CPU time into %s", period_us, path.c_str());
    std::thread([path] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            dump(path);
        }
    }).detach();
}
}  // namespace host
#else
namespace host {
void sample_current_thread(int period_us, const std::string&) {
    if (period_us > 0) hw_log("sampler: HWDER_SAMPLE_US is Linux-only");
}
}  // namespace host
#endif
