// Keep small stage props drawn in crowded scenes ("Keep props in big battles").
//
// Every frame StageLod_Update (x0 = the stage renderer) rewrites the per-layer draw distances that the stage
// part walker compares each part's camera distance against (owner + 220 + 4 * layer). In the docked
// performance configuration it also applies a crowd rule: with n = *(*(*StageLod_CrowdOwner) + 88) (doubled
// when StageLod_ScaleFlag() is set) above 60, it writes layer 1's distance as 0. Layer 1 holds the small
// props (fences, rubble, debris), so they all vanish while the crowd count is high and reappear when it
// drops, which reads as pop-in near large groups of enemies. The handheld configuration skips the rule
// (Perf_IsHandheld), and the runtime reports docked (service/misc.cpp, GetPerformanceConfiguration).
//
// With the option on, this hook restores layer 1 after the original ran the crowd rule, to the value the
// game's normal path would have written: v8 * owner[184], where v8 is the same scale the original applied to
// the other layers (recovered as owner[232] / owner[192], since the crowd path writes layer 3 = v8 * owner[192]).
// All other layers keep the game's own (possibly crowd-reduced) distances.
#include "game/guest.h"
#include "game/hook.h"
#include "gpu/settings.h"
#include "hwder/runtime.h"

HW_GUEST(StageLod_ScaleFlag);
HW_GUEST(Perf_IsHandheld);

namespace {

constexpr u32 kCrowdLimit = 60;  // the game's threshold (f_100044660c compares n against 0x3c)

// Run a guest predicate on a copy of the context so the hook leaves Ctx exactly as the original would.
u32 guest_flag(Ctx* c, void (*fn)(Ctx*)) {
    Ctx t = *c;
    game::call(&t, fn, 0);
    return (u32)t.x[0] & 0xFF;
}

}  // namespace

HW_HOOK(StageLod_Update) {
    const u64 owner = c->x[0];
    bool restore = false;
    if (settings::video().keep_crowd_props) {
        u64 holder = game::deref(HW_ADDR(StageLod_CrowdOwner));  // null until the battle's crowd object exists
        u64 crowd = holder ? game::deref(holder) : 0;
        u32 n = crowd ? game::read<u32>(crowd + 88) : 0;
        bool doubled = guest_flag(c, f_StageLod_ScaleFlag) != 0;  // the original zeroes layer 1 then anyway
        bool handheld = (guest_flag(c, f_Perf_IsHandheld) & 1) != 0;
        restore = !handheld && !doubled && n > kCrowdLimit;
    }
    HW_ORIG(StageLod_Update)(c);
    if (!restore || game::read<float>(owner + 224) != 0.0f) return;
    float tmpl3 = game::read<float>(owner + 192);
    if (tmpl3 <= 0.0f) return;
    float v8 = game::read<float>(owner + 232) / tmpl3;
    game::write<float>(owner + 224, v8 * game::read<float>(owner + 184));
}
