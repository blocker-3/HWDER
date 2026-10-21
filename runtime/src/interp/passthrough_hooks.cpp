// Pass-through bodies for the guest functions tools/recomp/hooks.txt lists from the stage a/b frame
// probe (docs/frame-loop.md). The generated code calls f_<addr> for them, so each must exist; the
// probe's logging is gone and they simply run the original. The Draw stage (0x10002acb54) and
// BeginFrame (0x100051e9c4) hooks live in interp_draw.cpp.
#include "hwder/runtime.h"

#define PASSTHROUGH(addr)                    \
    extern "C" void f_##addr##_orig(Ctx* c); \
    extern "C" void f_##addr(Ctx* c) { f_##addr##_orig(c); }

// scene class A (vtable 0x1000ba6008): slot 3 publishes the render cameras, slot 5 draws
PASSTHROUGH(10001766a4)
PASSTHROUGH(10001767e8)
PASSTHROUGH(100017672c)
PASSTHROUGH(1000176490)
PASSTHROUGH(10001763bc)
// scene class B (vtable 0x1000bd36f8)
PASSTHROUGH(10002d96b4)
PASSTHROUGH(10002d9878)
// camera constants upload (render side)
PASSTHROUGH(10003d0a74)
// MainThread's frame loop f_10002ad35c: the calls after the Draw stage
PASSTHROUGH(100045de0c)
PASSTHROUGH(10003d2da4)
PASSTHROUGH(10003d7028)
PASSTHROUGH(100052f6bc)
PASSTHROUGH(100051ea20)
PASSTHROUGH(10001dee94)
PASSTHROUGH(100052ab10)
PASSTHROUGH(10003878d0)
