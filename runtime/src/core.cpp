// HWDER runtime core: code dispatch, logging/crash reporting, and process boot.
#include <exception>
#include <csignal>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <timeapi.h>
#else
#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <filesystem>
#include <mutex>
#include <thread>
#include "host/platform.h"
#include "host/timing.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hwder/dispatch.h"
#include "hwder/module.h"
#include "hwder/runtime.h"
#include "kernel/kernel.h"
#include "service/ipc.h"
#include "symbols.h"
#include "gpu/display.h"
#include "gpu/prelaunch.h"
#include "gpu/renderer.h"
#include "gpu/settings.h"
#include "fs/nsp.h"
#include "fs/save_dir.h"
#include "perf_stats.h"
#include "debug_flags.h"

// ------------------------------------------------------------------ dispatch
// One dense table per module text segment: (addr - text_lo) / 4 -> host function.
struct TextTable {
    u64 lo, hi;
    std::vector<HwFn> fns;
};
static std::vector<TextTable> g_tables;
extern "C" const unsigned long long hw_resumable[];
extern "C" const unsigned hw_resumable_count;
static std::vector<u64> g_starts;  // sorted function starts

extern "C" int hw_guest_libc = 0;

static void build_dispatch() {
    hw_guest_libc = debug::flag(debug::GuestLibc) ? 1 : 0;
    for (unsigned m = 0; m < hw_module_count; m++) {
        TextTable t{hw_modules[m].text_lo, hw_modules[m].text_hi, {}};
        t.fns.assign((t.hi - t.lo) >> 2, nullptr);
        g_tables.push_back(std::move(t));
    }
    for (unsigned i = 0; i < hw_func_count; i++) {
        const HwFuncEntry& e = hw_func_table[i];
        for (auto& t : g_tables)
            if (e.addr >= t.lo && e.addr < t.hi) t.fns[(e.addr - t.lo) >> 2] = e.fn;
    }
    g_starts.reserve(hw_func_count);
    for (unsigned i = 0; i < hw_func_count; i++) g_starts.push_back(hw_func_table[i].addr);
    std::sort(g_starts.begin(), g_starts.end());
    hw_log("dispatch: %u functions in %u modules (%u resumable)", hw_func_count, hw_module_count, hw_resumable_count);
}

extern "C" HwFn hw_lookup(u64 addr) {
    for (auto& t : g_tables)
        if (addr >= t.lo && addr < t.hi) return t.fns[(addr - t.lo) >> 2];
    return nullptr;
}

