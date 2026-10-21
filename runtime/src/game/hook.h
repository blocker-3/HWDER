// Overriding and calling the game's functions by name.
//
// Names come from tools/recomp/symbols.txt; gen_main.py turns them into generated/symbols.h, which maps
// f_<Name> to the recompiled f_<address> (and f_<Name>_orig to the generated body of a hooked function)
// and defines HW_ADDR_<Name>. A hook is a guest function listed with `hook` in symbols.txt: the runtime
// must define f_<Name> (direct calls, tail calls and the dispatch table all reach it) and may call
// f_<Name>_orig for the game's own body.
//
//   HW_HOOK(Camera_BuildProj) {          // defines the override
//       ...
//       HW_ORIG(Camera_BuildProj)(c);    // runs the original
//   }
//
//   HW_GUEST(Scene_Draw);                // declares a game function the runtime calls directly
//   game::call(c, f_Scene_Draw, ret);    // calls it as generated code would (hwder/dispatch.h for hw_call)
//
// Every hook must leave Ctx exactly as the original would, and the generated code's ABI applies: the return
// address is in x30 and a callee that unwinds past it leaves x30 changed (see game::call).
#pragma once
#include "hwder/cpu.h"
#include "hwder/dispatch.h"
#include "symbols.h"

#define HW_GUEST(name) extern "C" void f_##name(Ctx* c)
#define HW_HOOK(name)                        \
    extern "C" void f_##name##_orig(Ctx* c); \
    extern "C" void f_##name(Ctx* c)
#define HW_ORIG(name) f_##name##_orig
#define HW_ADDR(name) HW_ADDR_##name

namespace game {

// Call a guest function the way the generated code does from return address `ret`. Returns false when the
// callee unwound past it (a guest exception or longjmp), which the caller must treat as "stop".
inline bool call(Ctx* c, void (*fn)(Ctx*), u64 ret) {
    c->x[30] = ret;
    fn(c);
    return c->x[30] == ret;
}

// Same for a target known only by address (a virtual call).
inline bool call_addr(Ctx* c, u64 target, u64 ret) {
    c->x[30] = ret;
    hw_call(c, target);
    return c->x[30] == ret;
}

}  // namespace game
