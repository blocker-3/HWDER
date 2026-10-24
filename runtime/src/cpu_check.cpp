// The game and runtime are compiled for AVX2 (HW_ARCH_FLAGS). On a CPU without it hwder.exe would die
// with an illegal-instruction crash and no message, before main. This file is built without those flags
// (hwder_cpu_check in CMakeLists.txt) and runs ahead of every other static initializer (MSVC/clang-cl:
// the compiler init segment; MinGW, the player's Setup toolchain: a priority-101 constructor), so the check
// itself never executes an AVX2 instruction.
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>

// XCR0. _xgetbv needs the xsave target feature under MinGW clang, so read it with the instruction there.
static unsigned long long xcr0() {
#ifdef _MSC_VER
    return _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((unsigned long long)hi << 32) | lo;
#endif
}

static bool has_avx2() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool osxsave = r[2] & (1 << 27), avx = r[2] & (1 << 28), fma = r[2] & (1 << 12), f16c = r[2] & (1 << 29);
    if (!osxsave || !avx || !fma || !f16c) return false;
    if ((xcr0() & 6) != 6) return false;  // the OS must save the YMM registers
    __cpuidex(r, 7, 0);
    return r[1] & (1 << 5);
}

static int check_cpu() {
    if (!has_avx2()) {
        MessageBoxW(nullptr,
                    L"HWDER cannot run on this PC because its processor lacks AVX2.\n\n"
                    L"Most Intel processors from 2013 (Haswell) on and AMD processors from 2015 (Excavator, Ryzen) on have it.",
                    L"HWDER", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        ExitProcess(1);
    }
    return 0;
}

#ifdef _MSC_VER
#pragma warning(disable : 4075)
#pragma init_seg(compiler)
static int g_cpu_checked = check_cpu();
#else
__attribute__((constructor(101))) static void cpu_check_first() { check_cpu(); }
#endif
#endif
