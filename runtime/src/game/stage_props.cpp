// Keep small stage props drawn in crowded scenes ("Keep props in big battles").
//
// How the game decides to draw a stage part (verified with F9 logs on branch draw-distance-popin and by reading
// the generated code; addresses and names in tools/recomp/symbols.txt):
//   StagePart_UpdateLod (part, view): camera distance d = |part[160..168] - camera|, picks a detail level, then
//   StagePart_UpdateFade: range = table[layer] + bias + 0.5 * part radius (part[256]); beyond range the part's
//     alpha (part+384+4*view) is set to 0 unless StageLod_ModeGate() is true; within range it stays or fades in.
//   StagePart_Append (owner, view, part): a part that is not faded out gets a draw item in the pass's bucket;
//   DrawBucket_CopyToList copies the bucket into the pass's draw list, and StageList_Draw / DrawList_Render draw
//   every entry. So a part culled here never reaches the GPU.
// The distance table lives in the stage renderer ("owner", also the bucket owner) and StageLod_Update rewrites it
// every frame on the game thread: owner + 220 + 4 * layer (layers 0..9) = scale * base values at owner + 180..216
// (layer 0 also times a rodata constant); owner+264 / +268 hold near/far scales. Layer 1 holds the small props
// (fences, rubble, debris).
//
// The crowd rule: in the docked performance configuration (Perf_IsHandheld() false; the runtime reports docked,
// service/misc.cpp) StageLod_Update reads a crowd count n = *(*(*StageLod_CrowdOwner) + 88) (doubled when
// StageLod_ScaleFlag() is set) and, when n exceeds 60 (cap StageLod_CrowdCap(), 210 in battle), writes layer 1's
// distance as 0. Every small prop then vanishes until n drops again, which reads as pop-in near large groups of
// enemies. What n counts exactly is inferred from its behaviour (it follows the size of nearby crowds).
//
// With the option on, this hook restores layer 1 after the original ran the crowd rule, to the value the normal
// path writes: scale * owner[184]. The scale is the one the original applied to the other layers, recovered as
// owner[232] / owner[192] (the crowd path writes layer 3 = scale * owner[192]). All other layers keep the game's
// own, possibly crowd-reduced, distances.
#include <cmath>

#include "game/guest.h"
#include "game/hook.h"
#include "gpu/settings.h"
#include "hwder/runtime.h"

HW_GUEST(StageLod_ScaleFlag);
HW_GUEST(Perf_IsHandheld);

namespace {

constexpr u32 kCrowdLimit = 60;  // the game's threshold (StageLod_Update compares n against 0x3c)

// Run a guest predicate on a copy of the context so the hook leaves Ctx exactly as the original would. Both
// predicates only read memory; StageLod_ScaleFlag's singleton getter initializes its static on the first call,
// which the original body triggers itself in the same frame.
u32 guest_flag(Ctx* c, void (*fn)(Ctx*)) {
    Ctx t = *c;
    game::call(&t, fn, 0);
    return (u32)t.x[0] & 0xFF;
}

bool usable(float v) { return std::isfinite(v) && v >= 0.0f; }

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
    // Scale from layer 3 / its base; fall back to 1 (the game's default scale) if either value is unusable.
    float base1 = game::read<float>(owner + 184), base3 = game::read<float>(owner + 192),
          layer3 = game::read<float>(owner + 232);
    float scale = usable(layer3) && usable(base3) && base3 > 0.0f ? layer3 / base3 : 1.0f;
    float layer1 = scale * base1;
    if (usable(layer1)) game::write<float>(owner + 224, layer1);
}
