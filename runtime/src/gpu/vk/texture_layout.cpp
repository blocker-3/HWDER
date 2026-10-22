// Guest texture layout arithmetic (Maxwell block-linear and pitch-linear). Moved text-for-text from
// texture_cache.cpp so the unit tests can link it without the Vulkan parts of the cache.
#include <algorithm>

#include "texture_layout.h"

namespace gpu::vk {

u32 mip_shift(u32 shift, u32 unit, u32 dim) {
    while (shift > 0 && (unit << (shift - 1)) >= dim) shift--;
    return shift;
}

LevelParams level_params(const ImageInfo& info, u32 l) {
    LevelParams p;
    p.w = std::max(1u, info.width >> l);
    p.h = std::max(1u, info.height >> l);
    p.d = info.type == ImageType::e3D ? std::max(1u, info.depth >> l) : 1;
    p.bx = (p.w + info.fmt.bw - 1) / info.fmt.bw;
    p.by = (p.h + info.fmt.bh - 1) / info.fmt.bh;
    p.width_bytes = p.bx * info.fmt.bpb;
    if (!info.block_linear) {
        u32 pitch = info.pitch ? info.pitch : p.width_bytes;
        p.bhs = p.bds = 0;
        p.size = (u64)pitch * p.by * p.d;
        return p;
    }
    if (info.levels == 1 && l == 0) {
        p.bhs = info.bh;
        p.bds = info.type == ImageType::e3D ? info.bd : 0;
    } else {
        p.bhs = mip_shift(info.bh, 8, p.by);
        p.bds = info.type == ImageType::e3D ? mip_shift(info.bd, 1, p.d) : 0;
    }
    u32 gobs_w = (p.width_bytes + 63) / 64;
    if (info.tws) {
        u32 a = 1u << info.tws;
        if (gobs_w > a) gobs_w = (gobs_w + a - 1) & ~(a - 1);
    }
    p.gobs_w = gobs_w;
    u32 gobs_h = (p.by + 7) / 8;
    u32 bh = 1u << p.bhs, bd = 1u << p.bds;
    u64 blocks = (u64)gobs_w * ((gobs_h + bh - 1) / bh) * ((p.d + bd - 1) / bd);
    p.size = blocks * (512ull << (p.bhs + p.bds));
    return p;
}

u64 TextureCache::compute_layout(const ImageInfo& info, u64* level_offset, u64* layer_stride) {
    u64 off = 0;
    for (u32 l = 0; l < info.levels && l < 16; l++) {
        if (level_offset) level_offset[l] = off;
        off += level_params(info, l).size;
    }
    u64 stride = off;
    if (info.layers > 1) {
        if (info.layer_stride) {
            stride = info.layer_stride;
        } else if (info.block_linear) {
            u32 bh = info.bh, bd = info.bd;
            u32 rows = (info.height + info.fmt.bh - 1) / info.fmt.bh;
            if (info.tws) {
                u64 a = 512ull << (info.tws + bh + bd);
                stride = (stride + a - 1) & ~(a - 1);
            } else {
                while (bh && rows <= (8u << (bh - 1))) bh--;
                while (bd && info.depth <= (1u << (bd - 1))) bd--;
                u64 a = 512ull << (bh + bd);
                stride = (stride + a - 1) & ~(a - 1);
            }
        }
    }
    if (layer_stride) *layer_stride = stride;
    return stride * (info.layers - 1) + off;
}

}  // namespace gpu::vk