// Name from tools/recomp/symbols.txt for a guest function start (hw_symbols is sorted by address).
extern "C" const char* hw_symbol_name(u64 addr) {
    unsigned lo = 0, hi = hw_symbol_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (hw_symbols[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < hw_symbol_count && hw_symbols[lo].addr == addr ? hw_symbols[lo].name : nullptr;
}

// "f_<addr>" -> "f_<addr>(Name)" when the function is named; other symbols unchanged.
static std::string pretty_symbol(const char* sym) {
    std::string s = sym;
    if (s.rfind("f_", 0) == 0) {
        char* end = nullptr;
        u64 addr = strtoull(s.c_str() + 2, &end, 16);
        if (end && *end == 0)
            if (const char* name = hw_symbol_name(addr)) s += std::string("(") + name + ")";
    }
    return s;
}

// Paths are UTF-8 internally (the ANSI code page cannot spell every folder name a user may have).
#ifdef _WIN32
static std::wstring to_w(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string to_u8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string env_u8(const char* name) {
    const wchar_t* e = _wgetenv(to_w(name).c_str());
    return e ? to_u8(e) : std::string();
}
static FILE* fopen_u8(const std::string& path, const char* mode) { return _wfopen(to_w(path).c_str(), to_w(mode).c_str()); }

static std::string exe_dir() {
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size());
    p.resize(n);
    std::string s = to_u8(p);
    return s.substr(0, s.find_last_of("\\/"));
}
#else
static std::string env_u8(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}
static FILE* fopen_u8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
static std::string exe_dir() {
    std::vector<char> path(4096);
    for (;;) {
        const auto count = readlink("/proc/self/exe", path.data(), path.size());
        if (count < 0) { std::perror("/proc/self/exe"); host::process_exit(1); }
        if (static_cast<std::size_t>(count) < path.size())
            return std::string(path.data(), static_cast<std::size_t>(count)).substr(0, std::string(path.data(), count).find_last_of('/'));
        path.resize(path.size() * 2);
    }
}
#endif

// Functions with an unresolved indirect branch accept entry at any instruction (see gen.py):
// the dispatcher sets c->pc = target | 1 and the function's prologue jumps to that label.
static bool try_resume(Ctx* c) {
    u64 pc = c->pc;
    auto it = std::upper_bound(g_starts.begin(), g_starts.end(), pc);
    if (it == g_starts.begin()) return false;
    u64 start = *(it - 1);
    if (!std::binary_search(hw_resumable, hw_resumable + hw_resumable_count, start)) return false;
    HwFn f = hw_lookup(start);
    if (!f) return false;
    static std::atomic<int> logged{0};
    if (logged++ < 20) hw_log("dispatch: resuming 0x%llx inside f_%llx", (unsigned long long)pc, (unsigned long long)start);
    c->pc = pc | 1;
    f(c);
    return true;
}

static void unknown_target(Ctx* c) {
    if (try_resume(c)) return;
    hw_log("FATAL: jump to unrecompiled guest address 0x%llx (lr=0x%llx)", (unsigned long long)c->pc,
           (unsigned long long)c->x[30]);
    for (auto& t : g_tables) {
        if (c->pc >= t.lo && c->pc < t.hi) {
            // Record it so the next generator run adds it as an entry point.
            if (FILE* f = fopen_u8(exe_dir() + "/extra_entries.txt", "a")) {
                fprintf(f, "0x%llx\n", (unsigned long long)c->pc);
                fclose(f);
            }
            hw_log("recorded 0x%llx in extra_entries.txt - regenerate to include it", (unsigned long long)c->pc);
        }
    }
    hw_fatal("unrecompiled target");
}

extern "C" void hw_call(Ctx* c, u64 addr) {
    HwFn f = hw_lookup(addr);
    if (!f) {
        c->pc = addr;
        unknown_target(c);
        return;
    }
    f(c);
}

extern "C" void hw_dispatch(Ctx* c) {
    HwFn f = hw_lookup(c->pc);
    if (!f) {
        unknown_target(c);
        return;
    }
    HW_MUSTTAIL return f(c);
}

// ------------------------------------------------------------------ misc runtime hooks
extern "C" u64 hw_cntvct(void) {
    static host::Counter freq = [] {
        host::Counter f;
        host::counter_frequency(&f);
        return f;
    }();
    host::Counter t;
    host::counter(&t);
    return (u64)((double)t.QuadPart * (19200000.0 / (double)freq.QuadPart));
}
extern "C" void hw_yield(void) { host::thread_yield(); }
extern "C" void hw_brk(Ctx* c, u64 pc, u32 imm) {
    hw_log("guest brk #0x%x at 0x%llx (x0=0x%llx)", imm, (unsigned long long)pc, (unsigned long long)c->x[0]);
    hw_fatal("guest abort");
}
extern "C" void hw_unimpl(Ctx* c, u64 pc, u32 word) {
    hw_log("unimplemented instruction %08x at 0x%llx", word, (unsigned long long)pc);
    hw_fatal("unimplemented instruction");
}
extern "C" void hw_sleep_ns(s64 ns) {
    if (ns <= 0) {
        host::thread_yield();
        return;
    }
    // Sleeps of a millisecond or more end on time: the game's frame limiter sleeps ~15 ms on a job worker every
    // frame, and the high-resolution timer woke it ~0.3 ms late, so a busy battle ran at 17.0 ms instead of 16.7.
    // The timer covers all but the last millisecond, the rest is a yielding spin to the deadline.
    constexpr s64 kSpinNs = 1000000;
    const u64 start = host::monotonic_ns();
    const bool precise = ns >= kSpinNs;
    const s64 timer_ns = precise ? ns - kSpinNs : ns;
    if (timer_ns > 0) host::sleep_ns(timer_ns);
    if (precise) {
        const u64 end = start + (u64)ns;
        while (host::monotonic_ns() < end) host::thread_yield();
    }
}

extern "C" void hw_exit_process(int code) {
    if (code == 0) display::restore_desktop_mode();
    host::process_exit(code);
}

// ------------------------------------------------------------------ logging / crashes
static FILE* g_log;
static std::mutex g_log_lock;

extern "C" void hw_log(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static const u64 t0 = host::ticks_ms();
    unsigned ms = (unsigned)(host::ticks_ms() - t0);
    std::lock_guard lock(g_log_lock);
    fprintf(stderr, "[hwder] %s\n", buf);
    if (g_log) {
        fprintf(g_log, "[%3u.%03u] %s\n", ms / 1000, ms % 1000, buf);
        fflush(g_log);
    }
}

extern "C" void hw_fatal(const char* why) {
    hw_log("fatal: %s", why);
#ifdef _WIN32
    if (IsDebuggerPresent()) __debugbreak();
#endif
    if (debug::flag(debug::Hold)) {  // keep the window up showing the reason
        display::set_status(std::string("halted: ") + why);
        for (;;) host::sleep_ms(1000);
    }
    host::process_exit(1);
}

// Minidumps go beside the log as hwder-crash-YYYYMMDD-HHMMSS.dmp (empty dir: disabled, HWDER_NO_DUMP=1).
// They stay on this machine; nothing is uploaded.
#ifdef _WIN32
static wchar_t g_dump_dir[MAX_PATH];
static std::atomic<DWORD> g_dump_owner{0};   // thread that started the one dump of this process
static std::atomic<DWORD> g_dump_worker{0};  // thread running MiniDumpWriteDump

struct DumpJob {
    DWORD tid;
    EXCEPTION_POINTERS* ep;
    HANDLE file;
    BOOL ok;
    DWORD err;
};

static DWORD WINAPI dump_thread(void* p) {
    auto* job = (DumpJob*)p;
    g_dump_worker = GetCurrentThreadId();
    MINIDUMP_EXCEPTION_INFORMATION mei = {job->tid, job->ep, FALSE};
    // Never full memory: the guest address space is huge.
    auto type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                MiniDumpWithHandleData | MiniDumpWithProcessThreadData);
    job->ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), job->file, type, job->ep ? &mei : nullptr, nullptr, nullptr);
    if (!job->ok) job->err = GetLastError();
    return 0;
}

