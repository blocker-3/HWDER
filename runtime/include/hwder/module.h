// Layout of the recompiled modules, emitted by tools/recomp/gen_main.py (module_info.c).
#pragma once
#include "hwder/cpu.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct HwModuleInfo {
    const char* name;  // ExeFS file name (rtld, main, subsdk0, subsdk1, sdk)
    u64 base, image_size, text_lo, text_hi;
    // First 16 bytes of the NSO's build id, as hex, from the module the code was generated from (null in a
    // generated dir from before this field). The loader refuses a different module: the code would not match.
    const char* build_id;
} HwModuleInfo;
// Generated module_info.c fills build_id only when this is defined, so a generated dir also builds with a
// checkout whose HwModuleInfo predates the field (worktrees share ../HWDER/generated).
#define HW_MODULE_HAS_BUILD_ID 1
extern const HwModuleInfo hw_modules[];
extern const unsigned hw_module_count;
#ifdef __cplusplus
}
#endif
