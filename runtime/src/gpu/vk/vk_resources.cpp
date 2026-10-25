// Resource binding of the Vulkan renderer: dummy images, framebuffer resolution, dynamic rendering, descriptor
// pools, TIC reads and filling a program's descriptor set. Moved text-for-text from vk_renderer.cpp.
#include <optional>
#include "vk_renderer.h"
#include "debug_flags.h"

namespace gpu::vk::detail {
using namespace m3d;

// ------------------------------------------------------------------ resources
void VkRenderer::create_dummies() {
    static u8 zeros[64 * 6] = {};
    if (debug::flag(debug::VkWhiteTex)) memset(zeros, 0xFF, sizeof(zeros));
    ImageInfo i;
    i.fmt = rt_format(0xD5);
    i.width = i.height = 1;
    i.layers = 6;
    i.block_linear = false;
    i.pitch = 4;
    // dummies live outside the cache (fake addresses never touched by the guest)
    dummy2d_ = tc_.get((u64)zeros, i, new Subresource);
    ImageInfo d = i;
    d.type = ImageType::e3D;
    d.layers = 1;
    dummy3d_ = tc_.get((u64)zeros + 64, d, new Subresource);
    ImageInfo o = i;
    o.type = ImageType::e1D;
    dummy1d_ = tc_.get((u64)zeros + 128, o, new Subresource);
    u32 tsc[8] = {2 | (2 << 3) | (2 << 6), 1 | (1 << 4) | (1 << 6), 0, 0, 0, 0, 0, 0};
    dummy_sampler_ = tc_.sampler(tsc);
    g_rec.flush();
}

VkImageView VkRenderer::dummy_view(VkImageViewType t) {
    switch (t) {
    case VK_IMAGE_VIEW_TYPE_3D: return tc_.view(dummy3d_, VK_FORMAT_UNDEFINED, t, 0, 1, 0, 1);
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        return tc_.view(dummy1d_, VK_FORMAT_UNDEFINED, t, 0, 1, 0, 6);
    default: return tc_.view(dummy2d_, VK_FORMAT_UNDEFINED, t, 0, 1, 0, 6);
    }
}

Framebuffer VkRenderer::resolve_framebuffer(const u32* regs, bool for_clear, u32 clear_rt) {
    SlowTimer timer("resolve_framebuffer", 30.0);
    Framebuffer fb;
    u32 ctrl = regs[RtControl];
    u32 count = ctrl & 0xF;
    if (for_clear && clear_rt >= count) count = clear_rt + 1;
    u32 w = 0xFFFFFFFF, h = 0xFFFFFFFF;
    for (u32 i = 0; i < count && i < 8; i++) {
        u32 slot = (ctrl >> (4 + i * 3)) & 7;
        if (for_clear && i == clear_rt) slot = i < (ctrl & 0xF) ? slot : i;
        const u32* rt = regs + RenderTarget + slot * 16;
        u64 va = ((u64)rt[0] << 32) | rt[1];
        FormatInfo fi = rt_format(rt[4]);
        if (!va || !fi.valid()) continue;
        u8* p = host(va);
        if (!p) continue;
        ImageInfo info;
        info.fmt = fi;
        info.render_target = true;
        u32 tile = rt[5];
        if (tile & 0x1000) {
            info.block_linear = false;
            info.pitch = rt[2];
            info.width = std::max(1u, rt[2] / fi.bpb);
        } else {
            info.width = rt[2];
            info.bh = (tile >> 4) & 0xF;
            info.bd = (tile >> 8) & 0xF;
        }
        info.height = std::max(1u, rt[3]);
        u32 n = rt[6] & 0xFFFF;
        if (rt[6] & 0x10000) {
            info.type = ImageType::e3D;
            info.depth = std::max(1u, n);
        } else {
            info.layers = std::max(1u, n);
            info.layer_stride = rt[7] * 4;
        }
        if (!info.width) continue;
        Subresource sub;
        std::optional<ProfScope> fps(std::in_place, kProfFbGet);
        Image* img = tc_.get((u64)p, info, &sub);
        fps.reset();
        u32 layer = sub.layer + (info.type == ImageType::e3D ? 0 : rt[8]);
        fb.color[i] = img;
        fps.emplace(kProfFbView);
        fb.color_view[i] = tc_.view(img, fi.vk, VK_IMAGE_VIEW_TYPE_2D, sub.level, 1, layer, 1);
        fps.reset();
        fb.color_format[i] = img->info.fmt.vk == fi.vk || vk_format_bytes(img->info.fmt.vk) == vk_format_bytes(fi.vk)
                                 ? fi.vk
                                 : img->info.fmt.vk;
        fb.num_colors = i + 1;
        w = std::min(w, img->level_width(sub.level));
        h = std::min(h, img->level_height(sub.level));
        fb.scale = img->scale;
        fb.scale_y = img->scale_y;
    }
    if (regs[ZetaEnable]) {
        u64 va = addr(regs, Zeta);
        FormatInfo fi = zeta_format(regs[Zeta + 2]);
        u8* p = va ? host(va) : nullptr;
        if (p && fi.valid()) {
            ImageInfo info;
            info.fmt = fi;
            info.render_target = true;
            u32 tile = regs[Zeta + 3];
            info.bh = (tile >> 4) & 0xF;
            info.bd = (tile >> 8) & 0xF;
            info.width = std::max(1u, regs[ZetaSize]);
            info.height = std::max(1u, regs[ZetaSize + 1]);
            u32 n = regs[ZetaSize + 2] & 0xFFFF;
            if (!(regs[ZetaSize + 2] & 0x10000)) info.layers = std::max(1u, n);
            info.layer_stride = regs[Zeta + 4] * 4;
            Subresource sub;
            Image* img = tc_.get((u64)p, info, &sub);
            fb.depth = img;
            fb.depth_format = img->info.fmt.vk;
            fb.has_stencil = img->info.fmt.stencil;
            fb.depth_view = tc_.view(img, img->info.fmt.vk, VK_IMAGE_VIEW_TYPE_2D, sub.level, 1, sub.layer, 1);
            w = std::min(w, img->level_width(sub.level));
            h = std::min(h, img->level_height(sub.level));
            fb.scale = img->scale;
            fb.scale_y = img->scale_y;
        }
    }
    if (w == 0xFFFFFFFF) w = h = 1;
    fb.width = w;
    fb.height = h;
    return fb;
}

void VkRenderer::begin_rendering(const Framebuffer& fb) {
    SlowTimer timer("begin_rendering", 30.0);
    if (g_rec.rendering && cur_fb_ == fb) return;
    VkCommandBuffer cb = g_rec.cmd();  // ends a previous rendering instance
    VkRenderingAttachmentInfo ca[8];
    for (u32 i = 0; i < fb.num_colors; i++) {
        ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca[i].imageView = fb.color_view[i];
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = fb.depth_view;
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {fb.width, fb.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = fb.num_colors;
    ri.pColorAttachments = ca;
    if (fb.depth_view) {
        ri.pDepthAttachment = &da;
        if (fb.has_stencil) ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(cb, &ri);
    g_rec.rendering = true;
    cur_fb_ = fb;
}

VkDescriptorSet VkRenderer::alloc_set(VkDescriptorSetLayout layout) {
    g_rec.cmd_in_pass();
    Frame* f = g_s.cur;
    for (;;) {
        if (f->dpool_index >= f->dpools.size()) {
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            ci.maxSets = 4096;
            ci.poolSizeCount = 5;
            ci.pPoolSizes = sizes;
            VkDescriptorPool p;
            VK_CHECK(vkCreateDescriptorPool(dev(), &ci, nullptr, &p));
            f->dpools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f->dpools[f->dpool_index];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(dev(), &ai, &set);
        if (r == VK_SUCCESS) return set;
        f->dpool_index++;
    }
}

bool VkRenderer::read_tic(u64 pool_va, u32 limit, u32 index, u32* out) {
    if (!pool_va || (limit && index > limit)) return false;
    u8* p = host(pool_va + (u64)index * 32);
    if (!p) return false;
    memcpy(out, p, 32);
    return true;
}

// Fill descriptor set `set` for a program's resources. cbuf(index, offset) reads bound cbuf words;
// cb_range(index, va_lo/hi...) gives the bound range of c[index]. pools: [tic va, tic limit, tsc va, tsc limit].
bool VkRenderer::bind_resources(const shader::Resources& res, StageLayout* sl, VkDescriptorSet set,
                                const std::function<u32(u32, u32)>& cbuf,
                                const std::function<bool(u32, u64&, u32&)>& cb_range, const u32* pools,
                                bool linked_tsc, std::vector<std::function<void()>>& post) {
    DescWrite& dw = dw_;
    dw.writes.clear();
    dw.buffers.clear();
    dw.images.clear();
    dw.texels.clear();
    {
        size_t nimg = res.images.size() + 1, ntex = 1;
        for (const auto& t : res.textures) nimg += std::max(1u, t.count);
        for (const auto& t : res.texel_buffers) ntex += std::max(1u, t.count);
        dw.writes.reserve(res.const_buffers.size() + res.storage_buffers.size() + res.texel_buffers.size() + res.textures.size() + res.images.size() + 1);
        dw.buffers.reserve(res.const_buffers.size() + res.storage_buffers.size() + 1);
        dw.images.reserve(nimg);
        dw.texels.reserve(ntex);
    }
    u32 binding = 0;
    auto add_write = [&](VkDescriptorType t, u32 count) -> VkWriteDescriptorSet& {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding++;
        w.descriptorType = t;
        w.descriptorCount = count;
        dw.writes.push_back(w);
        return dw.writes.back();
    };
    const auto& props = display::device().props.limits;
    // constant buffers
    for (const auto& c : res.const_buffers) {
        ProfScope ps(kProfCbuf);
        u32 size = 0;
        u64 va = 0;
        bool bound = cb_range(c.index, va, size);
        u64 bytes = std::min<u64>(std::max<u32>(c.size, 16), 0x10000);
        if (bound && size) bytes = std::min<u64>(bytes, ((u64)size + 15) & ~15ull);
        Slice s = g_rec.stage(bytes, props.minUniformBufferOffsetAlignment);
        g_prof_cbufs++;
        g_prof_cbuf_bytes += bytes;
        // Object blending: vertex-stage c3 holds the object/bone transforms; build it in cached memory (the staging
        // slice is write-combined: reading it back is very slow), blend, then write it out once.
        const bool obj = bound && obj_candidate(c.index, bytes);
        alignas(16) static thread_local u8 obj_buf[4096];
        if (obj && obj_async()) {
            ProfScope ops(kProfObj);
            ObjJob& job = obj_job_slot();
            u64 n = std::min<u64>(bytes, size);
            mm_->read(va, job.data, n);
            if (n < bytes) memset(job.data + n, 0, bytes - n);
            job.dst = s.ptr, job.bytes = bytes, job.index = c.index, job.key = cur_draw_key_, job.tag = gpu::exec_gi_tag();
            obj_job_push();
        } else if (obj) {
            ProfScope ops(kProfObj);
            u64 n = std::min<u64>(bytes, size);
            mm_->read(va, obj_buf, n);
            if (n < bytes) memset(obj_buf + n, 0, bytes - n);
            obj_transforms(c.index, obj_buf, bytes, cur_draw_key_, gpu::exec_gi_tag());
            memcpy(s.ptr, obj_buf, bytes);
        } else if (bound) {
            ProfScope cps(kProfCbufCopy);
            u64 n = std::min<u64>(bytes, size);
            mm_->read(va, s.ptr, n);
            if (n < bytes) memset(s.ptr + n, 0, bytes - n);
            static const bool census = debug::flag(debug::GiObjTrace);
            if (census && cur_stage_ == 0 && c.index != 3 && n >= 48 && n <= 4096 && (gpu::exec_gi_tag() & 1)) {
                alignas(16) u8 tmp[4096];
                mm_->read(va, tmp, n);  // the staging slice is write-combined: read the guest copy instead
                obj_cbuf_census(c.index, tmp, n);
            }
        } else {
            memset(s.ptr, 0, bytes);
        }
        dw.buffers.push_back({s.buf, s.offset, bytes});
        add_write(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1).pBufferInfo = &dw.buffers.back();
    }
    // storage buffers
    for (const auto& sb : res.storage_buffers) {
        u64 va = cbuf(sb.cbuf_index, sb.cbuf_offset) | ((u64)cbuf(sb.cbuf_index, sb.cbuf_offset + 4) << 32);
        u32 size = cbuf(sb.cbuf_index, sb.cbuf_offset + 8);
        u64 bytes = std::max<u64>(16, ((u64)size + 15) & ~15ull);
        bytes = std::min<u64>(bytes, 64ull << 20);
        if (bytes > (1u << 20)) VK_LOG_ONCE("vk: storage buffer of %llu KiB bound (copied per draw, written %d)", (unsigned long long)(bytes >> 10), (int)sb.written);
        u8* src = va ? host(va) : nullptr;
        u64 avail = src ? mm_->contiguous_size(va, size) : 0;
        if (sb.written && src) {
            Buffer* b = new Buffer(create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true));
            memset(b->map, 0, bytes);
            memcpy(b->map, src, std::min<u64>(avail, size));
            u64 n = std::min<u64>(avail, size);
            post.push_back([this, b, src, n] {
                // GPU wrote the buffer: copy back to guest memory once the work completed
                g_rec.defer([b, src, n] {
                    memcpy(src, b->map, n);
                    destroy_buffer(*b);
                    delete b;
                });
            });
            dw.buffers.push_back({b->buf, 0, bytes});
        } else {
            Slice s = g_rec.stage(bytes, props.minStorageBufferOffsetAlignment);
            memset(s.ptr, 0, bytes);
            if (src) memcpy(s.ptr, src, std::min<u64>(avail, size));
            dw.buffers.push_back({s.buf, s.offset, bytes});
        }
        add_write(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1).pBufferInfo = &dw.buffers.back();
    }
    auto handle_of = [&](const shader::TextureUse& t, u32 j) -> u32 {
        u32 raw = cbuf(t.cbuf_index, t.cbuf_offset + j * 4);
        if (t.has_secondary) raw |= cbuf(t.secondary_cbuf_index, t.secondary_cbuf_offset + j * 4);
        return raw;
    };
    // texel buffers
    for (const auto& t : res.texel_buffers) {
        VkWriteDescriptorSet& w = add_write(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, std::max(1u, t.count));
        size_t first = dw.texels.size();
        for (u32 j = 0; j < std::max(1u, t.count); j++) {
            u32 tic[8];
            VkBufferView view = VK_NULL_HANDLE;
            if (read_tic(((u64)pools[0] << 32) | pools[1], pools[2], handle_of(t, j) & 0xFFFFF, tic)) {
                FormatInfo fi = tic_format(tic);
                u64 va = tic[1] | ((u64)(tic[2] & 0xFFFF) << 32);
                u32 texels = (((tic[3] & 0xFFFF) << 16) | (tic[4] & 0xFFFF)) + 1;
                u64 bytes = (u64)texels * fi.bpb;
                u8* src = va ? host(va) : nullptr;
                if (src && fi.valid() && bytes) {
                    bytes = std::min<u64>(bytes, 64ull << 20);
                    if (bytes > (1u << 20)) VK_LOG_ONCE("vk: texel buffer of %llu KiB bound (copied per draw)", (unsigned long long)(bytes >> 10));
                    Slice s = g_rec.stage(bytes, std::max<u64>(props.minTexelBufferOffsetAlignment, 16));
                    memcpy(s.ptr, src, std::min<u64>(bytes, mm_->contiguous_size(va, bytes)));
                    VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
                    ci.buffer = s.buf;
                    ci.format = fi.vk;
                    ci.offset = s.offset;
                    ci.range = bytes;
                    if (vkCreateBufferView(dev(), &ci, nullptr, &view) == VK_SUCCESS)
                        g_rec.defer([view] { vkDestroyBufferView(dev(), view, nullptr); });
                    else
                        view = VK_NULL_HANDLE;
                }
            }
            if (!view) {
                Slice s = g_rec.stage(16, 256);
                memset(s.ptr, 0, 16);
                VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
                ci.buffer = s.buf;
                ci.format = VK_FORMAT_R32_UINT;
                ci.offset = s.offset;
                ci.range = 16;
                vkCreateBufferView(dev(), &ci, nullptr, &view);
                g_rec.defer([view] { vkDestroyBufferView(dev(), view, nullptr); });
            }
            dw.texels.push_back(view);
        }
        w.pTexelBufferView = &dw.texels[first];
    }
    // sampled textures
    std::optional<ProfScope> texs(std::in_place, kProfTextures);
    for (const auto& t : res.textures) {
        u32 n = std::max(1u, t.count);
        VkWriteDescriptorSet& w = add_write(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n);
        size_t first = dw.images.size();
        VkImageViewType vt = view_type(t.type);
        for (u32 j = 0; j < n; j++) {
            u32 h = handle_of(t, j);
            u32 tic_id = h & 0xFFFFF, tsc_id = linked_tsc ? tic_id : h >> 20;
            u32 tic[8], tsc[8];
            VkDescriptorImageInfo ii{dummy_sampler_, dummy_view(vt), VK_IMAGE_LAYOUT_GENERAL};
            static bool white = debug::flag(debug::VkWhiteTex);
            bool tic_ok = !white && read_tic(((u64)pools[0] << 32) | pools[1], pools[2], tic_id, tic);
            if (dumping() && !tic_ok)
                hw_log("fd:   tex DUMMY: handle 0x%x (c%u+0x%x) tic %u not readable (pool %llx limit %u)", h, t.cbuf_index, t.cbuf_offset,
                       tic_id, (unsigned long long)(((u64)pools[0] << 32) | pools[1]), pools[2]);
            if (tic_ok) {
                u64 tkey = 0;
                TicEntry* te = nullptr;
                if (!dumping()) {
                    tkey = hash_bytes((const u8*)tic, 32) ^ ((u64)vt * 0x9E3779B97F4A7C15ull);
                    auto it = tic_cache_.find(tkey);
                    if (it != tic_cache_.end() && it->second.gen == tc_.tex_gen && it->second.vt == vt &&
                        memcmp(it->second.tic, tic, 32) == 0)
                        te = &it->second;
                }
                if (te) {
                    ProfScope hps(kProfTexHit);
                    st_.tic_hit++;
                    tc_.validate(te->img);
                    if (te->view) ii.imageView = te->view;
                    if (drawlog_active()) {
                        char t[96];
                        snprintf(t, sizeof(t), " tex[%llx %ux%u f%d%s]", (unsigned long long)te->img->addr, te->img->info.width,
                                 te->img->info.height, (int)te->img->info.fmt.vk, te->img->gpu_modified ? " gpu" : "");
                        drawlog_tex_ += t;
                    }
                    if (te->img->info.render_target || te->img->gpu_modified) draw_samples_rt_ = true;
                } else {
                ImageInfo info;
                u64 a;
                u32 bl, nl, blayer;
                std::optional<ProfScope> tps(std::in_place, kProfTexGet);
                bool ok = tc_.info_from_tic(tic, info, a, bl, nl, blayer);
                static int nlog = 0;
                if (verbose_ && nlog < 60) {
                    nlog++;
                    hw_log("vk: tex handle 0x%x (c%u+0x%x) tic %u tsc %u: tic %08x %08x %08x %08x %08x %08x %08x %08x -> %s fmt %d %ux%u addr %llx",
                           h, t.cbuf_index, t.cbuf_offset, tic_id, tsc_id, tic[0], tic[1], tic[2], tic[3], tic[4], tic[5],
                           tic[6], tic[7], ok ? "ok" : "REJECTED", (int)info.fmt.vk, info.width, info.height,
                           (unsigned long long)a);
                }
                if (dumping() && !ok)
                    hw_log("fd:   tex DUMMY: handle 0x%x tic %u REJECTED fmt 0x%x words %08x %08x %08x %08x", h, tic_id, tic[0] & 0x7F, tic[0], tic[1], tic[2], tic[3]);
                if (ok) {
                    st_.tic_miss++;
                    Subresource sub;
                    Image* img = tc_.get(a, info, &sub);
                    VkComponentMapping swz{tic_swizzle((tic[0] >> 19) & 7), tic_swizzle((tic[0] >> 22) & 7),
                                           tic_swizzle((tic[0] >> 25) & 7), tic_swizzle((tic[0] >> 28) & 7)};
                    if (info.fmt.swizzle.r != VK_COMPONENT_SWIZZLE_IDENTITY) {
                        auto remap = [&](VkComponentSwizzle s) {
                            switch (s) {
                            case VK_COMPONENT_SWIZZLE_R: return info.fmt.swizzle.r;
                            case VK_COMPONENT_SWIZZLE_G: return info.fmt.swizzle.g;
                            case VK_COMPONENT_SWIZZLE_B: return info.fmt.swizzle.b;
                            case VK_COMPONENT_SWIZZLE_A: return info.fmt.swizzle.a;
                            default: return s;
                            }
                        };
                        swz = {remap(swz.r), remap(swz.g), remap(swz.b), remap(swz.a)};
                    }
                    VkImageAspectFlags aspect = img->info.fmt.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                : img->info.fmt.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                                        : VK_IMAGE_ASPECT_COLOR_BIT;
                    if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
                        // A depth/stencil view exposes a single channel in R. Maxwell's S8Z24 (TIC 0x29)
                        // carries the 24-bit depth as its G component, so the game's TIC swizzles X <- G;
                        // without this remap a depth read returns 0 (the UI depth copy wrote zeros and every
                        // depth-tested marker - names, health bars, attack counters - failed its test).
                        auto fix = [](VkComponentSwizzle c) {
                            return (c == VK_COMPONENT_SWIZZLE_G || c == VK_COMPONENT_SWIZZLE_B || c == VK_COMPONENT_SWIZZLE_A) ? VK_COMPONENT_SWIZZLE_R : c;
                        };
                        swz = {fix(swz.r), fix(swz.g), fix(swz.b), fix(swz.a)};
                    }
                    u32 layers = info.layers;
                    tps.reset();
                    tps.emplace(kProfTexView);
                    VkImageView v = tc_.view(img, info.fmt.vk, vt, sub.level + bl, nl, sub.layer + blayer, layers, swz,
                                             aspect);
                    if (v) ii.imageView = v;
                    if (img->info.render_target || img->gpu_modified) draw_samples_rt_ = true;
                    if (!dumping() && !tc_.has_same_addr_alias(img)) {
                        if (tic_cache_.size() > 65536) tic_cache_.clear();
                        TicEntry e;
                        memcpy(e.tic, tic, 32);
                        e.vt = vt;
                        e.gen = tc_.tex_gen;
                        e.img = img;
                        e.view = v;
                        e.vp = {info.fmt.vk, vt, sub.level + bl, nl, sub.layer + blayer, layers, swz, aspect};
                        tic_cache_[tkey] = e;
                    }
                    if (drawlog_active()) {
                        char t[96];
                        snprintf(t, sizeof(t), " tex[%llx %ux%u f%d%s]", (unsigned long long)img->addr, img->info.width, img->info.height,
                                 (int)img->info.fmt.vk, img->gpu_modified ? " gpu" : "");
                        drawlog_tex_ += t;
                    }
                    if (dumping()) {
                        char t[128];
                        bool stale = !img->gpu_modified && hash_bytes((const u8*)img->addr, img->readable) != img->hash;
                        snprintf(t, sizeof(t), " tex[%llx %ux%u img f%d tic %02x/%08x->f%d%s%s]", (unsigned long long)img->addr, img->info.width,
                                 img->info.height, (int)img->info.fmt.vk, tic[0] & 0x7F, tic[0], (int)info.fmt.vk, img->gpu_modified ? " gpu" : "",
                                 stale ? " STALE" : "");
                        dump_tex_ += t;
                        // Guest bytes of every sampled texture (HWDER_VK_DUMP_TEX=1): fd/tex_<addr>_WxH_f<fmt>_bh<n>_tws<n>_<bl|pitch>.guest
                        static bool dump_tex_bytes = debug::flag(debug::VkDumpTex);
                        if (dump_tex_bytes && img->readable) {
                            { std::error_code mkdir_ec; std::filesystem::create_directory("fd", mkdir_ec); }
                            char path[256];
                            snprintf(path, sizeof(path), "fd/tex_%llx_%ux%u_f%d_bh%u_tws%u_%s%u.guest", (unsigned long long)img->addr,
                                     img->info.width, img->info.height, (int)img->info.fmt.vk, img->info.bh, img->info.tws,
                                     img->info.block_linear ? "bl" : "pitch", img->info.block_linear ? img->info.levels : img->info.pitch);
                            if (FILE* f = fopen(path, "wb")) {
                                fwrite((const void*)img->addr, 1, img->readable, f);
                                fclose(f);
                            }
                        }
                    }
                }
                tps.reset();
                }
            }
            {
                ProfScope sps(kProfSampler);
                if (read_tic(((u64)pools[3] << 32) | pools[4], pools[5], tsc_id, tsc)) ii.sampler = tc_.sampler(tsc);
            }
            dw.images.push_back(ii);
        }
        w.pImageInfo = &dw.images[first];
    }
    texs.reset();
    // storage images
    for (const auto& im : res.images) {
        VkWriteDescriptorSet& w = add_write(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1);
        u32 h = cbuf(im.cbuf_index, im.cbuf_offset);
        u32 tic[8];
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, dummy_view(view_type(im.type)), VK_IMAGE_LAYOUT_GENERAL};
        if (read_tic(((u64)pools[0] << 32) | pools[1], pools[2], h & 0xFFFFF, tic)) {
            ImageInfo info;
            u64 a;
            u32 bl, nl, blayer;
            if (tc_.info_from_tic(tic, info, a, bl, nl, blayer) && !info.fmt.compressed && !info.fmt.depth) {
                Subresource sub;
                Image* img = tc_.get(a, info, &sub);
                VkFormat f = info.fmt.vk;
                if (f == VK_FORMAT_R8G8B8A8_SRGB) f = VK_FORMAT_R8G8B8A8_UNORM;
                if (f == VK_FORMAT_B8G8R8A8_SRGB) f = VK_FORMAT_B8G8R8A8_UNORM;
                VkImageView v = tc_.view(img, f, view_type(im.type), sub.level + bl, 1, sub.layer + blayer, info.layers);
                if (v) ii.imageView = v;
                if (im.is_written) tc_.mark_modified(img);
            }
        }
        dw.images.push_back(ii);
        w.pImageInfo = &dw.images.back();
    }
    {
        ProfScope ups(kProfDescUpdate);
        if (!dw.writes.empty()) vkUpdateDescriptorSets(dev(), (u32)dw.writes.size(), dw.writes.data(), 0, nullptr);
    }
    return true;
}

}  // namespace gpu::vk::detail
