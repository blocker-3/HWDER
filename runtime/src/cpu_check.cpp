// The game and runtime are compiled for AVX2 (HW_ARCH_FLAGS). On a CPU without it hwder.exe would die
// with an illegal-instruction crash and no message, before main. This file is built without those flags
// (hwder_cpu_check in CMakeLists.txt) and runs from the compiler init segment, ahead of every other
// static initializer, so the check itself never executes an AVX2 instruction.
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>

static bool has_avx2() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool osxsave = r[2] & (1 << 27), avx = r[2] & (1 << 28), fma = r[2] & (1 << 12), f16c = r[2] & (1 << 29);
    if (!osxsave || !avx || !fma || !f16c) return false;
    if ((_xgetbv(0) & 6) != 6) return false;  // the OS must save the YMM registers
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

#pragma warning(disable : 4075)
#pragma init_seg(compiler)
static int g_cpu_checked = check_cpu();
#endif
