// Draw, clear, compute dispatch and 2D blit of the Vulkan renderer: the Maxwell state to pipeline key, vertex
// and index staging, descriptor binding and command recording of every draw. Moved text-for-text from
// vk_renderer.cpp.
#include <optional>
#include <cmath>
#include "host/timing.h"
#include "host/platform.h"

#include "gpu/settings.h"
#include "gpu/diag.h"
#include "vk_renderer.h"
#include "debug_flags.h"

namespace gpu::vk::detail {
using namespace m3d;

// A draw's commands as one recorded closure: the state that changed since the previous draw in this command
// buffer (record_draw tracks it in rs_) and the draw itself.
struct DrawRec {
    enum : u32 {
        kViewport = 1, kScissor = 2, kBias = 4, kBlend = 8, kStencil = 16, kLineWidth = 32, kBounds = 64, kPush = 128,
        kIndexBuffer = 256, kIndexed = 512
    };
    u32 flags = 0;
    u8 nsets = 0, nvb = 0;
    u8 set_slot[5], vb_slot[17];
    VkPipeline pipe = VK_NULL_HANDLE;  // bind when set
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet sets[5];
    VkBuffer vb[17];
    VkDeviceSize vo[17];
    VkViewport vp;
    VkRect2D sc;
    float bias[3], bc[4], lw, bounds[2];
    u32 st[6];
    shader::PushConstants pc;
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize io = 0;
    VkIndexType it = VK_INDEX_TYPE_UINT16;
    u32 count = 0, first = 0, base_instance = 0;
    s32 vertex_offset = 0;

    void exec(VkCommandBuffer cb) const {
        if (pipe) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        for (u32 i = 0; i < nsets; i++)
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set_slot[i], 1, &sets[i], 0, nullptr);
        for (u32 i = 0; i < nvb; i++) vkCmdBindVertexBuffers(cb, vb_slot[i], 1, &vb[i], &vo[i]);
        if (flags & kViewport) vkCmdSetViewport(cb, 0, 1, &vp);
        if (flags & kScissor) vkCmdSetScissor(cb, 0, 1, &sc);
        if (flags & kBias) vkCmdSetDepthBias(cb, bias[0], bias[1], bias[2]);
        if (flags & kBlend) vkCmdSetBlendConstants(cb, bc);
        if (flags & kStencil) {
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, st[0]);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, st[1]);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, st[2]);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, st[3]);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, st[4]);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, st[5]);
        }
        if (flags & kLineWidth) vkCmdSetLineWidth(cb, lw);
        if (flags & kBounds) vkCmdSetDepthBounds(cb, bounds[0], bounds[1]);
        if (flags & kPush) vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        if (flags & kIndexBuffer) vkCmdBindIndexBuffer(cb, ib, io, it);
        if (flags & kIndexed) vkCmdDrawIndexed(cb, count, 1, 0, vertex_offset, base_instance);
        else vkCmdDraw(cb, count, 1, first, base_instance);
    }
};