// Written from a fresh thread so a stack overflow on the faulting thread doesn't matter. Only the
// first caller dumps; another thread faulting meanwhile parks here until the first one exits the process.
static void write_crash_dump(EXCEPTION_POINTERS* ep) {
    if (!g_dump_dir[0]) return;
    DWORD self = GetCurrentThreadId(), none = 0;
    if (!g_dump_owner.compare_exchange_strong(none, self)) {
        if (none != self)
            for (;;) host::sleep_ms(INFINITE);
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH + 40];
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%s\\hwder-crash-%04u%02u%02u-%02u%02u%02u.dmp", g_dump_dir, st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DumpJob job = {self, ep, CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr), FALSE, 0};
    if (job.file == INVALID_HANDLE_VALUE) {
        hw_log("crash dump failed: %lu", GetLastError());
        return;
    }
    HANDLE t = CreateThread(nullptr, 0, dump_thread, &job, 0, nullptr);
    if (!t) {
        job.err = GetLastError();
    } else {
        if (WaitForSingleObject(t, 30000) != WAIT_OBJECT_0) {
            hw_log("crash dump failed: %lu (timed out)", (unsigned long)WAIT_TIMEOUT);
            return;  // the worker still owns the file; the process is about to exit anyway
        }
        CloseHandle(t);
    }
    CloseHandle(job.file);
    if (job.ok) {
        hw_log("crash dump written: %ls", path);
    } else {
        hw_log("crash dump failed: %lu", job.err);
        DeleteFileW(path);
    }
}

// Keep only the newest few dumps (names sort by time).
static void prune_crash_dumps(const wchar_t* dir, size_t keep) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(dir) + L"\\hwder-crash-*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i + keep < names.size(); i++) DeleteFileW((std::wstring(dir) + L"\\" + names[i]).c_str());
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && er->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        er->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && er->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (GetCurrentThreadId() == g_dump_worker) return EXCEPTION_CONTINUE_SEARCH;  // dbghelp handles its own faults
    HANDLE proc = GetCurrentProcess();
    char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = (SYMBOL_INFO*)symbuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    const char* name = SymFromAddr(proc, (DWORD64)er->ExceptionAddress, &disp, sym) ? sym->Name : "?";
    auto modname = [](const void* pc, char* out, size_t n) {
        HMODULE mod = nullptr;
        strcpy_s(out, n, "?");
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod))
            GetModuleBaseNameA(GetCurrentProcess(), mod, out, (DWORD)n);
    };
    char mn[MAX_PATH];
    modname(er->ExceptionAddress, mn, sizeof(mn));
    PWSTR tdesc = nullptr;
    GetThreadDescription(GetCurrentThread(), &tdesc);
    hw_log("CRASH code=0x%08lx at %p (%s!%s+0x%llx) access=%s 0x%llx (thread %lu %ls)", er->ExceptionCode,
           er->ExceptionAddress, mn, name, (unsigned long long)disp,
           er->NumberParameters > 0 && er->ExceptionInformation[0] ? "write" : "read",
           er->NumberParameters > 1 ? (unsigned long long)er->ExceptionInformation[1] : 0ull, GetCurrentThreadId(), tdesc ? tdesc : L"");
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
            break;
        disp = 0;
        std::string n = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym) ? pretty_symbol(sym->Name) : "?";
        char fm[MAX_PATH];
        modname((const void*)sf.AddrPC.Offset, fm, sizeof(fm));
        hw_log("  #%02d %s!%s+0x%llx", i, fm, n.c_str(), (unsigned long long)disp);
    }
    write_crash_dump(ep);
    ExitProcess(2);
}

