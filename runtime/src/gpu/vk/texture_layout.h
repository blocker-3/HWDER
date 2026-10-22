// Guest texture layout arithmetic (Maxwell block-linear and pitch-linear): pure functions of ImageInfo, shared
// by the texture cache and the unit tests. Moved text-for-text from texture_cache.cpp.
#pragma once
#include "vk_common.h"

namespace gpu::vk {

u32 mip_shift(u32 shift, u32 unit, u32 dim);

struct LevelParams {
    u32 w, h, d;           // texels
    u32 bx, by;            // blocks
    u32 width_bytes;
    u32 bhs, bds;          // block-linear shifts
    u32 gobs_w = 0;
    u64 size;              // guest bytes
};

LevelParams level_params(const ImageInfo& info, u32 l);

}  // namespace gpu::vk
