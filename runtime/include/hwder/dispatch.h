// HWDER - interface between recompiled code and the runtime.
#pragma once
#include "hwder/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*HwFn)(Ctx* c);

typedef struct HwFuncEntry {
    u64 addr;
    HwFn fn;
} HwFuncEntry;

extern const HwFuncEntry hw_func_table[];
extern const unsigned hw_func_count;

// Named guest symbols (tools/recomp/symbols.txt), sorted by address; emitted into functable.c.
typedef struct HwSymbol {
    u64 addr;
    const char* name;
} HwSymbol;
extern const HwSymbol hw_symbols[];
extern const unsigned hw_symbol_count;
// Name of a guest function start, or NULL (runtime/src/core.cpp).
const char* hw_symbol_name(u64 addr);

// Nonzero: the SDK's memcpy/memset/... run their translated bodies instead of host calls (gen_main.py
// LIBC_INTRINSICS; HWDER_GUEST_LIBC). Set once at startup.
extern int hw_guest_libc;

// Resolve a guest code address to its recompiled host function (NULL if none).
HwFn hw_lookup(u64 addr);
// Indirect call: BLR. Return address is already in x30.
void hw_call(Ctx* c, u64 addr);
// Indirect jump: BR / RET Xn. Target in c->pc.
void hw_dispatch(Ctx* c);

#ifdef __cplusplus
}
#endif