// Log the calling thread's host stack (guest functions appear as f_<addr>).
extern "C" void hw_log_backtrace(const char* tag) {
    void* pcs[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, pcs, nullptr);
    HANDLE proc = GetCurrentProcess();
    char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = (SYMBOL_INFO*)symbuf;
    std::string line;
    for (USHORT i = 0; i < n; i++) {
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char* name = SymFromAddr(proc, (DWORD64)pcs[i], &disp, sym) ? sym->Name : "?";
        if (strncmp(name, "f_", 2) != 0) continue;  // guest frames only
        line += " ";
        line += pretty_symbol(name);
    }
    hw_log("%s backtrace:%s", tag, line.c_str());
}

// ------------------------------------------------------------------ thread dump (HWDER_THREAD_DUMP=<seconds>)
// Periodically logs every guest thread's host call stack (guest functions appear as f_<addr>).
static void dump_threads() {
    HANDLE proc = GetCurrentProcess();
    for (auto& t : kern::all_threads()) {
        if (!t->started || t->exited || !t->host_handle) continue;
        u64 pcs[24];
        int n = 0;
        if (SuspendThread(t->host_handle) == (DWORD)-1) continue;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t->host_handle, &ctx)) {
            // RtlVirtualUnwind does not allocate, so it is safe while the target is suspended.
            for (; n < 24 && ctx.Rip; n++) {
                pcs[n] = ctx.Rip;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!rf) {
                    ctx.Rip = *(DWORD64*)ctx.Rsp;
                    ctx.Rsp += 8;
                    continue;
                }
                void* hd = nullptr;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &ef, nullptr);
            }
        }
        ResumeThread(t->host_handle);
        std::string line;
        char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = (SYMBOL_INFO*)symbuf;
        for (int i = 0; i < n; i++) {
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            char b[300];
            snprintf(b, sizeof(b), " %s+0x%llx", SymFromAddr(proc, pcs[i], &disp, sym) ? pretty_symbol(sym->Name).c_str() : "?",
                     (unsigned long long)disp);
            line += b;
            if (i < n - 1) line += " <";
        }
        hw_log("thread %llu '%s' prio %d%s:%s", (unsigned long long)t->id, t->name.c_str(), t->priority,
               t->waiting ? " [waiting]" : "", line.c_str());
    }
}
static void start_thread_dumper() {
    const char* e = debug::str(debug::ThreadDump);
    if (!e || !atoi(e)) return;
    static int period = atoi(e);
    CreateThread(nullptr, 0, [](void*) -> DWORD {
        for (;;) {
            host::sleep_ms(period * 1000);
            hw_log("---- thread dump ----");
            dump_threads();
        }
    }, nullptr, 0, nullptr);
}

#else
extern "C" void hw_log_backtrace(const char* tag) {
    hw_log("backtrace: %s", tag);
    void* frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
}
static void start_thread_dumper() {
    const char* setting = std::getenv("HWDER_THREAD_DUMP");
    const int seconds = setting ? std::atoi(setting) : 0;
    if (seconds <= 0) return;
    std::thread([seconds] {
        for (;;) {
            host::sleep_ms(static_cast<unsigned>(seconds) * 1000);
            hw_log("---- thread state (Linux: running register capture unavailable) ----");
            for (const auto& thread : kern::all_threads())
                hw_log("thread %llu entry %llx started %d exited %d", (unsigned long long)thread->id,
                       (unsigned long long)thread->entry, (int)thread->started.load(), (int)thread->exited.load());
        }
    }).detach();
}
static int g_crash_fd = -1;  // the log's descriptor, for the signal handler