// ------------------------------------------------------------------ draw
void VkRenderer::build_pipeline_key(const u32* regs, const DrawParams& p, const void* inline_idx, u64 prog_key, const Framebuffer& fb,
                                    const GfxProgram* prog, DrawCtx& d) {
    // ---- pipeline key
    PipelineKey& key = d.key;
    memset(&key, 0, sizeof(key));
    key.program = prog_key;
    key.num_colors = fb.num_colors;
    for (u32 i = 0; i < 8; i++) key.color_formats[i] = fb.color_view[i] ? fb.color_format[i] : 0;
    key.depth_format = fb.depth_view ? fb.depth_format : 0;
    u32 topo = p.topology & 0xFFFF;
    bool quads = topo == 7 || topo == 8;
    key.topology = topology(topo);
    key.patch_points = regs[0x373];
    // rasterizer
    static const bool no_cull = debug::flag(debug::VkNoCull);
    key.cull_enable = regs[GlCullTestEnabled] != 0 && !no_cull;
    key.cull_mode = regs[GlCullFace] == 0x404 ? VK_CULL_MODE_FRONT_BIT
                    : regs[GlCullFace] == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK
                                                : VK_CULL_MODE_BACK_BIT;
    if (regs[AlphaTestEnable]) {  // alpha test is not emulated: log each distinct func/ref once
        static std::unordered_set<u64> seen_alpha;
        u64 k = (u64)regs[AlphaTestFunc] << 32 | regs[AlphaTestRef];
        if (seen_alpha.size() < 16 && seen_alpha.insert(k).second)
            hw_log("vk: draw uses alpha test (not emulated): func 0x%x ref %.3f", regs[AlphaTestFunc],
                   *(const float*)&regs[AlphaTestRef]);
    }
    // Facing: the hardware evaluates the GL area formula in its window coordinates (y-up for a
    // lower-left origin, y-down for upper-left) and TRIANGLE_RAST_FLIP (WindowOrigin bit 4)
    // negates the result. Vulkan evaluates the visual orientation of the framebuffer picture, so
    // the two agree exactly when lower_left != flip_y (GL style: lower-left/no flip; D3D style:
    // upper-left + negative scale + flip) and are mirrored otherwise. HW:DE draws its post-process
    // quads upper-left + flip_y + CCW; inverting on flip_y alone culled every one of them.
    bool flip_y = (regs[WindowOrigin] >> 4) & 1;
    bool ll = regs[WindowOrigin] & 1;
    bool ccw = regs[GlFrontFace] == 0x901;
    static const char* face_rule = debug::str(debug::VkFaceRule);
    bool invert = face_rule ? (atoi(face_rule) == 1 ? flip_y : false) : (ll == flip_y);
    if (invert) ccw = !ccw;
    key.front_face = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    u32 pm = regs[PolygonModeFront];
    key.polygon_mode = pm == 0x1B00 ? VK_POLYGON_MODE_POINT : pm == 0x1B01 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    {
        u32 cls = key.topology <= VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 0
                  : key.topology <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? 1
                                                                      : 2;
        key.depth_bias = regs[0x370 + cls] != 0;
    }
    key.discard = regs[0xDF] == 0 && false;
    // depth / stencil
    if (fb.depth_view) {
        key.depth_test = regs[DepthTestEnable] != 0;
        key.depth_write = regs[DepthWriteEnable] != 0;
        key.depth_func = compare_op(regs[DepthTestFunc]);
        key.depth_bounds = regs[0x66F] != 0;
        if (fb.has_stencil && regs[StencilEnable]) {
            key.stencil_enable = 1;
            for (int k = 0; k < 3; k++) key.front[k] = stencil_op(regs[StencilFrontOp + k]);
            key.front[3] = compare_op(regs[StencilFrontOp + 3]);
            const u32* back = regs[StencilTwoSideEnable] ? regs + StencilBackOp : regs + StencilFrontOp;
            for (int k = 0; k < 3; k++) key.back[k] = stencil_op(back[k]);
            key.back[3] = compare_op(back[3]);
        }
    } else {
        key.depth_func = VK_COMPARE_OP_ALWAYS;
    }
    // blend
    for (u32 i = 0; i < fb.num_colors; i++) {
        const u32* b = regs[BlendPerTargetEnabled] ? regs + BlendPerTarget + i * 8 : nullptr;
        key.blend_enable[i] = fb.color_view[i] && regs[Blend + 9 + i] != 0 && !fb.color[i]->info.fmt.integer;
        if (key.blend_enable[i]) {
            if (b) {
                key.blend[i][0] = blend_op(b[1]);
                key.blend[i][1] = blend_factor(b[2]);
                key.blend[i][2] = blend_factor(b[3]);
                key.blend[i][3] = blend_op(b[4]);
                key.blend[i][4] = blend_factor(b[5]);
                key.blend[i][5] = blend_factor(b[6]);
            } else {
                const u32* c = regs + Blend;
                key.blend[i][0] = blend_op(c[1]);
                key.blend[i][1] = blend_factor(c[2]);
                key.blend[i][2] = blend_factor(c[3]);
                key.blend[i][3] = blend_op(c[4]);
                key.blend[i][4] = blend_factor(c[5]);
                key.blend[i][5] = blend_factor(c[7]);
            }
        }
        u32 cm = regs[ColorMaskCommon] ? regs[ColorMask] : regs[ColorMask + i];
        key.color_mask[i] = ((cm & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((cm & 0x10) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                            ((cm & 0x100) ? VK_COLOR_COMPONENT_B_BIT : 0) |
                            ((cm & 0x1000) ? VK_COLOR_COMPONENT_A_BIT : 0);
    }
    if (regs[LogicOp] && fb.num_colors) {
        key.logic_enable = 1;
        key.logic = logic_op(regs[LogicOp + 1]);
    }
    bool strip = key.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                 key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    key.prim_restart = regs[PrimitiveRestart] && strip && (p.indexed || inline_idx);

    // ---- vertex input
    u32 attrs_used = prog->attributes;
    u32 streams_used = 0;
    bool need_const = false;
    for (u32 a = 0; a < 32; a++) {
        if (!(attrs_used & (1u << a))) continue;
        u32 raw = regs[VertexAttribFormat + a];
        u32 sz = 0;
        VkFormat f = cached_vertex_format(raw, &sz);
        u32 s = raw & 0x1F;
        bool constant = (raw >> 6) & 1;
        bool stream_ok = !constant && f && s < 16 && (regs[VertexStreams + s * 4] & 0x1000) &&
                         addr(regs, VertexStreams + s * 4 + 1);
        if (stream_ok) {
            key.attrib_format[a] = f;
            key.attrib_offset[a] = (raw >> 7) & 0x3FFF;
            key.attrib_binding[a] = s;
            streams_used |= 1u << s;
        } else {
            key.attrib_format[a] = VK_FORMAT_R32G32B32A32_SFLOAT;
            key.attrib_offset[a] = 0;
            key.attrib_binding[a] = kConstBinding;
            need_const = true;
        }
    }
    d.topo = topo;
    d.quads = quads;
    d.attrs_used = attrs_used;
    d.streams_used = streams_used;
    d.need_const = need_const;
}

bool VkRenderer::geom_check(GeomEntry& e, const u8* src, u64 bytes) {
    const u64 serial = g_rec.serial();
    if (e.checked_serial == serial) return e.clean_now;
    e.checked_serial = serial;
    e.last_epoch = tc_.epoch();
    const int written = src && bytes ? tc_.written_since((u64)(uintptr_t)src, bytes, e.watch_epoch) : -1;
    const u64 sample = sample_hash(src, bytes);
    e.clean_now = written == 0 && e.sample == sample;
    e.sample = sample;
    e.clean = e.clean_now ? e.clean + 1 : 0;
    if (!e.clean_now) {
        e.has_range = false;
        if (e.chunk >= 0) geom_release(e);
    }
    return e.clean_now;
}

void VkRenderer::geom_release(GeomEntry& e) {
    // The last submission that used the copy may still be on the GPU: free it once this one completes.
    std::shared_ptr<std::atomic<u64>> live = geom_chunks_[e.chunk].live;
    const u64 n = (e.bytes + 255) & ~255ull;
    g_rec.defer([live, n] { live->fetch_sub(n); });
    e.chunk = -1;
}

bool VkRenderer::geom_slice(u64 key, const u8* src, u64 bytes, Slice& out) {
    {   // drop ranges not drawn for 300 frames: a 256th of the table per frame, in place (a whole-table rebuild every
        // 300 frames took ~20 ms on this thread and made one frame in every 300 late; a 32nd per frame still cost
        // ~4% of the thread in a heavy battle)
        const u64 epoch = tc_.epoch();
        auto stale = [&](u64, GeomEntry& g) {
            if (epoch - g.last_epoch <= 300) return false;
            if (g.chunk >= 0) geom_release(g);
            return true;
        };
        if (geom_.size() > 200000) {
            geom_.erase_if([&](u64, GeomEntry& g) {
                if (g.chunk >= 0) geom_release(g);
                return true;
            });
            geom_cursor_ = 0;
        } else if (epoch != geom_swept_) {
            geom_swept_ = epoch;
            geom_cursor_ = geom_.sweep(geom_cursor_, geom_.capacity() / 256 + 1, stale);
        }
    }
    GeomEntry& e = geom_[key];
    if (!geom_check(e, src, bytes)) return false;
    if (e.chunk >= 0 && e.bytes == bytes) {
        GeomChunk& c = geom_chunks_[e.chunk];
        out = {c.buf.buf, e.off, c.buf.map + e.off};
        return true;
    }
    if (e.chunk >= 0) geom_release(e);
    if (e.clean < 2) return false;  // rewritten recently: keep staging it per frame
    const u64 need = (bytes + 255) & ~255ull;
    if (need > kGeomChunk / 4) return false;
    if (geom_cur_ < 0 || geom_chunks_[geom_cur_].head + need > kGeomChunk) {
        geom_cur_ = -1;
        for (int i = 0; i < (int)geom_chunks_.size(); i++)
            if (geom_chunks_[i].live->load() == 0) {  // every range freed and done on the GPU: reuse from the start
                geom_chunks_[i].head = 0;
                geom_cur_ = i;
                break;
            }
        if (geom_cur_ < 0) {
            if (geom_chunks_.size() >= kGeomMaxChunks) return false;
            GeomChunk c;
            c.buf = create_buffer(kGeomChunk, kRingUsage, true, true);
            c.live = std::make_shared<std::atomic<u64>>(0);
            geom_chunks_.push_back(std::move(c));
            geom_cur_ = (int)geom_chunks_.size() - 1;
        }
    }
    GeomChunk& c = geom_chunks_[geom_cur_];
    e.chunk = geom_cur_;
    e.off = c.head;
    e.bytes = bytes;
    c.head += need;
    c.live->fetch_add(need);
    memcpy(c.buf.map + e.off, src, bytes);
    g_prof_vtx_bytes += bytes;  // counted as a copy once, when it becomes resident
    out = {c.buf.buf, e.off, c.buf.map + e.off};
    return true;
}

bool VkRenderer::read_indices(const u32* regs, const DrawParams& p, const void* inline_idx, u32 inline_size, DrawCtx& d) {
    const PipelineKey& key = d.key;
    const u32 topo = d.topo;
    const bool quads = d.quads;
    // ---- indices
    bool indexed = p.indexed || inline_idx;
    u32 isize = 0;
    std::vector<u32> converted;
    const u8* idx_src = nullptr;
    if (indexed) {
        if (inline_idx) {
            isize = inline_size;
            idx_src = (const u8*)inline_idx;
        } else {
            u32 fmt = regs[IndexBuffer + 4];
            isize = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
            u64 va = addr(regs, IndexBuffer) + (u64)p.first * isize;
            idx_src = host(va);
            if (!idx_src || mm_->contiguous_size(va, (u64)p.count * isize) < (u64)p.count * isize) {
                VK_LOG_ONCE("vk: index buffer not mapped");
                skipped_++;
                return false;
            }
        }
    }
    auto idx_at = [&](u32 i) -> u32 {
        if (isize == 1) return idx_src[i];
        if (isize == 2) return ((const u16*)idx_src)[i];
        return ((const u32*)idx_src)[i];
    };
    // vertex range
    u32 vmin = 0, vmax = 0, count = p.count;
    if (indexed) {
        u64 ikey = 0, isample = 0;
        IdxRange* cached = nullptr;
        GeomEntry* range = nullptr;  // persistent scan result, valid while the indices are unchanged
        if (!inline_idx) {
            u64 kw[3] = {(u64)(uintptr_t)idx_src, ((u64)p.count << 8) | (isize << 1) | (key.prim_restart ? 1 : 0), 0x1d1d};
            ikey = hash_bytes(kw, sizeof(kw), 0);
            range = &geom_[ikey ^ 0x5ca75ca75ca75ca7ull];
            // Unchanged since its scan (write watch + sample): use the persistent range without the per-submission cache.
            if (!geom_check(*range, idx_src, (u64)p.count * isize) || !range->has_range) {
                isample = sample_hash(idx_src, (u64)p.count * isize);
                auto cit = idx_range_cache_.find(ikey);
                if (cit != idx_range_cache_.end() && cit->second.serial == g_rec.serial() && cit->second.sample == isample) {
                    cached = &cit->second;
                    range->has_range = true, range->vmin = cached->vmin, range->vmax = cached->vmax;
                }
            }
        }
        if (range && range->has_range && !cached) {
            st_.idx_hit++;
            vmin = range->vmin;
            vmax = range->vmax;
        } else if (cached) {
            st_.idx_hit++;
            vmin = cached->vmin;
            vmax = cached->vmax;
        } else {
            st_.idx_miss++;
            u32 restart = isize == 1 ? 0xFF : isize == 2 ? 0xFFFF : 0xFFFFFFFF;
            vmin = 0xFFFFFFFF;
            g_prof_idx_scanned += p.count;
            for (u32 i = 0; i < p.count; i++) {
                u32 v = idx_at(i);
                if (key.prim_restart && v == restart) continue;
                vmin = std::min(vmin, v);
                vmax = std::max(vmax, v);
            }
            if (vmin == 0xFFFFFFFF) vmin = vmax = 0;
            if (!inline_idx) {
                if (idx_range_cache_.size() > 32768) idx_range_cache_.clear();
                idx_range_cache_[ikey] = {g_rec.serial(), isample, vmin, vmax};
                range->has_range = true;
                range->vmin = vmin;
                range->vmax = vmax;
            }
        }
        vmin += p.base_vertex;
        vmax += p.base_vertex;
    } else {
        vmin = p.first;
        vmax = p.first + p.count - 1;
    }
    if (quads) {
        u32 nq = topo == 7 ? count / 4 : (count >= 4 ? (count - 2) / 2 : 0);
        converted.reserve(nq * 6);
        for (u32 q = 0; q < nq; q++) {
            u32 b = topo == 7 ? q * 4 : q * 2;
            u32 v[4] = {b, b + 1, topo == 7 ? b + 2 : b + 3, topo == 7 ? b + 3 : b + 2};
            for (u32 k : {0u, 1u, 2u, 0u, 2u, 3u}) {
                u32 id = indexed ? idx_at(v[k]) : p.first + v[k];
                converted.push_back(id);
            }
        }
        count = (u32)converted.size();
        if (!count) return false;
    }
    d.indexed = indexed;
    d.isize = isize;
    d.converted = std::move(converted);
    d.idx_src = idx_src;
    d.vmin = vmin;
    d.vmax = vmax;
    d.count = count;
    return true;
}

void VkRenderer::stage_vertex_input(const u32* regs, const DrawParams& p, DrawCtx& d) {
    PipelineKey& key = d.key;
    const u32 streams_used = d.streams_used;
    const bool need_const = d.need_const;
    const bool indexed = d.indexed;
    const u32 isize = d.isize;
    const u8* const idx_src = d.idx_src;
    const u32 vmin = d.vmin, vmax = d.vmax;
    // ---- vertex streams
    VkBuffer* vbufs = d.vbufs;
    VkDeviceSize* voffs = d.voffs;
    u32 vb_first = 32, vb_last = 0;
    for (u32 s = 0; s < 16; s++) {
        if (!(streams_used & (1u << s))) continue;
        u32 cfg = regs[VertexStreams + s * 4];
        u32 stride = cfg & 0xFFF;
        u64 va = addr(regs, VertexStreams + s * 4 + 1);
        u64 limit = addr(regs, VertexStreamLimits + s * 2);
        u64 size = limit >= va ? limit - va + 1 : 0;
        bool instanced = regs[VertexStreamInstances + s] & 1;
        u64 start = 0, end = size;
        if (instanced) {
            start = (u64)p.base_instance * stride;
            end = start + std::max(stride, 16u) + 64;
            key.binding_stride[s] = 0;
        } else {
            key.binding_stride[s] = stride;
            if (stride) {
                start = (u64)vmin * stride;
                end = ((u64)vmax + 1) * stride + 64;
            } else {
                end = 64;
            }
        }
        if (size && end > size) end = size;
        if (start >= end) start = end > 64 ? end - 64 : 0;
        u64 bytes = end - start;
        u8* src = host(va + start);
        u64 avail = src ? mm_->contiguous_size(va + start, bytes) : 0;
        Slice sl;
        {
            u64 kw[2] = {va + start, bytes};
            u64 vkey = hash_bytes(kw, sizeof(kw), 0x7b7b);
            u64 vsample = 0;
            FlatMap<SliceEntry>::Slot* vit = nullptr;
            if (avail == bytes && geom_slice(vkey, src, bytes, sl)) {
                st_.vb_hit++;
            } else if ((vsample = sample_hash(src, avail), vit = vb_cache_.find(vkey)) && vit->second.serial == g_rec.serial() &&
                       vit->second.bytes == bytes && vit->second.sample == vsample) {
                st_.vb_hit++;
                sl = vit->second.slice;
            } else {
                st_.vb_miss++;
                sl = g_rec.stage(std::max<u64>(bytes, 16), 16);
                g_prof_vtx_copies++;
                g_prof_vtx_bytes += bytes;
                if (avail) memcpy(sl.ptr, src, avail);
                if (avail < bytes) memset(sl.ptr + avail, 0, bytes - avail);
                if (vb_cache_.size() > 32768) vb_cache_.clear();
                vb_cache_[vkey] = {g_rec.serial(), bytes, vsample, sl};
            }
        }
        if (dumping()) {
            float f4[4] = {};
            if (avail >= 16) memcpy(f4, src, 16);
            hw_log("fd:   vstream %u va %llx stride %u limit-size %llx start %llx bytes %llx avail %llx vmin %u vmax %u first %.3f %.3f %.3f %.3f", s,
                   (unsigned long long)va, stride, (unsigned long long)size, (unsigned long long)start, (unsigned long long)bytes,
                   (unsigned long long)avail, vmin, vmax, f4[0], f4[1], f4[2], f4[3]);
            static const bool fd_raw = debug::flag(debug::FdRaw);
            if (fd_raw && avail) {  // raw vertex data for offline analysis
                char path[128];
                snprintf(path, sizeof(path), "fd/vs_%llu_s%u.bin", (unsigned long long)draws_, s);
                if (FILE* f = fopen(path, "wb")) {
                    fwrite(src, 1, avail, f);
                    fclose(f);
                }
                if (indexed && idx_src) {
                    snprintf(path, sizeof(path), "fd/ib_%llu_i%u.bin", (unsigned long long)draws_, isize);
                    if (FILE* f = fopen(path, "wb")) {
                        fwrite(idx_src, 1, (u64)p.count * isize, f);
                        fclose(f);
                    }
                }
            }
            if (bytes <= 256 && stride >= 4 && avail == bytes) {
                for (u64 off = 0; off + stride <= bytes; off += stride) {
                    char vb[400];
                    int n = 0;
                    for (u32 k = 0; k < std::min<u32>(stride / 4, 8); k++) {
                        float fv;
                        memcpy(&fv, src + off + k * 4, 4);
                        n += snprintf(vb + n, sizeof(vb) - n, " %.4g", fv);
                    }
                    hw_log("fd:     v[%llu]:%s", (unsigned long long)(off / stride), vb);
                }
            }
        }
        vbufs[s] = sl.buf;
        // vertex rate streams hold vertices [vmin, ...]: rebase so that vertex vmin is element 0
        voffs[s] = sl.offset;
        vb_first = std::min(vb_first, s);
        vb_last = std::max(vb_last, s);
    }
    if (need_const) {
        key.binding_stride[kConstBinding] = 0;
        Slice sl = g_rec.stage(16, 16);
        float def[4] = {0, 0, 0, 1};
        memcpy(sl.ptr, def, 16);
        vbufs[kConstBinding] = sl.buf;
        voffs[kConstBinding] = sl.offset;
    }
}

void VkRenderer::stage_indices(const DrawParams& p, const void* inline_idx, DrawCtx& d) {
    const PipelineKey& key = d.key;
    const bool quads = d.quads;
    bool& indexed = d.indexed;
    const u32 isize = d.isize;
    const std::vector<u32>& converted = d.converted;
    const u8* const idx_src = d.idx_src;
    const u32 vmin = d.vmin, count = d.count;
    // ---- index buffer upload
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkDeviceSize ioff = 0;
    VkIndexType itype = VK_INDEX_TYPE_UINT32;
    s32 vertex_offset = 0;
    u32 first_vertex = 0;
    if (quads) {
        Slice sl = g_rec.stage(converted.size() * 4, 16);
        for (size_t i = 0; i < converted.size(); i++) {
            u32 v = converted[i];
            if (indexed) v += p.base_vertex;
            ((u32*)sl.ptr)[i] = v - vmin;
        }
        ibuf = sl.buf;
        ioff = sl.offset;
        indexed = true;
        vertex_offset = 0;
    } else if (indexed) {
        if (isize == 1) {
            Slice sl = g_rec.stage((u64)count * 2, 16);
            for (u32 i = 0; i < count; i++) {
                u32 v = idx_src[i];
                ((u16*)sl.ptr)[i] = (u16)(v == 0xFF && key.prim_restart ? 0xFFFF : v);
            }
            ibuf = sl.buf;
            ioff = sl.offset;
            itype = VK_INDEX_TYPE_UINT16;
        } else {
            Slice sl;
            u64 kw[2] = {(u64)(uintptr_t)idx_src, (u64)count * isize};
            u64 ikey2 = hash_bytes(kw, sizeof(kw), 0x1b1b);
            u64 isample2 = 0;
            FlatMap<SliceEntry>::Slot* iit = nullptr;
            if (!inline_idx && geom_slice(ikey2, idx_src, (u64)count * isize, sl)) {
            } else if (!inline_idx && (isample2 = sample_hash(idx_src, (u64)count * isize), iit = vb_cache_.find(ikey2)) &&
                       iit->second.serial == g_rec.serial() && iit->second.bytes == (u64)count * isize &&
                       iit->second.sample == isample2) {
                sl = iit->second.slice;
            } else {
                sl = g_rec.stage((u64)count * isize, 16);
                g_prof_idx_bytes += (u64)count * isize;
                memcpy(sl.ptr, idx_src, (u64)count * isize);
                if (!inline_idx) vb_cache_[ikey2] = {g_rec.serial(), (u64)count * isize, isample2, sl};
            }
            ibuf = sl.buf;
            ioff = sl.offset;
            itype = isize == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        }
        vertex_offset = (s32)(p.base_vertex - vmin);
    } else {
        first_vertex = p.first - vmin;
    }
    d.ibuf = ibuf;
    d.ioff = ioff;
    d.itype = itype;
    d.vertex_offset = vertex_offset;
    d.first_vertex = first_vertex;
}

void VkRenderer::record_draw(const u32* regs, const DrawParams& p, const Framebuffer& fb, GfxProgram* prog, VkPipeline pipe,
                             const VkDescriptorSet* sets, std::vector<std::function<void()>>& post, DrawCtx& d) {
    const PipelineKey& key = d.key;
    const u32 streams_used = d.streams_used;
    const bool need_const = d.need_const;
    const bool indexed = d.indexed;
    const u32 count = d.count;
    const VkBuffer* vbufs = d.vbufs;
    const VkDeviceSize* voffs = d.voffs;
    const VkBuffer ibuf = d.ibuf;
    const VkDeviceSize ioff = d.ioff;
    const VkIndexType itype = d.itype;
    const s32 vertex_offset = d.vertex_offset;
    const u32 first_vertex = d.first_vertex;
    // ---- record: one closure with the state that changed (rs_) and the draw
    begin_rendering(fb);
    g_rec.cmd_in_pass();
    DrawRec r;
    if (rs_.serial != g_rec.serial()) {
        rs_ = RecState{};
        rs_.serial = g_rec.serial();
    }
    if (rs_.pipe != pipe) {
        r.pipe = pipe;
        rs_.pipe = pipe;
    }
    if (rs_.set_layout != prog->layout) {  // sets bound with another layout no longer count as bound
        rs_.set_layout = prog->layout;
        for (VkDescriptorSet& bs : rs_.sets) bs = VK_NULL_HANDLE;
    }
    for (u32 s = 0; s < 5; s++)
        if (sets[s] && rs_.sets[s] != sets[s]) {
            r.set_slot[r.nsets] = (u8)s;
            r.sets[r.nsets++] = sets[s];
            rs_.sets[s] = sets[s];
        }
    for (u32 s = 0; s < 16; s++)
        if ((streams_used & (1u << s)) && (rs_.vb[s] != vbufs[s] || rs_.vo[s] != voffs[s])) {
            r.vb_slot[r.nvb] = (u8)s;
            r.vo[r.nvb] = voffs[s];
            r.vb[r.nvb++] = vbufs[s];
            rs_.vb[s] = vbufs[s];
            rs_.vo[s] = voffs[s];
        }
    if (need_const && (rs_.vb[kConstBinding] != vbufs[kConstBinding] || rs_.vo[kConstBinding] != voffs[kConstBinding])) {
        r.vb_slot[r.nvb] = (u8)kConstBinding;
        r.vo[r.nvb] = voffs[kConstBinding];
        r.vb[r.nvb++] = vbufs[kConstBinding];
        rs_.vb[kConstBinding] = vbufs[kConstBinding];
        rs_.vo[kConstBinding] = voffs[kConstBinding];
    }
    // viewport
    {
        const float* vt = (const float*)(regs + ViewportTransform);
        float x, y, w, h;
        if (regs[0x64B]) {
            x = vt[3] - vt[0];
            w = vt[0] * 2.0f;
            y = vt[4] - vt[1];
            h = vt[1] * 2.0f;
        } else {
            x = 0;
            y = 0;
            w = (float)fb.width;
            h = (float)fb.height;
        }
        // Lower-left window origin (or a viewport swizzle that negates Y) flips the viewport. The
        // WindowOrigin flip-y bit only inverts the front face (handled in the pipeline key).
        bool lower_left = regs[WindowOrigin] & 1;
        bool swz_neg_y = ((regs[ViewportTransform + 6] >> 4) & 7) == 3;
        if (lower_left != swz_neg_y) {
            y += h;
            h = -h;
        }
        float near_z, far_z;
        if (regs[DepthMode] == 0) {
            near_z = vt[5] - vt[2];
            far_z = vt[5] + vt[2];
        } else {
            near_z = vt[5];
            far_z = vt[5] + vt[2];
        }
        if (!regs[0x64B]) near_z = 0, far_z = 1;
        if (!regs[0x64B] && (fb.scale != 1.0f || fb.scale_y != 1.0f)) {
            if (++audit::unscaled_viewports == 1)
                hw_log("gfx-audit: first disabled viewport transform: present %llu physical %ux%u scale %.3fx%.3f",
                       (unsigned long long)g_s.present_count, fb.width, fb.height, fb.scale, fb.scale_y);
        }
        // A disabled transform uses the framebuffer's physical dimensions already.
        if (regs[0x64B] && (fb.scale != 1.0f || fb.scale_y != 1.0f)) {
            x *= fb.scale;
            y *= fb.scale_y;
            w *= fb.scale;
            h *= fb.scale_y;
        }
        VkViewport v{x, y, w ? w : 1.0f, h ? h : 1.0f, std::clamp(near_z, 0.0f, 1.0f), std::clamp(far_z, 0.0f, 1.0f)};
        if (memcmp(&rs_.vp, &v, sizeof(v)) != 0) {
            r.flags |= DrawRec::kViewport;
            r.vp = v;
            rs_.vp = v;
        }
        VkRect2D sc{{0, 0}, {fb.width, fb.height}};
        const u32* st = regs + ScissorTest;
        if (st[0]) {
            s32 x0 = st[1] & 0xFFFF, x1 = st[1] >> 16, y0 = st[2] & 0xFFFF, y1 = st[2] >> 16;
            if (lower_left) {
                s32 clip_h = (s32)(regs[0x3FD] >> 16);
                if (!clip_h) clip_h = (s32)(fb.height / fb.scale_y + 0.5f);
                s32 ny0 = clip_h - y1, ny1 = clip_h - y0;
                y0 = std::max(0, ny0);
                y1 = std::max(0, ny1);
            }
            if (fb.scale != 1.0f || fb.scale_y != 1.0f) {
                x0 = (s32)(x0 * fb.scale);
                x1 = (s32)(x1 * fb.scale + 0.5f);
                y0 = (s32)(y0 * fb.scale_y);
                y1 = (s32)(y1 * fb.scale_y + 0.5f);
            }
            x0 = std::clamp(x0, 0, (s32)fb.width);
            x1 = std::clamp(x1, x0, (s32)fb.width);
            y0 = std::clamp(y0, 0, (s32)fb.height);
            y1 = std::clamp(y1, y0, (s32)fb.height);
            sc = {{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
        }
        if (memcmp(&rs_.sc, &sc, sizeof(sc)) != 0) {
            r.flags |= DrawRec::kScissor;
            r.sc = sc;
            rs_.sc = sc;
        }
    }
    {
        float units, slope, clamp;
        memcpy(&units, &regs[DepthBias], 4);
        memcpy(&slope, &regs[SlopeScaleDepthBias], 4);
        memcpy(&clamp, &regs[0x61F], 4);
        if (key.depth_bias) {
            auto& count = audit_depth_bias_[(u32)fb.depth_format];
            if (++count == 1)
                hw_log("gfx-audit: first depth bias: format %u present %llu units %g slope %g clamp %g",
                       (unsigned)fb.depth_format, (unsigned long long)g_s.present_count, units, slope, clamp);
        }
        float db[3] = {units / 2.0f, std::isfinite(clamp) ? clamp : 0.0f, slope};
        if (memcmp(rs_.bias, db, sizeof(db)) != 0) {
            r.flags |= DrawRec::kBias;
            memcpy(r.bias, db, sizeof(db));
            memcpy(rs_.bias, db, sizeof(db));
        }
        float bc[4];
        memcpy(bc, &regs[BlendColor], 16);
        if (memcmp(rs_.bc, bc, 16) != 0) {
            r.flags |= DrawRec::kBlend;
            memcpy(r.bc, bc, 16);
            memcpy(rs_.bc, bc, 16);
        }
        bool two = regs[StencilTwoSideEnable];
        u32 stv[6] = {regs[StencilFrontFuncMask], regs[StencilFrontMask], regs[StencilFrontRef],
                      two ? regs[StencilBackFuncMask] : regs[StencilFrontFuncMask],
                      two ? regs[StencilBackMask] : regs[StencilFrontMask], two ? regs[StencilBackRef] : regs[StencilFrontRef]};
        if (memcmp(rs_.st, stv, sizeof(stv)) != 0) {
            r.flags |= DrawRec::kStencil;
            memcpy(r.st, stv, sizeof(stv));
            memcpy(rs_.st, stv, sizeof(stv));
        }
        float lw;
        memcpy(&lw, &regs[LineWidthAliased], 4);
        float lwv = (lw > 0 && lw < 64) ? lw : 1.0f;
        if (rs_.lw != lwv) {
            r.flags |= DrawRec::kLineWidth;
            r.lw = lwv;
            rs_.lw = lwv;
        }
        float bmin, bmax;
        memcpy(&bmin, &regs[DepthBoundsMin], 4);
        memcpy(&bmax, &regs[DepthBoundsMax], 4);
        float bd[2] = {std::clamp(bmin, 0.0f, 1.0f), std::clamp(bmax, 0.0f, 1.0f)};
        if (memcmp(rs_.bounds, bd, sizeof(bd)) != 0) {
            r.flags |= DrawRec::kBounds;
            memcpy(r.bounds, bd, sizeof(bd));
            memcpy(rs_.bounds, bd, sizeof(bd));
        }
    }
    shader::PushConstants pc{};
    pc.viewport_scale[0] = pc.viewport_scale[1] = pc.viewport_scale[2] = pc.viewport_scale[3] = 1.0f;
    if (fb.scale != fb.scale_y && fb.scale > 0.0f) {
        // "Unlocked" rendering at a non-16:9 aspect: the game's projection is 16:9, so the stretched
        // target is corrected by squeezing clip-space X. Perspective draws get a wider field of view,
        // orthographic UI is pillarboxed to a centred 16:9 area - except full-screen passes that
        // sample render targets (post-processing), which must keep covering the whole target.
        float fix = fb.scale_y / fb.scale;
        pc.viewport_scale[0] = fix;
        pc.viewport_scale[1] = draw_samples_rt_ ? 1.0f : fix;
        // Fill ultrawide: the game's own projection already has the window's aspect, so perspective draws
        // are not squeezed; the orthographic HUD is still pillarboxed to a centred 16:9.
        if (settings::video().unlocked_res && settings::video().ultrawide) {
            pc.viewport_scale[0] = 1.0f;
            if (draw_samples_rt_)  // full-screen passes keep the whole width: the side bands get drawn
                for (u32 i = 0; i < fb.num_colors; i++)
                    if (fb.color[i]) {
                        fb.color[i]->full_width_frame = g_s.present_count;
                        fb.color[i]->full_width_since_present = true;
                    }
        }
    }
    pc.base_vertex = p.base_vertex;
    pc.base_instance = p.base_instance - p.instance;
    for (u32 i = 0; i < fb.num_colors; i++)
        if (fb.color_view[i]) pc.rt_mask |= 1u << i;
    pc.picture[0] = settings::video().contrast / 100.0f;  // read only by the final tonemap pass (kPictureShaders)
    pc.picture[1] = settings::video().bloom / 100.0f;
    if (rs_.pc_layout != prog->layout || memcmp(&rs_.pc, &pc, sizeof(pc)) != 0) {
        r.flags |= DrawRec::kPush;
        r.pc = pc;
        rs_.pc_layout = prog->layout;
        rs_.pc = pc;
    }
    if (indexed) {
        if (rs_.ib != ibuf || rs_.io != ioff || rs_.it != itype) {
            r.flags |= DrawRec::kIndexBuffer;
            r.ib = ibuf;
            r.io = ioff;
            r.it = itype;
            rs_.ib = ibuf;
            rs_.io = ioff;
            rs_.it = itype;
        }
        r.flags |= DrawRec::kIndexed;
    }
    r.layout = prog->layout;
    r.count = count;
    r.first = first_vertex;
    r.vertex_offset = vertex_offset;
    r.base_instance = p.base_instance;
    g_rec.record([r](VkCommandBuffer cb) { r.exec(cb); });
    gputime::count_draw(prog->stages[4] ? prog->stages[4]->key : 0);
    for (u32 i = 0; i < fb.num_colors; i++)
        if (fb.color[i] && key.color_mask[i]) tc_.mark_modified(fb.color[i]);
    if (fb.depth && (key.depth_write || key.stencil_enable)) tc_.mark_modified(fb.depth);
    for (auto& f : post) f();
    if ((settings::video().fxaa || settings::video().sharpen > 0) && prog->stages[4] && fb.color[0])
        for (const PictureShader& ps : kPictureShaders)
            if (prog->stages[4]->key == ps.key) post_picture(fb.color[0]);
}

void VkRenderer::do_draw(const u32* regs, const DrawParams& p, const void* inline_idx, u32 inline_size) {
    draws_++;
    st_.draws++;
    if (!p.count) return;
    if (!warmup_first_draw_) {
        warmup_first_draw_ = true;
        log_warmup("before_first_live_draw");
    }
    u64 prog_key = 0;
    g_prof_draws++;
    GfxProgram* prog;
    {
        ProfScope ps(kProfProgram);
        prog = get_program(regs, &prog_key);
    }
    if (!prog || !prog->ok) {
        skipped_++;
        st_.skipped++;
        VK_LOG_ONCE("vk: draw skipped (no usable program)");
        return;
    }
    std::optional<ProfScope> phase(std::in_place, kProfFramebuffer);
    // The render-target registers rarely change between draws: reuse the last resolved framebuffer
    // while they (and the texture cache's image set) are unchanged. Compared word for word (cheaper than hashing them).
    u32 fbregs[kFbKeyWords];
    memcpy(fbregs, regs + RenderTarget, 8 * 16 * 4);
    memcpy(fbregs + 128, regs + Zeta, 5 * 4);
    memcpy(fbregs + 133, regs + ZetaSize, 3 * 4);
    fbregs[136] = regs[RtControl];
    fbregs[137] = regs[ZetaEnable];
    Framebuffer fb;
    if (fb_cache_valid_ && fb_cache_gen_ == tc_.generation && memcmp(fb_cache_regs_, fbregs, sizeof(fbregs)) == 0) {
        fb = fb_cache_;
    } else {
        fb = resolve_framebuffer(regs, false, 0);
        fb_cache_ = fb;
        memcpy(fb_cache_regs_, fbregs, sizeof(fbregs));
        fb_cache_gen_ = tc_.generation;
        fb_cache_valid_ = true;
    }

    phase.reset();
    phase.emplace(kProfKeyBuild);
    DrawCtx d;
    build_pipeline_key(regs, p, inline_idx, prog_key, fb, prog, d);
    PipelineKey& key = d.key;
    u32& topo = d.topo;
    u32& attrs_used = d.attrs_used;

    if (!read_indices(regs, p, inline_idx, inline_size, d)) return;
    bool& indexed = d.indexed;
    u32& count = d.count;

    phase.reset();
    phase.emplace(kProfVertex);
    stage_vertex_input(regs, p, d);

    phase.reset();
    phase.emplace(kProfIndex);
    stage_indices(p, inline_idx, d);

    phase.reset();
    phase.emplace(kProfDescriptors);
    // ---- descriptor sets (may upload textures: before rendering begins)
    VkDescriptorSet sets[5] = {};
    std::vector<std::function<void()>> post;
    draw_samples_rt_ = false;
    u32 pools[6] = {regs[TexHeader], regs[TexHeader + 1], regs[TexHeader + 2],
                    regs[TexSampler], regs[TexSampler + 1], regs[TexSampler + 2]};
    bool linked = regs[SamplerBinding] == 1;
    {
        u64 kw[5] = {prog_key, addr(regs, VertexStreams + 1), addr(regs, IndexBuffer), ((u64)p.count << 32) | p.first, p.topology};
        cur_draw_key_ = hash_bytes(kw, sizeof(kw), 0x1e1e);
    }
    for (u32 s = 0; s < 5; s++) {
        if (!prog->stages[s]) continue;
        StageLayout* sl = prog->layouts[s];
        if (sl->types.empty()) continue;
        cur_stage_ = (int)s;  // s is also the stage's bind group; bind_resources allocates (or reuses) sets[s]
        bind_resources(prog->stages[s]->prog.resources, sl, sets[s], cbufs_[s], 18, pools, linked, post);
    }

    phase.reset();
    phase.emplace(kProfPipeline);
    VkPipeline pipe = get_pipeline(prog, key);
    phase.reset();
    phase.reset();
    if (!pipe) {
        skipped_++;
        st_.skipped++;
        {   // diagnostics: which draws a pending pipeline hides, and whether they read render targets
            ++audit_skips_by_program_[prog_key];
            if (++audit::skipped_draws == 1) {
                hw_log("vk: async pipeline: skipped draw (present %llu, prog %016llx, rt0 %llx %ux%u, samples rt %d)",
                       (unsigned long long)g_s.present_count, (unsigned long long)prog_key,
                       fb.color[0] ? (unsigned long long)fb.color[0]->addr : 0ull, fb.width, fb.height, (int)draw_samples_rt_);
            }
        }
        return;
    }
    ProfScope record_scope(kProfRecord);
    if (!dump_prog_.empty()) {
        // HWDER_VK_FRAME_DUMP_AFTER=<secs>: ignore matches before that run time (the same UI program is used
        // on the loading screen at ~10 s and on the Select Mode menu at ~22 s).
        static const double after = debug::value(debug::VkFrameDumpAfter, 0.0);
        char kb[20];
        snprintf(kb, sizeof(kb), "%016llx", (unsigned long long)prog_key);
        if (host::ticks_ms() - g_start_ms >= (u64)(after * 1000.0) && !strncmp(kb, dump_prog_.c_str(), dump_prog_.size())) {
            dump_frame_ = g_s.present_count + 2;  // the next full present
            hw_log("fd: program %s drawn (draw #%llu); dumping from present %llu", kb, (unsigned long long)draws_, (unsigned long long)dump_frame_);
            dump_prog_.clear();
        }
    }
    if (drawlog_active()) {
        const float* vt = (const float*)(regs + ViewportTransform);
        const u32* sc = regs + ScissorTest;
        fprintf(g_drawlog, "%llu %llu prog %016llx topo %u count %u rt0 %llx %ux%u f%d rts %u depth %llx t%d w%d | blend %d mask %x | vp %.0f,%.0f %.0f,%.0f | sc %d %u-%u,%u-%u |%s\n",
                (unsigned long long)g_s.present_count, (unsigned long long)draws_, (unsigned long long)prog_key, topo, count,
                fb.color[0] ? (unsigned long long)fb.color[0]->addr : 0ull, fb.width, fb.height, (int)fb.color_format[0], fb.num_colors,
                fb.depth ? (unsigned long long)fb.depth->addr : 0ull, (int)key.depth_test, (int)key.depth_write, (int)key.blend_enable[0],
                key.color_mask[0], vt[0], vt[1], vt[3], vt[4], (int)sc[0], sc[1] & 0xFFFF, sc[1] >> 16, sc[2] & 0xFFFF, sc[2] >> 16,
                drawlog_tex_.c_str());
    }
    drawlog_tex_.clear();
    if (dumping()) {
        const float* vt = (const float*)(regs + ViewportTransform);
        const u32* sc = regs + ScissorTest;
        u32 rt0_slot = (regs[RtControl] >> 4) & 7;
        hw_log("fd: draw #%llu prog %016llx topo %u count %u | rt0 %llx %ux%u f%d raw %02x (rts %u) depth %llx f%d test %d write %d func %d | blend0 %d mask0 %x | "
               "vt scale %.0f,%.0f tr %.0f,%.0f swz %x origin %x clip %ux%u scissor %d %u-%u,%u-%u cull %d%s"
               " | blendregs %x:%x,%x,%x/%x,%x,%x pt %x alpha %u/%x/%.3f logic %x pt0 %x:%x,%x,%x/%x,%x,%x",
               (unsigned long long)draws_, (unsigned long long)prog_key, topo, count, fb.color[0] ? (unsigned long long)fb.color[0]->addr : 0ull,
               fb.width, fb.height, (int)fb.color_format[0], regs[RenderTarget + rt0_slot * 16 + 4] & 0xFF, fb.num_colors,
               fb.depth ? (unsigned long long)fb.depth->addr : 0ull,
               (int)fb.depth_format, (int)key.depth_test, (int)key.depth_write, (int)key.depth_func, (int)key.blend_enable[0],
               key.color_mask[0], vt[0], vt[1], vt[3], vt[4], regs[ViewportTransform + 6], regs[WindowOrigin], regs[0x3FD] & 0xFFFF,
               regs[0x3FD] >> 16, (int)sc[0], sc[1] & 0xFFFF, sc[1] >> 16, sc[2] & 0xFFFF, sc[2] >> 16, (int)key.cull_enable,
               dump_tex_.c_str(),
               // raw Maxwell blend state: separate-alpha flag : rgb op,src,dst / alpha op,src,dst; per-target enable;
               // alpha test enable/func/ref; logic op enable
               regs[Blend], regs[Blend + 1], regs[Blend + 2], regs[Blend + 3], regs[Blend + 4], regs[Blend + 5], regs[Blend + 7],
               regs[BlendPerTargetEnabled], regs[AlphaTestEnable], regs[AlphaTestFunc], *(const float*)&regs[AlphaTestRef], regs[LogicOp],
               regs[BlendPerTarget], regs[BlendPerTarget + 1], regs[BlendPerTarget + 2], regs[BlendPerTarget + 3], regs[BlendPerTarget + 4],
               regs[BlendPerTarget + 5], regs[BlendPerTarget + 6]);
        dump_tex_.clear();
        // Fragment-stage constant buffers actually used by the program: slot, bound size, first floats.
        if (prog->stages[4]) {
            std::string cb;
            for (const auto& c : prog->stages[4]->prog.resources.const_buffers) {
                const CbufBinding& bnd = cbufs_[4][c.index < 18 ? c.index : 0];
                char t[200];
                float f[4] = {};
                if (bnd.valid) mm_->read(bnd.va, f, 16);
                snprintf(t, sizeof(t), " c%u[%s size 0x%x use 0x%x: %.3g %.3g %.3g %.3g]", c.index, bnd.valid ? "ok" : "UNBOUND", bnd.size,
                         c.size, f[0], f[1], f[2], f[3]);
                cb += t;
                // HWDER_VK_DUMP_CBUF=<frag shader key prefix>: full contents of that shader's cbufs (8 floats per line).
                static const char* dump_cbuf = debug::str(debug::VkDumpCbuf);
                if (dump_cbuf && bnd.valid && prog->stages[4]) {
                    char kbuf[20];
                    snprintf(kbuf, sizeof(kbuf), "%016llx", (unsigned long long)prog->stages[4]->key);
                    if (!strncmp(kbuf, dump_cbuf, strlen(dump_cbuf))) {
                        u32 n = std::min<u32>(std::min<u32>(c.size, bnd.size), 0x1000) / 4;
                        std::vector<float> v(n);
                        mm_->read(bnd.va, v.data(), n * 4);
                        for (u32 i = 0; i < n; i += 8) {
                            std::string line;
                            for (u32 j = i; j < std::min(n, i + 8); j++) {
                                char fs[32];
                                snprintf(fs, sizeof(fs), " %.4g", v[j]);
                                line += fs;
                            }
                            hw_log("fd:     c%u[0x%03x]:%s", c.index, i * 4, line.c_str());
                        }
                    }
                }
            }
            hw_log("fd:   frag shader %016llx cbufs:%s | vert %016llx | cull en %u face %x front %x origin %x", (unsigned long long)prog->stages[4]->key, cb.c_str(),
                   (unsigned long long)(prog->stages[0] ? prog->stages[0]->key : 0), regs[GlCullTestEnabled], regs[GlCullFace], regs[GlFrontFace], regs[WindowOrigin]);
        }
        // Vertex-stage constant buffers and the raw formats of the attributes the vertex program reads.
        if (prog->stages[0]) {
            std::string cb;
            for (const auto& c : prog->stages[0]->prog.resources.const_buffers) {
                const CbufBinding& bnd = cbufs_[0][c.index < 18 ? c.index : 0];
                char t[200];
                float f[4] = {};
                if (bnd.valid) mm_->read(bnd.va, f, 16);
                snprintf(t, sizeof(t), " c%u[%s size 0x%x use 0x%x: %.3g %.3g %.3g %.3g]", c.index, bnd.valid ? "ok" : "UNBOUND", bnd.size,
                         c.size, f[0], f[1], f[2], f[3]);
                cb += t;
                static const bool fd_raw = debug::flag(debug::FdRaw);
                if (fd_raw && bnd.valid && bnd.size) {  // raw vertex-stage cbuf for offline analysis
                    std::vector<u8> buf(std::min<u32>(bnd.size, 0x10000));
                    mm_->read(bnd.va, buf.data(), buf.size());
                    char path[128];
                    snprintf(path, sizeof(path), "fd/cb_%llu_c%u.bin", (unsigned long long)draws_, c.index);
                    if (FILE* fo = fopen(path, "wb")) {
                        fwrite(buf.data(), 1, buf.size(), fo);
                        fclose(fo);
                    }
                }
            }
            std::string at;
            for (u32 a = 0; a < 32; a++) {
                if (!(attrs_used & (1u << a))) continue;
                char t[64];
                snprintf(t, sizeof(t), " a%u[raw %08x s%u off %u vkf %d]", a, regs[VertexAttribFormat + a], regs[VertexAttribFormat + a] & 0x1F,
                         (regs[VertexAttribFormat + a] >> 7) & 0x3FFF, (int)key.attrib_format[a]);
                at += t;
            }
            hw_log("fd:   vert cbufs:%s | attrs:%s | inst %u base_vertex %u first %u", cb.c_str(), at.c_str(), p.base_instance, p.base_vertex, p.first);
        }
        for (u32 i = 0; i < fb.num_colors; i++)
            if (fb.color[i] && std::find(dump_rts_.begin(), dump_rts_.end(), fb.color[i]) == dump_rts_.end())
                dump_rts_.push_back(fb.color[i]);
        if (fb.depth && std::find(dump_rts_.begin(), dump_rts_.end(), fb.depth) == dump_rts_.end()) dump_rts_.push_back(fb.depth);
    }
    if (verbose_ && st_.draws < 400)
        hw_log("vk: draw #%llu topo %u %s count %u inst %u rts %u (%ux%u fmt0 %d) depth %d prog %016llx", (unsigned long long)draws_,
               topo, indexed ? "indexed" : "arrays", count, p.base_instance, fb.num_colors, fb.width, fb.height,
               (int)fb.color_format[0], (int)fb.depth_format, (unsigned long long)prog_key);

    record_draw(regs, p, fb, prog, pipe, sets, post, d);
}

void VkRenderer::do_clear(const u32* regs, u32 v) {
    st_.clears++;
    u32 rt = clear_rt(v);
    bool color = clear_rgba_mask(v) != 0;
    bool z = clear_z(v), s = clear_s(v);
    Framebuffer fb = resolve_framebuffer(regs, color, rt);
    std::vector<VkClearAttachment> att;
    if (color && rt < fb.num_colors && fb.color_view[rt]) {
        VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, rt};
        memcpy(&a.clearValue.color, &regs[ClearColor], 16);
        if (clear_rgba_mask(v) != 0xF) VK_LOG_ONCE("vk: partial color clear mask 0x%x ignored", clear_rgba_mask(v));
        att.push_back(a);
        tc_.mark_modified(fb.color[rt], true);
    }
    if ((z || s) && fb.depth_view) {
        VkClearAttachment a{};
        if (z) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (s && fb.has_stencil) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        float d;
        memcpy(&d, &regs[ClearDepth], 4);
        a.clearValue.depthStencil = {std::clamp(d, 0.0f, 1.0f), regs[ClearStencil] & 0xFF};
        if (a.aspectMask) {
            att.push_back(a);
            tc_.mark_modified(fb.depth);
        }
    }
    if (dumping())
        hw_log("fd: clear 0x%x rt %u -> %llx %ux%u f%d depth %llx", v, rt, fb.color[rt < 8 ? rt : 0] ? (unsigned long long)fb.color[rt < 8 ? rt : 0]->addr : 0ull,
               fb.width, fb.height, rt < fb.num_colors ? (int)fb.color_format[rt] : -1, fb.depth ? (unsigned long long)fb.depth->addr : 0ull);
    if (verbose_ && st_.clears < 100) {
        float c[4];
        memcpy(c, &regs[ClearColor], 16);
        hw_log("vk: clear 0x%x rt %u (%ux%u, %u att) color %.2f %.2f %.2f %.2f", v, rt, fb.width, fb.height,
               (u32)att.size(), c[0], c[1], c[2], c[3]);
    }
    if (att.empty()) return;
    VkClearRect r{{{0, 0}, {fb.width, fb.height}}, 0, 1};
    u32 ctrl = regs[0x43E];
    const u32* st = regs + ScissorTest;
    if ((ctrl & 0x100) && st[0]) {
        s32 x0 = st[1] & 0xFFFF, x1 = st[1] >> 16, y0 = st[2] & 0xFFFF, y1 = st[2] >> 16;
        if (regs[WindowOrigin] & 1) {
            s32 clip_h = (s32)(regs[0x3FD] >> 16);
            if (!clip_h) clip_h = (s32)(fb.height / fb.scale_y + 0.5f);
            s32 ny0 = clip_h - y1, ny1 = clip_h - y0;
            y0 = std::max(0, ny0);
            y1 = std::max(0, ny1);
        }
        if (fb.scale != 1.0f || fb.scale_y != 1.0f) {
            x0 = (s32)(x0 * fb.scale);
            x1 = (s32)(x1 * fb.scale + 0.5f);
            y0 = (s32)(y0 * fb.scale_y);
            y1 = (s32)(y1 * fb.scale_y + 0.5f);
        }
        x0 = std::clamp(x0, 0, (s32)fb.width);
        x1 = std::clamp(x1, x0, (s32)fb.width);
        y0 = std::clamp(y0, 0, (s32)fb.height);
        y1 = std::clamp(y1, y0, (s32)fb.height);
        r.rect = {{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    }
    if (!r.rect.extent.width || !r.rect.extent.height) return;
    if (color && rt < fb.num_colors && fb.color[rt] && r.rect.offset.x == 0 && r.rect.extent.width == fb.width) {
        fb.color[rt]->full_width_frame = g_s.present_count;
        fb.color[rt]->full_width_since_present = true;
    }
    // A colour clear of a whole single-level, single-layer image is a load-op clear in COLOR_ATTACHMENT_OPTIMAL
    // layout: the driver's fast clear (metadata only), which images in GENERAL layout never get. The game's full-
    // screen G-buffer and effect clears were ~0.6 ms of GPU time per frame at the busy spot. (Depth clears gained
    // nothing this way and stay as they are.)
    if (att.size() == 1 && att[0].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
        const Image* img = fb.color[rt];
        if (img && img->info.levels == 1 && img->info.layers == 1 && img->info.type == ImageType::e2D && r.rect.offset.x == 0 &&
            r.rect.offset.y == 0 && r.rect.extent.width == img->level_width(0) && r.rect.extent.height == img->level_height(0)) {
            fast_clear_color(fb.color[rt], fb.color_view[rt], att[0].clearValue.color);
            gputime::count_clear();
            return;
        }
    }
    begin_rendering(fb);
    struct {
        VkClearAttachment a[2];
        u32 n;
        VkClearRect r;
    } c{};
    c.n = (u32)std::min<size_t>(att.size(), 2);
    for (u32 i = 0; i < c.n; i++) c.a[i] = att[i];
    c.r = r;
    g_rec.record([c](VkCommandBuffer cb) { vkCmdClearAttachments(cb, c.n, c.a, 1, &c.r); });
    gputime::count_clear();
}

void VkRenderer::fast_clear_color(Image* img, VkImageView view, const VkClearColorValue& color) {
    g_rec.cmd();  // ends any rendering instance (with its barrier)
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_rec.record([b](VkCommandBuffer cb) {
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    VkRenderingAttachmentInfo ai{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ai.imageView = view;
    ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ai.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ai.clearValue.color = color;
    const VkExtent2D ext{img->level_width(0), img->level_height(0)};
    gputime::mark(gputime::kPass, ext.width, ext.height, (u32)img->info.fmt.vk, 0, 1);
    g_rec.record([ai, ext](VkCommandBuffer cb) {
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, ext};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ai;
        vkCmdBeginRendering(cb, &ri);
        vkCmdEndRendering(cb);
    });
    gputime::mark(gputime::kOther);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_rec.record([b](VkCommandBuffer cb) {
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
}

// ------------------------------------------------------------------ compute
void VkRenderer::do_dispatch(const u32* cr, u64 qmd_va) {
    st_.dispatches++;
    u32 q[64];
    mm_->read(qmd_va, q, sizeof(q));
    u64 code_va = (((u64)cr[0x582] << 32) | cr[0x583]) + q[8];
    u8* code = host(code_va);
    if (!code) return;
    u64 cb_va[8] = {};
    u32 cb_size[8] = {};
    u32 mask = q[20] & 0xFF;
    for (u32 i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        cb_va[i] = q[29 + i * 2] | ((u64)(q[30 + i * 2] & 0xFF) << 32);
        cb_size[i] = q[30 + i * 2] >> 15;
    }
    auto cbuf = [this, cb_va, cb_size](u32 index, u32 offset) -> u32 {
        if (index >= 8 || !cb_va[index] || offset + 4 > cb_size[index]) return 0;
        return read_u32(cb_va[index] + offset);
    };
    GuestEnv env;
    env.graphics = false;
    env.code = code;
    env.code_avail = mm_->contiguous_size(code_va, 1 << 20);
    env.cbuf = cbuf;
    env.bound_buffer = cr[0x982];
    env.lsize[0] = std::max(1u, q[18] >> 16);
    env.lsize[1] = std::max(1u, q[19] & 0xFFFF);
    env.lsize[2] = std::max(1u, q[19] >> 16);
    env.smem = q[17] & 0x3FFFF;
    bool linked = (q[11] >> 30) & 1;
    env.tex_type = [this, cr](u32 handle) -> shader::TextureType {
        u32 tic[8];
        if (!read_tic(((u64)cr[0x55D] << 32) | cr[0x55E], cr[0x55F], handle & 0xFFFFF, tic))
            return shader::TextureType::Tex2D;
        switch ((tic[4] >> 23) & 0xF) {
        case 0: return shader::TextureType::Tex1D;
        case 2: return shader::TextureType::Tex3D;
        case 3: return shader::TextureType::Cube;
        case 4: return shader::TextureType::Tex1DArray;
        case 5: return shader::TextureType::Tex2DArray;
        case 6: return shader::TextureType::Buffer;
        case 8: return shader::TextureType::CubeArray;
        default: return shader::TextureType::Tex2D;
        }
    };
    u64 h = shader::hash_program(env, shader::Stage::Compute);
    u64 key = hash_bytes(env.lsize, sizeof(env.lsize), h ^ env.smem);
    ComputeProgram* cp;
    auto it = compute_.find(key);
    if (it != compute_.end()) {
        cp = it->second;
    } else {
        cp = new ComputeProgram;
        compute_[key] = cp;
        shader::Options opt;
        cp->sh = get_shader(env, shader::Stage::Compute, opt, nullptr);
        if (cp->sh->ok) {
            cp->layout = stage_layout(cp->sh->prog.resources, VK_SHADER_STAGE_COMPUTE_BIT);
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &cp->layout->layout;
            VK_CHECK(vkCreatePipelineLayout(dev(), &pli, nullptr, &cp->pl));
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = cp->sh->module;
            ci.stage.pName = "main";
            ci.layout = cp->pl;
            if (vkCreateComputePipelines(dev(), pcache_, 1, &ci, nullptr, &cp->pipe) != VK_SUCCESS) {
                hw_log("vk: compute pipeline creation failed");
                cp->pipe = VK_NULL_HANDLE;
            }
        }
    }
    if (!cp->pipe) {
        VK_LOG_ONCE("vk: dispatch skipped (no usable compute program)");
        return;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::vector<std::function<void()>> post;
    if (!cp->layout->types.empty()) {
        set = alloc_set(cp->layout->layout);
        u32 pools[6] = {cr[0x55D], cr[0x55E], cr[0x55F], cr[0x557], cr[0x558], cr[0x559]};
        CbufBinding cbs[8];
        for (u32 i = 0; i < 8; i++) cbs[i] = {cb_va[i], cb_size[i], cb_va[i] != 0};
        bind_resources(cp->sh->prog.resources, cp->layout, set, cbs, 8, pools, linked, post);
    }
    g_rec.cmd();
    const VkPipeline pipe = cp->pipe;
    const VkPipelineLayout pl = cp->pl;
    const u32 gx = std::max(1u, q[12] & 0x7FFFFFFF), gy = std::max(1u, q[13] & 0xFFFF), gz = std::max(1u, q[13] >> 16);
    g_rec.record([pipe, pl, set, gx, gy, gz](VkCommandBuffer cb) {
        full_barrier(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cb, gx, gy, gz);
        full_barrier(cb);
    });
    for (auto& f : post) f();
}

// ------------------------------------------------------------------ 2D blit
bool VkRenderer::do_blit(const u32* r) {
    st_.blits++;
    auto surface = [&](u32 base, ImageInfo& info, u64& a) -> bool {
        const u32* s = r + base;
        info = ImageInfo{};
        info.fmt = rt_format(s[0]);
        if (!info.fmt.valid()) {
            info.fmt = zeta_format(s[0]);
            if (!info.fmt.valid()) return false;
        }
        info.render_target = true;
        info.block_linear = s[1] == 0;
        info.bh = (s[2] >> 4) & 0xF;
        info.bd = (s[2] >> 8) & 0xF;
        info.width = std::max(1u, s[6]);
        info.height = std::max(1u, s[7]);
        if (!info.block_linear) info.pitch = s[5];
        u64 va = ((u64)s[8] << 32) | s[9];
        u8* p = va ? host(va) : nullptr;
        if (!p) return false;
        a = (u64)p;
        return true;
    };
    ImageInfo si, di;
    u64 sa, da;
    if (!surface(0x8C, si, sa) || !surface(0x80, di, da)) {
        VK_LOG_ONCE("vk: 2d blit with unsupported surface (src fmt 0x%x dst fmt 0x%x)", r[0x8C], r[0x80]);
        return false;
    }
    s64 du_dx = (s64)(((u64)r[0x231] << 32) | r[0x230]);
    s64 dv_dy = (s64)(((u64)r[0x233] << 32) | r[0x232]);
    s64 src_x0 = (s64)(((u64)r[0x235] << 32) | r[0x234]);
    s64 src_y0 = (s64)(((u64)r[0x237] << 32) | r[0x236]);
    s32 dx0 = (s32)r[0x22C], dy0 = (s32)r[0x22D], dw = (s32)r[0x22E], dh = (s32)r[0x22F];
    bool corner = r[0x223] & 1;
    bool linear = (r[0x223] >> 4) & 1;
    if (corner) {
        src_x0 -= (du_dx >> 33) << 32;
        src_y0 -= (dv_dy >> 33) << 32;
    }
    s32 sx0 = (s32)(src_x0 >> 32), sy0 = (s32)(src_y0 >> 32);
    s32 sx1 = (s32)((src_x0 + du_dx * dw) >> 32), sy1 = (s32)((src_y0 + dv_dy * dh) >> 32);
    Subresource ss, ds;
    Image* src = tc_.get(sa, si, &ss);
    Image* dst = tc_.get(da, di, &ds);
    if (src == dst) {
        VK_LOG_ONCE("vk: 2d blit within one image");
    }
    VkImageBlit b{};
    b.srcSubresource = {src->aspect, ss.level, ss.layer, 1};
    b.dstSubresource = {dst->aspect, ds.level, ds.layer, 1};
    if (src->scale != 1.0f || src->scale_y != 1.0f) {
        sx0 = (s32)(sx0 * src->scale);
        sy0 = (s32)(sy0 * src->scale_y);
        sx1 = (s32)(sx1 * src->scale + 0.5f);
        sy1 = (s32)(sy1 * src->scale_y + 0.5f);
    }
    if (dst->scale != 1.0f || dst->scale_y != 1.0f) {
        dx0 = (s32)(dx0 * dst->scale);
        dy0 = (s32)(dy0 * dst->scale_y);
        dw = (s32)(dw * dst->scale + 0.5f);
        dh = (s32)(dh * dst->scale_y + 0.5f);
    }
    auto clampx = [](s32 v, u32 m) { return std::clamp(v, 0, (s32)m); };
    b.srcOffsets[0] = {clampx(sx0, src->level_width(ss.level)), clampx(sy0, src->level_height(ss.level)), 0};
    b.srcOffsets[1] = {clampx(sx1, src->level_width(ss.level)), clampx(sy1, src->level_height(ss.level)), 1};
    b.dstOffsets[0] = {clampx(dx0, dst->level_width(ds.level)), clampx(dy0, dst->level_height(ds.level)), 0};
    b.dstOffsets[1] = {clampx(dx0 + dw, dst->level_width(ds.level)), clampx(dy0 + dh, dst->level_height(ds.level)), 1};
    if (b.srcOffsets[0].x == b.srcOffsets[1].x || b.srcOffsets[0].y == b.srcOffsets[1].y ||
        b.dstOffsets[0].x == b.dstOffsets[1].x || b.dstOffsets[0].y == b.dstOffsets[1].y)
        return true;
    if ((src->aspect != dst->aspect) || (src->info.fmt.integer != dst->info.fmt.integer)) {
        VK_LOG_ONCE("vk: 2d blit between incompatible formats");
        return true;
    }
    bool depth = src->aspect != VK_IMAGE_ASPECT_COLOR_BIT;
    if (dumping())
        hw_log("fd: blit %llx %ux%u f%d [%d,%d-%d,%d] -> %llx %ux%u f%d [%d,%d-%d,%d]", (unsigned long long)src->addr, src->info.width,
               src->info.height, (int)src->info.fmt.vk, b.srcOffsets[0].x, b.srcOffsets[0].y, b.srcOffsets[1].x, b.srcOffsets[1].y,
               (unsigned long long)dst->addr, dst->info.width, dst->info.height, (int)dst->info.fmt.vk, b.dstOffsets[0].x,
               b.dstOffsets[0].y, b.dstOffsets[1].x, b.dstOffsets[1].y);
    g_rec.cmd();
    const VkImage simg = src->image, dimg = dst->image;
    const VkFilter filter = linear && !depth && !src->info.fmt.integer ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    g_rec.record([simg, dimg, b, filter](VkCommandBuffer cb) {
        full_barrier(cb);
        vkCmdBlitImage(cb, simg, VK_IMAGE_LAYOUT_GENERAL, dimg, VK_IMAGE_LAYOUT_GENERAL, 1, &b, filter);
        full_barrier(cb);
    });
    tc_.mark_modified(dst);
    return true;
}

}  // namespace gpu::vk::detail