// Async-signal-safe: hexadecimal into `out`, returns the length.
static int crash_hex(char* out, unsigned long long v) {
    char tmp[16];
    int n = 0;
    do tmp[n++] = "0123456789abcdef"[v & 15]; while ((v >>= 4) && n < 16);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static void crash_write(const char* text, size_t size) {
    ssize_t ignored = write(STDERR_FILENO, text, size);
    if (g_crash_fd >= 0) ignored = write(g_crash_fd, text, size);
    (void)ignored;
}

static void install_linux_crash_handlers() {
    std::set_terminate([] {
        hw_log_backtrace("std::terminate");
        std::_Exit(3);
    });
    // The crash report goes into hwder.log (and stderr): signal, faulting address, host thread and a backtrace,
    // whose names include the recompiled guest functions (f_<address>, the executable exports its symbols).
    // Only async-signal-safe calls; backtrace() is called once here so its library is loaded before a crash.
    // The default handler then produces the distribution's ordinary core dump, subject to its policy.
    if (g_log) {
        fflush(g_log);
        g_crash_fd = fileno(g_log);
    }
    void* warm[1];
    backtrace(warm, 1);
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND | SA_SIGINFO;
    action.sa_sigaction = [](int signal, siginfo_t* info, void*) {
        char line[160];
        int n = 0;
        auto put = [&](const char* s) { while (*s && n < (int)sizeof(line) - 1) line[n++] = *s++; };
        put("\n[hwder] CRASH: signal ");
        if (signal >= 10) line[n++] = (char)('0' + signal / 10 % 10);
        line[n++] = (char)('0' + signal % 10);
        put(" at address 0x");
        n += crash_hex(line + n, (unsigned long long)(uintptr_t)(info ? info->si_addr : nullptr));
        put(", host thread ");
        n += crash_hex(line + n, (unsigned long long)syscall(SYS_gettid));
        put(" (hex); backtrace:\n");
        crash_write(line, (size_t)n);
        void* frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        if (g_crash_fd >= 0) backtrace_symbols_fd(frames, count, g_crash_fd);
        constexpr char tail[] = "[hwder] end of crash report; the system's core dump policy applies\n";
        crash_write(tail, sizeof(tail) - 1);
        raise(signal);
    };
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(signal, &action, nullptr);
}
#endif

// ------------------------------------------------------------------ NSO loading
static bool lz4_decompress(const u8* src, size_t src_size, u8* dst, size_t dst_size) {
    const u8* ip = src;
    const u8* iend = src + src_size;
    u8* op = dst;
    u8* oend = dst + dst_size;
    while (ip < iend) {
        u8 token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            u8 b;
            do { b = *ip++; lit += b; } while (b == 255);
        }
        if (op + lit > oend || ip + lit > iend) return false;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= iend) break;
        size_t off = ip[0] | (ip[1] << 8);
        ip += 2;
        size_t len = token & 15;
        if (len == 15) {
            u8 b;
            do { b = *ip++; len += b; } while (b == 255);
        }
        len += 4;
        u8* match = op - off;
        if (match < dst || op + len > oend) return false;
        for (size_t i = 0; i < len; i++) op[i] = match[i];  // overlapping copy
        op += len;
    }
    return op == oend;
}

static std::vector<u8> read_file(const std::string& path) {
    std::vector<u8> d;
    FILE* f = fopen_u8(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    return d;
}

// ExeFS file: from the NSP when one is loaded, else from the extracted exefs directory.
static std::vector<u8> read_exefs(const std::string& exefs, const char* name) {
    std::vector<u8> d;
    if (hwfs::nsp::active()) {
        hwfs::nsp::read_exefs(name, d);
        return d;
    }
    return read_file(exefs + "/" + name);
}

static void load_module(const HwModuleInfo& mi, const std::string& exefs) {
    std::vector<u8> nso = read_exefs(exefs, mi.name);
    if (nso.size() < 0x100 || memcmp(nso.data(), "NSO0", 4) != 0) {
        hw_log("loader: cannot read %s from %s", mi.name, hwfs::nsp::active() ? hwfs::nsp::path().c_str() : exefs.c_str());
        hw_fatal("missing ExeFS module");
    }
    if (mi.build_id) {
        char id[33];
        for (int i = 0; i < 16; i++) snprintf(id + 2 * i, 3, "%02x", nso[0x40 + i]);
        if (strcmp(id, mi.build_id) != 0) {
            hw_log("loader: %s has build id %s, but this build was made for %s", mi.name, id, mi.build_id);
#ifdef _WIN32
            if (!debug::flag(debug::Mute))
                MessageBoxW(nullptr,
                            L"This HWDER build was prepared for a different version of the game than the files it "
                            L"was given (for example with or without the game update).\n\n"
                            L"Open HWDER Setup and choose Prepare again with the files you want to play.",
                            L"HWDER", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#else
            if (!debug::flag(debug::Mute))
                display::show_message("This HWDER build was prepared for a different version of the game than the files it "
                                      "was given (for example with or without the game update).\n\n"
                                      "Run play-local.sh again with the files you want to play.");
#endif
            hw_fatal("game module does not match the generated code");
        }
    }
    u32 flags = *(u32*)&nso[0xC];
    struct Seg { u32 file_off, mem_off, size, extra; } seg[3];
    memcpy(seg, &nso[0x10], sizeof(seg));
    u32 csize[3];
    memcpy(csize, &nso[0x60], sizeof(csize));
    u64 bss = seg[2].extra;
    u64 image = (seg[2].mem_off + seg[2].size + bss + 0xFFF) & ~0xFFFull;
    kern::mem::map(mi.base, image, kern::mem::CodeData, kern::mem::RW);
    for (int i = 0; i < 3; i++) {
        u8* dst = (u8*)(mi.base + seg[i].mem_off);
        if (flags & (1u << i)) {
            if (!lz4_decompress(&nso[seg[i].file_off], csize[i], dst, seg[i].size)) hw_fatal("lz4");
        } else {
            memcpy(dst, &nso[seg[i].file_off], seg[i].size);
        }
    }
    // Experiment (HWDER_PATCH_DT=<hz>): the game's per-frame time step lives in two rodata floats
    // (1/60 s, plus 1/30 s for its half-rate mode). Rewriting them to 1/hz together with releasing
    // frames at hz tells us whether the simulation is dt-driven (then high refresh is feasible).
    if (!strcmp(mi.name, "main")) {
        if (const char* hz = debug::str(debug::PatchDt)) {
            float f = (float)atof(hz);
            if (f >= 30.0f) {
                float dt = 1.0f / f, dt2 = 2.0f / f;
                memcpy((void*)HW_ADDR_FrameDelta60a, &dt, 4);
                memcpy((void*)HW_ADDR_FrameDelta60b, &dt, 4);
                memcpy((void*)HW_ADDR_FrameDelta30, &dt2, 4);
                hw_log("loader: patched frame delta constants to 1/%.0f s", f);
            }
        }
    }
    u64 text_end = (seg[0].mem_off + seg[0].size + 0xFFF) & ~0xFFFull;
    u64 ro_end = (seg[1].mem_off + seg[1].size + 0xFFF) & ~0xFFFull;
    kern::mem::map(mi.base, text_end, kern::mem::Code, kern::mem::RX, false);
    kern::mem::map(mi.base + seg[1].mem_off, ro_end - seg[1].mem_off, kern::mem::Code, kern::mem::R, false);
    kern::mem::map(mi.base + seg[2].mem_off, image - seg[2].mem_off, kern::mem::CodeData, kern::mem::RW, false);
    hw_log("loader: %-8s at 0x%llx (0x%llx bytes)", mi.name, (unsigned long long)mi.base, (unsigned long long)image);
}

static void load_npdm(const std::string& exefs) {
    std::vector<u8> d = read_exefs(exefs, "main.npdm");
    if (d.size() < 0x80 || memcmp(d.data(), "META", 4) != 0) return;
    kern::g_process.main_thread_priority = d[0xE];
    kern::g_process.main_thread_core = d[0xF];
    kern::g_process.main_thread_stack = *(u32*)&d[0x1C];
    hw_log("npdm: main thread prio %u core %u stack 0x%x", kern::g_process.main_thread_priority,
           kern::g_process.main_thread_core, kern::g_process.main_thread_stack);
}

// ------------------------------------------------------------------ boot
void hw_config_load(const std::string& dir);

// Hybrid CPUs (Intel P/E cores): keep the game's threads on the performance cores. In a big battle the GPU command
// thread, the presenter and their helpers landing on efficiency cores cost about 10% of the 120 fps mode's frame rate
// (stage-4 replay: 9.0 -> 7.6 ms per frame on the GPU thread). HWDER_ALL_CORES=1 keeps every core.
// Windows only: Linux leaves placement to the scheduler (the Ally's Z1 Extreme has no efficiency cores).
#ifdef _WIN32
static void prefer_performance_cores() {
    if (debug::flag(debug::AllCores)) return;
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (!len) return;
    std::vector<char> buf(len);
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0)) return;
    BYTE lo = 255, hi = 0;
    for (ULONG o = 0; o < len;) {
        auto* s = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + o);
        if (s->Type == CpuSetInformation) lo = std::min(lo, s->CpuSet.EfficiencyClass), hi = std::max(hi, s->CpuSet.EfficiencyClass);
        o += s->Size;
    }
    if (lo >= hi) return;  // not a hybrid CPU
    std::vector<ULONG> ids;
    for (ULONG o = 0; o < len;) {
        auto* s = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + o);
        if (s->Type == CpuSetInformation && s->CpuSet.EfficiencyClass == hi) ids.push_back(s->CpuSet.Id);
        o += s->Size;
    }
    if (ids.size() < 4) return;  // too few performance cores to give up the others
    if (SetProcessDefaultCpuSets(GetCurrentProcess(), ids.data(), (ULONG)ids.size()))
        hw_log("cpu: hybrid CPU, preferring its %zu performance logical processors (HWDER_ALL_CORES=1 to use all)", ids.size());
}
#endif

static int runtime_main(int argc, char** argv) {
    std::string dir = exe_dir();
#ifdef _WIN32
    SetCurrentDirectoryW(to_w(dir).c_str());
#else
    if (chdir(dir.c_str()) != 0) { std::perror("chdir executable directory"); return 1; }
#endif  // caches, settings and saves live beside the exe
    std::string log_path = env_u8("HWDER_LOG");
    if (log_path.empty()) log_path = dir + "/hwder.log";
    g_log = fopen_u8(log_path, "w");
    hw_log("HWDER %s (commit %s, built %s)", hw_version(), hw_commit(), hw_build_date());
#ifdef _WIN32
    prefer_performance_cores();
    std::string no_dump = env_u8("HWDER_NO_DUMP");
    if (no_dump.empty() || no_dump == "0") {
        wchar_t* file = nullptr;
        if (GetFullPathNameW(to_w(log_path).c_str(), MAX_PATH, g_dump_dir, &file) && file) {
            file[-1] = 0;  // strip "\hwder.log"
            prune_crash_dumps(g_dump_dir, 5);
        } else {
            g_dump_dir[0] = 0;
        }
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, crash_handler);
    // Deaths that bypass SEH: C++ terminate (uncaught exception, e.g. in a worker thread), abort(),
    // CRT invalid-parameter. Log them with the thread id instead of dying silently.
    std::set_terminate([] {
        hw_log("TERMINATE: std::terminate called (uncaught exception?) on thread %lu", GetCurrentThreadId());
        write_crash_dump(nullptr);
        ExitProcess(3);
    });
    signal(SIGABRT, [](int) {
        PWSTR desc = nullptr;
        GetThreadDescription(GetCurrentThread(), &desc);
        hw_log("ABORT: abort() called on thread %lu (%ls)", GetCurrentThreadId(), desc ? desc : L"?");
        void* pcs[48];
        USHORT n = RtlCaptureStackBackTrace(0, 48, pcs, nullptr);
        char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = (SYMBOL_INFO*)symbuf;
        for (USHORT i = 0; i < n; i++) {
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            const char* name = SymFromAddr(GetCurrentProcess(), (DWORD64)pcs[i], &disp, sym) ? sym->Name : "?";
            HMODULE mod = nullptr;
            char modname[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pcs[i], &mod))
                GetModuleBaseNameA(GetCurrentProcess(), mod, modname, sizeof(modname));
            hw_log("  #%02u %s!%s+0x%llx", i, modname, name, (unsigned long long)disp);
        }
        write_crash_dump(nullptr);
        ExitProcess(3);
    });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        hw_log("CRT: invalid parameter on thread %lu", GetCurrentThreadId());
    });
#else
    install_linux_crash_handlers();
#endif
    if (env_u8("HWDER_TEST_CRASH") == "1") {  // exercises the crash log + minidump path without game data
        hw_log("HWDER_TEST_CRASH=1: crashing on purpose");
        volatile int* p = nullptr;
        *p = 1;
    }
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    hw_config_load(dir);

    std::string exefs = env_u8("HWDER_EXEFS");
    if (exefs.empty()) exefs = dir + "/exefs";
    // Shift held at launch shows the startup screen even when it is set to be skipped.
#ifdef _WIN32
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    const bool shift = false;  // HWDER_PRELAUNCH=1 forces startup before a Linux window exists.
#endif
    bool prelaunch = false;
    {
        // Game data: an NSP (decrypted in-process with the user's prod.keys) or an extracted exefs/ +
        // romfs/ pair. Nothing is written out either way. Files picked on the startup screen are in the settings.
        settings::load();
        hwfs::run_pending_save_copy();  // a save folder change made in the menu last session
        hwfs::nsp::set_paths(settings::launch().nsp_path, settings::launch().keys_path);
        hwfs::nsp::set_update(settings::launch().update_path);
        std::string err;
        bool nsp = hwfs::nsp::init(dir, err);
        std::error_code path_error;
        bool have_dir = std::filesystem::is_regular_file(std::filesystem::u8path(exefs) / "main", path_error);
        // The startup screen: always when the game file or keys are missing or fail their checks, else unless
        // skipped in the settings. Never in directory (dev) mode or in automated runs (HWDER_NO_PRELAUNCH, or
        // HWDER_MUTE with HWDER_OFFSCREEN); HWDER_PRELAUNCH=1 forces it for the latter.
        const bool dev = !env_u8("HWDER_EXEFS").empty() || !env_u8("HWDER_ROMFS").empty() || (!nsp && have_dir);
        const bool automated = debug::flag(debug::Mute) && debug::flag(debug::Offscreen) && env_u8("HWDER_PRELAUNCH") != "1";
        if (!dev && !debug::flag(debug::NoPrelaunch) && !automated)
            prelaunch = !nsp || !settings::launch().skip_prelaunch || shift || env_u8("HWDER_PRELAUNCH") == "1";
        if (!nsp && !have_dir && !prelaunch) {
            std::string msg = err.empty() ? "No game data found.\n\nPut your own dump of Hyrule Warriors: Definitive Edition "
                                            "(the .nsp) and your prod.keys next to hwder.exe.\n\n(Looked for *.nsp in: " + dir + ")"
                                          : "Cannot load the game from the NSP:\n\n" + err;
            hw_log("boot: %s", msg.c_str());
#ifdef _WIN32
            if (!debug::flag(debug::Mute)) MessageBoxW(nullptr, to_w(msg).c_str(), L"HWDER", MB_ICONERROR | MB_OK);
#else
            if (!debug::flag(debug::Mute)) display::show_message(msg);
#endif
            return 2;
        }
        if (!prelaunch) hw_log("boot: game data from %s", nsp ? hwfs::nsp::path().c_str() : exefs.c_str());
    }
    kern::mem::init();
    build_dispatch();
    display::init();
    gpu::start_pipeline_prewarm();
    if (prelaunch) {
        if (!prelaunch::run(dir)) {
            hw_log("boot: quit from the startup screen");
            hw_exit_process(0);
        }
        hw_log("boot: game data from %s", hwfs::nsp::path().c_str());
    }
    gpu::prepare_pipeline_catalog();
    display::start();
    ipc::register_all_services();
    load_npdm(exefs);
    for (unsigned i = 0; i < hw_module_count; i++) load_module(hw_modules[i], exefs);

    // Main thread: stack in the stack region, entry at rtld (the first module), x1 = own handle.
    u64 stack_size = (kern::g_process.main_thread_stack + 0xFFF) & ~0xFFFull;
    u64 stack_base = kern::mem::kStackBase;
    kern::mem::map(stack_base, stack_size, kern::mem::Stack, kern::mem::RW);
    auto t = kern::create_thread(hw_modules[0].text_lo, 0, stack_base + stack_size,
                                 (s32)kern::g_process.main_thread_priority, (s32)kern::g_process.main_thread_core);
    t->handle = kern::g_handles.add(t);
    t->ctx->x[1] = t->handle;
    t->name = "MainThread";
    hw_log("boot: main thread entry 0x%llx, stack 0x%llx+0x%llx", (unsigned long long)t->entry,
           (unsigned long long)stack_base, (unsigned long long)stack_size);
    kern::start_thread(t.get());
    start_thread_dumper();
    perf::start();
    host::thread_join_release(t->host_handle);
    t->host_handle = nullptr;
    hw_log("main thread exited");
    hw_exit_process(0);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    return runtime_main(argc, argv);
#else
    return display::run_main(argc, argv, runtime_main);
#endif
}
