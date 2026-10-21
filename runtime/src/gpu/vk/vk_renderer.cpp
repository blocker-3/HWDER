// Vulkan renderer: executes Maxwell 3D / compute / 2D work recorded by the GPU core.
//
// Design (simple and correct first):
//  * one command buffer is recorded at a time; flush_commands() submits it and signals a timeline
//    semaphore with its serial. A waiter thread runs deferred callbacks (syncpoint increments,
//    semaphore releases, resource destruction) once their serial completes.
//  * every image lives in VK_IMAGE_LAYOUT_GENERAL; synchronization uses full memory barriers between
//    rendering instances / transfers.
//  * uniform, vertex and index data are copied per draw into a host-visible ring (snapshot semantics
//    match the GPU's: constant buffer updates between draws are honored).
#include "gpu/settings.h"
#include "interp/camera.h"
#include "interp/interp.h"
#include <unordered_set>
#include <deque>
#include <windows.h>

#include <array>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <chrono>
#include <memory>
#include <thread>

#include "shader/shader.h"
#include "gpu/gpu.h"
#include "gpu/graphics_audit.h"
#include "gpu/vk/pipeline_warmup_cache.h"
#include "gpu/vk/catalog_game_sources.h"
#include "gpu/prelaunch.h"
#include "fs/nsp.h"
#include "vk_common.h"
#include "post_spv.h"
#include "vk_renderer.h"

namespace gpu {
namespace vk {

using namespace m3d;

// ------------------------------------------------------------------ basics
u32 find_memory_type(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    const auto& mp = display::device().mem;
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want &&
            !(mp.memoryTypes[i].propertyFlags & avoid))
            return i;
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

Buffer create_buffer(u64 size, VkBufferUsageFlags usage, bool host_visible, bool prefer_device_local, bool cached) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VK_CHECK(vkCreateBuffer(dev(), &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev(), b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    VkMemoryPropertyFlags want = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                              : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (host_visible && cached) {
        VkMemoryPropertyFlags want_c = want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, want_c, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((display::device().mem.memoryTypes[ai.memoryTypeIndex].propertyFlags & want_c) == want_c)
            r = vkAllocateMemory(dev(), &ai, nullptr, &b.mem);
    }
    if (r != VK_SUCCESS && host_visible && prefer_device_local) {
        ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (display::device().mem.memoryTypes[ai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            r = vkAllocateMemory(dev(), &ai, nullptr, &b.mem);
    }
    if (r != VK_SUCCESS) {
        ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, want,
                                              host_visible ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0);
        VK_CHECK(vkAllocateMemory(dev(), &ai, nullptr, &b.mem));
    }
    VK_CHECK(vkBindBufferMemory(dev(), b.buf, b.mem, 0));
    if (host_visible) VK_CHECK(vkMapMemory(dev(), b.mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.map));
    return b;
}

void destroy_buffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev(), b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev(), b.mem, nullptr);
    b = Buffer{};
}

void decode_astc(const u8* src, u32 width, u32 height, u32 depth, u32 bw, u32 bh, u8* dst);

}  // namespace vk

// ================================================================== renderer
namespace vk::detail {

// Draws in the last presented frame (interp/interp.h: read by the HWDER_AUTOPLAY=2 input script to tell battle from menus).
extern "C" std::atomic<u32> hw_last_frame_draws{0};

// HWDER_DRAWLOG=1 (with HWDER_FILMSTRIP): one line per draw into <filmstrip dir>/drawlog.txt while the
// filmstrip capture window is open, keyed by present number, to find the draw behind a bad frame.
static bool g_drawlog_window = false;
static FILE* g_drawlog = nullptr;
bool drawlog_active() {
    static const bool on = getenv("HWDER_DRAWLOG") && getenv("HWDER_FILMSTRIP");
    if (!on || !g_drawlog_window) return false;
    if (!g_drawlog) {
        std::string path = std::string(getenv("HWDER_FILMSTRIP")) + "/drawlog.txt";
        g_drawlog = fopen(path.c_str(), "w");
    }
    return g_drawlog != nullptr;
}

static const u64 g_start_ms = GetTickCount64();

using gpu::vk::g_prof;
using gpu::vk::g_prof_draws;
double prof_flush() {
    double total = 0;
    for (double v : g_prof) total += v;
    static const double limit = getenv("HWDER_PROFILE_MS") ? atof(getenv("HWDER_PROFILE_MS")) : 200.0;
    if (total > limit) {
        char line[512];
        int o = snprintf(line, sizeof(line), "vk: profile: %u draws, %.0f ms:", g_prof_draws, total);
        for (int i = 0; i < kProfCount; i++) o += snprintf(line + o, sizeof(line) - o, " %s %.0f", kProfNames[i], g_prof[i]);
        hw_log("%s", line);
    }
    memset(g_prof, 0, sizeof(g_prof));
    g_prof_draws = 0;
    return total;
}

// ------------------------------------------------------------------ draw
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
    ProfScope* phase = new ProfScope(kProfFramebuffer);
    // The render-target registers rarely change between draws: reuse the last resolved framebuffer
    // while they (and the texture cache's image set) are unchanged.
    u64 fbkey = hash_bytes(regs + RenderTarget, 8 * 16 * 4, regs[RtControl]);
    fbkey = hash_bytes(regs + Zeta, 5 * 4, fbkey);
    fbkey = hash_bytes(regs + ZetaSize, 3 * 4, fbkey ^ regs[ZetaEnable]);
    Framebuffer fb;
    if (fb_cache_valid_ && fb_cache_key_ == fbkey && fb_cache_gen_ == tc_.generation) {
        fb = fb_cache_;
    } else {
        fb = resolve_framebuffer(regs, false, 0);
        fb_cache_ = fb;
        fb_cache_key_ = fbkey;
        fb_cache_gen_ = tc_.generation;
        fb_cache_valid_ = true;
    }

    delete phase;
    phase = new ProfScope(kProfKeyBuild);
    // ---- pipeline key
    PipelineKey key;
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
    static const bool no_cull = getenv("HWDER_VK_NO_CULL") != nullptr;
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
    static const char* face_rule = getenv("HWDER_VK_FACE_RULE");
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
                return;
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
        if (!inline_idx) {
            u64 kw[3] = {(u64)(uintptr_t)idx_src, ((u64)p.count << 8) | (isize << 1) | (key.prim_restart ? 1 : 0), 0x1d1d};
            ikey = hash_bytes(kw, sizeof(kw), 0);
            isample = sample_hash(idx_src, (u64)p.count * isize);
            auto cit = idx_range_cache_.find(ikey);
            if (cit != idx_range_cache_.end() && cit->second.serial == g_rec.serial() && cit->second.sample == isample)
                cached = &cit->second;
        }
        if (cached) {
            st_.idx_hit++;
            vmin = cached->vmin;
            vmax = cached->vmax;
        } else {
            st_.idx_miss++;
            u32 restart = isize == 1 ? 0xFF : isize == 2 ? 0xFFFF : 0xFFFFFFFF;
            vmin = 0xFFFFFFFF;
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
        if (!count) return;
    }

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
    delete phase;
    phase = new ProfScope(kProfVertex);
    VkBuffer vbufs[32];
    VkDeviceSize voffs[32];
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
            u64 vsample = sample_hash(src, avail);
            auto vit = vb_cache_.find(vkey);
            if (vit != vb_cache_.end() && vit->second.serial == g_rec.serial() && vit->second.bytes == bytes &&
                vit->second.sample == vsample) {
                st_.vb_hit++;
                sl = vit->second.slice;
            } else {
                st_.vb_miss++;
                sl = g_rec.stage(std::max<u64>(bytes, 16), 16);
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
            static const bool fd_raw = getenv("HWDER_FD_RAW") != nullptr;
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

    delete phase;
    phase = new ProfScope(kProfIndex);
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
            u64 isample2 = sample_hash(idx_src, (u64)count * isize);
            auto iit = vb_cache_.find(ikey2);
            if (!inline_idx && iit != vb_cache_.end() && iit->second.serial == g_rec.serial() &&
                iit->second.bytes == (u64)count * isize && iit->second.sample == isample2) {
                sl = iit->second.slice;
            } else {
                sl = g_rec.stage((u64)count * isize, 16);
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

    delete phase;
    phase = new ProfScope(kProfDescriptors);
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
        {
            ProfScope aps(kProfDescAlloc);
            sets[s] = alloc_set(sl->layout);
        }
        u32 group = s;  // bind group of the stage
        auto cbuf = [this, group](u32 index, u32 offset) -> u32 {
            if (index >= 18) return 0;
            const CbufBinding& b = cbufs_[group][index];
            if (!b.valid || offset + 4 > b.size) return 0;
            return read_u32(b.va + offset);
        };
        auto range = [this, group](u32 index, u64& lo, u32& size) -> bool {
            if (index >= 18) return false;
            const CbufBinding& b = cbufs_[group][index];
            lo = b.va;
            size = b.size;
            return b.valid;
        };
        cur_stage_ = (int)s;
        bind_resources(prog->stages[s]->prog.resources, sl, sets[s], cbuf, range, pools, linked, post);
    }

    delete phase;
    phase = new ProfScope(kProfPipeline);
    VkPipeline pipe = get_pipeline(prog, key);
    delete phase;
    phase = nullptr;
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
        static const double after = getenv("HWDER_VK_FRAME_DUMP_AFTER") ? atof(getenv("HWDER_VK_FRAME_DUMP_AFTER")) : 0.0;
        char kb[20];
        snprintf(kb, sizeof(kb), "%016llx", (unsigned long long)prog_key);
        if (GetTickCount64() - g_start_ms >= (u64)(after * 1000.0) && !strncmp(kb, dump_prog_.c_str(), dump_prog_.size())) {
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
                static const char* dump_cbuf = getenv("HWDER_VK_DUMP_CBUF");
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
                static const bool fd_raw = getenv("HWDER_FD_RAW") != nullptr;
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

    // ---- record
    begin_rendering(fb);
    VkCommandBuffer cb = g_rec.cmd_in_pass();
    if (rs_.serial != g_rec.serial()) {
        rs_ = RecState{};
        rs_.serial = g_rec.serial();
    }
    if (rs_.pipe != pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        rs_.pipe = pipe;
    }
    for (u32 s = 0; s < 5; s++)
        if (sets[s]) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, prog->layout, s, 1, &sets[s], 0, nullptr);
    for (u32 s = 0; s < 16; s++)
        if ((streams_used & (1u << s)) && (rs_.vb[s] != vbufs[s] || rs_.vo[s] != voffs[s])) {
            vkCmdBindVertexBuffers(cb, s, 1, &vbufs[s], &voffs[s]);
            rs_.vb[s] = vbufs[s];
            rs_.vo[s] = voffs[s];
        }
    if (need_const && (rs_.vb[kConstBinding] != vbufs[kConstBinding] || rs_.vo[kConstBinding] != voffs[kConstBinding])) {
        vkCmdBindVertexBuffers(cb, kConstBinding, 1, &vbufs[kConstBinding], &voffs[kConstBinding]);
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
            vkCmdSetViewport(cb, 0, 1, &v);
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
            vkCmdSetScissor(cb, 0, 1, &sc);
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
            vkCmdSetDepthBias(cb, db[0], db[1], db[2]);
            memcpy(rs_.bias, db, sizeof(db));
        }
        float bc[4];
        memcpy(bc, &regs[BlendColor], 16);
        if (memcmp(rs_.bc, bc, 16) != 0) {
            vkCmdSetBlendConstants(cb, bc);
            memcpy(rs_.bc, bc, 16);
        }
        bool two = regs[StencilTwoSideEnable];
        u32 stv[6] = {regs[StencilFrontFuncMask], regs[StencilFrontMask], regs[StencilFrontRef],
                      two ? regs[StencilBackFuncMask] : regs[StencilFrontFuncMask],
                      two ? regs[StencilBackMask] : regs[StencilFrontMask], two ? regs[StencilBackRef] : regs[StencilFrontRef]};
        if (memcmp(rs_.st, stv, sizeof(stv)) != 0) {
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, stv[0]);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, stv[1]);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, stv[2]);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, stv[3]);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, stv[4]);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, stv[5]);
            memcpy(rs_.st, stv, sizeof(stv));
        }
        float lw;
        memcpy(&lw, &regs[LineWidthAliased], 4);
        float lwv = (lw > 0 && lw < 64) ? lw : 1.0f;
        if (rs_.lw != lwv) {
            vkCmdSetLineWidth(cb, lwv);
            rs_.lw = lwv;
        }
        float bmin, bmax;
        memcpy(&bmin, &regs[DepthBoundsMin], 4);
        memcpy(&bmax, &regs[DepthBoundsMax], 4);
        float bd[2] = {std::clamp(bmin, 0.0f, 1.0f), std::clamp(bmax, 0.0f, 1.0f)};
        if (memcmp(rs_.bounds, bd, sizeof(bd)) != 0) {
            vkCmdSetDepthBounds(cb, bd[0], bd[1]);
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
        vkCmdPushConstants(cb, prog->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        rs_.pc_layout = prog->layout;
        rs_.pc = pc;
    }
    if (indexed) {
        if (rs_.ib != ibuf || rs_.io != ioff || rs_.it != itype) {
            vkCmdBindIndexBuffer(cb, ibuf, ioff, itype);
            rs_.ib = ibuf;
            rs_.io = ioff;
            rs_.it = itype;
        }
        vkCmdDrawIndexed(cb, count, 1, 0, vertex_offset, p.base_instance);
    } else {
        vkCmdDraw(cb, count, 1, first_vertex, p.base_instance);
    }
    for (u32 i = 0; i < fb.num_colors; i++)
        if (fb.color[i] && key.color_mask[i]) tc_.mark_modified(fb.color[i]);
    if (fb.depth && (key.depth_write || key.stencil_enable)) tc_.mark_modified(fb.depth);
    for (auto& f : post) f();
    if ((settings::video().fxaa || settings::video().sharpen > 0) && prog->stages[4] && fb.color[0])
        for (const PictureShader& ps : kPictureShaders)
            if (prog->stages[4]->key == ps.key) post_picture(fb.color[0]);
}

void VkRenderer::post_picture(Image* img) {
    if (post_.failed) return;
    VkFormat f = img->info.fmt.vk;
    if (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB && f != VK_FORMAT_B8G8R8A8_UNORM &&
        f != VK_FORMAT_B8G8R8A8_SRGB && f != VK_FORMAT_A8B8G8R8_UNORM_PACK32 && f != VK_FORMAT_A8B8G8R8_SRGB_PACK32) {
        VK_LOG_ONCE("vk: picture post-process: unsupported target format");
        return;
    }
    if (!post_.pipe) {
        VkDescriptorSetLayoutBinding b[2] = {};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dci.bindingCount = 2;
        dci.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(dev(), &dci, nullptr, &post_.dsl));
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &post_.dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(dev(), &pli, nullptr, &post_.pl));
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = sizeof(kPostSpv);
        mci.pCode = kPostSpv;
        VkShaderModule mod;
        VK_CHECK(vkCreateShaderModule(dev(), &mci, nullptr, &mod));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = post_.pl;
        VkResult r = vkCreateComputePipelines(dev(), pcache_, 1, &ci, nullptr, &post_.pipe);
        vkDestroyShaderModule(dev(), mod, nullptr);
        if (r != VK_SUCCESS) {
            hw_log("vk: picture post-process pipeline creation failed (%d)", (int)r);
            post_.failed = true;
            return;
        }
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        VK_CHECK(vkCreateSampler(dev(), &sci, nullptr, &post_.sampler));
        hw_log("vk: picture post-process (FXAA / sharpening) ready");
    }
    u32 w = img->phys_w(), h = img->phys_h();
    if (post_.w != w || post_.h != h) {  // (re)create the copy at the picture's physical size
        if (post_.copy) {
            VkImage im = post_.copy;
            VkDeviceMemory m = post_.mem;
            VkImageView v = post_.copy_view;
            retire_resource([im, m, v] {
                vkDestroyImageView(dev(), v, nullptr);
                vkDestroyImage(dev(), im, nullptr);
                vkFreeMemory(dev(), m, nullptr);
            });
        }
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VK_CHECK(vkCreateImage(dev(), &ci, nullptr, &post_.copy));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev(), post_.copy, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(dev(), &ai, nullptr, &post_.mem));
        VK_CHECK(vkBindImageMemory(dev(), post_.copy, post_.mem, 0));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = post_.copy;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(dev(), &vci, nullptr, &post_.copy_view));
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = post_.copy;
        ib.subresourceRange = vci.subresourceRange;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(g_rec.cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &ib);
        post_.w = w;
        post_.h = h;
    }
    // The picture's bytes into the copy (same texel size, so a raw copy; R/B order stays consistent because
    // the pass also writes through an R8G8B8A8 view of the picture).
    g_rec.barrier();
    VkCommandBuffer cb = g_rec.cmd();
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {w, h, 1};
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, post_.copy, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    g_rec.barrier();
    VkDescriptorSet set = alloc_set(post_.dsl);
    VkDescriptorImageInfo ii[2] = {{post_.sampler, post_.copy_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, tc_.view(img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1),
                                    VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (u32 i = 0; i < 2; i++) {
        wr[i].dstSet = set;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev(), 2, wr, 0, nullptr);
    cb = g_rec.cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, post_.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, post_.pl, 0, 1, &set, 0, nullptr);
    float pc[4] = {1.0f / w, 1.0f / h, settings::video().fxaa ? 1.0f : 0.0f, settings::video().sharpen / 100.0f};
    vkCmdPushConstants(cb, post_.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    g_rec.barrier();
    rs_.pc_layout = VK_NULL_HANDLE;  // the game's next draw pushes its constants again
    tc_.mark_modified(img);
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
    begin_rendering(fb);
    vkCmdClearAttachments(g_rec.cmd_in_pass(), (u32)att.size(), att.data(), 1, &r);
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
        auto range = [cb_va, cb_size](u32 index, u64& lo, u32& size) -> bool {
            if (index >= 8 || !cb_va[index]) return false;
            lo = cb_va[index];
            size = cb_size[index];
            return true;
        };
        bind_resources(cp->sh->prog.resources, cp->layout, set, cbuf, range, pools, linked, post);
    }
    VkCommandBuffer cb = g_rec.cmd();
    full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pl, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cb, std::max(1u, q[12] & 0x7FFFFFFF), std::max(1u, q[13] & 0xFFFF), std::max(1u, q[13] >> 16));
    full_barrier(cb);
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
    VkCommandBuffer cb = g_rec.cmd();
    full_barrier(cb);
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   linear && !depth && !src->info.fmt.integer ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    full_barrier(cb);
    tc_.mark_modified(dst);
    return true;
}

// ------------------------------------------------------------------ present
static void write_image_bmp(VkImage image, u32 w, u32 h, VkFormat fmt, const char* path,
                            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
static void dump_frame(VkImage image, u32 w, u32 h, VkFormat fmt) {
    static const char* dir = getenv("HWDER_DUMP_FRAMES");
    static u32 n = 0;
    if (!dir) return;
    // Note: each dump waits for the GPU and writes an 8 MB BMP (a visible hitch) - keep `every` large.
    u32 every = getenv("HWDER_DUMP_EVERY") ? (u32)atoi(getenv("HWDER_DUMP_EVERY")) : 600;
    // HWDER_DUMP_AFTER=<secs>: no dumps (and no dump stalls) before that time, so the run reaches
    // the interesting part at normal speed.
    static double after_s = getenv("HWDER_DUMP_AFTER") ? atof(getenv("HWDER_DUMP_AFTER")) : 0.0;
    if (after_s > 0 && GetTickCount64() - g_start_ms < (u64)(after_s * 1000.0)) {
        n++;
        return;
    }
    if ((n++ % std::max(1u, every)) != 0) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%05u.bmp", dir, n - 1);
    write_image_bmp(image, w, h, fmt, path);
}
// Reads back a 32-bit colour image and writes it as a BMP (waits for the GPU).
static void write_image_bmp(VkImage image, u32 w, u32 h, VkFormat fmt, const char* path, VkImageAspectFlags aspect) {
    u32 bpp = aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? 4 : vk_format_bytes(fmt);  // depth reads back as 32-bit words
    if (vk_format_compressed(fmt) || !bpp) return;
    Buffer rb = create_buffer((u64)w * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false, true);
    VkBufferImageCopy c{};
    c.imageSubresource = {aspect, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    VkCommandBuffer cb = g_rec.cmd();
    full_barrier(cb);
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, rb.buf, 1, &c);
    g_rec.wait_idle();
    {  // raw texels next to the BMP (floats are not representable in a BMP)
        std::string raw = std::string(path) + ".raw";
        if (FILE* f = fopen(raw.c_str(), "wb")) {
            fwrite(rb.map, 1, (size_t)w * h * bpp, f);
            fclose(f);
        }
    }
    if (bpp != 4 || aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        destroy_buffer(rb);
        return;
    }
    // BMP (bottom-up BGRA)
    if (FILE* f = fopen(path, "wb")) {
        u32 size = 54 + w * h * 4;
        u8 hdr[54] = {'B', 'M'};
        memcpy(hdr + 2, &size, 4);
        u32 v = 54;
        memcpy(hdr + 10, &v, 4);
        v = 40;
        memcpy(hdr + 14, &v, 4);
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        u16 planes = 1, bpp = 32;
        memcpy(hdr + 26, &planes, 2);
        memcpy(hdr + 28, &bpp, 2);
        fwrite(hdr, 1, 54, f);
        bool bgr = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
        std::vector<u8> row(w * 4);
        for (u32 y = 0; y < h; y++) {
            const u8* s = rb.map + (u64)(h - 1 - y) * w * 4;
            for (u32 x = 0; x < w; x++) {
                row[x * 4 + 0] = bgr ? s[x * 4 + 0] : s[x * 4 + 2];
                row[x * 4 + 1] = s[x * 4 + 1];
                row[x * 4 + 2] = bgr ? s[x * 4 + 2] : s[x * 4 + 0];
                row[x * 4 + 3] = 255;
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
        hw_log("vk: dumped %s", path);
    }
    destroy_buffer(rb);
}

// Filmstrip index tag of the next capture: what the output is (always the real frame).
static char g_strip_tag[64] = "real";

// HWDER_FILMSTRIP=<dir>: a 96x54 thumbnail of every presented frame (f_<present>.raw RGBA8 + index.txt),
// read back two presents later so it never stalls. For frame-by-frame inspection of transitions/cadence.
static void filmstrip_capture(VkImage src, u32 w, u32 h, const char* dir) {
    // HWDER_FILMSTRIP_W=<width> (default 96): thumbnail width, 16:9.
    static const u32 SW = getenv("HWDER_FILMSTRIP_W") ? std::max(16, atoi(getenv("HWDER_FILMSTRIP_W"))) : 96, SH = SW * 9 / 16;
    // HWDER_FILMSTRIP_BATTLE=<s>: start that long after the first battle-sized frame (> 450 draws);
    // HWDER_FILMSTRIP_AFTER=<s>: or at that time since boot; HWDER_FILMSTRIP_FOR=<s>: stop after that long.
    {
        static const char* battle = getenv("HWDER_FILMSTRIP_BATTLE");
        static const double dur = getenv("HWDER_FILMSTRIP_FOR") ? atof(getenv("HWDER_FILMSTRIP_FOR")) : 1e9;
        static const double after = getenv("HWDER_FILMSTRIP_AFTER") ? atof(getenv("HWDER_FILMSTRIP_AFTER")) : 0.0;
        static double start = -1;
        double now = (GetTickCount64() - g_start_ms) / 1000.0;
        if (battle && start < 0) {
            if (hw_last_frame_draws.load() <= 450) return;
            start = now + atof(battle);
            hw_log("filmstrip: battle frame seen, capturing from t=%.1f for %.1f s", start, dur);
        }
        if (start < 0) start = std::max(now, after);
        g_drawlog_window = now >= start && now <= start + dur;
        if (!g_drawlog_window) return;
    }
    static VkImage simg = VK_NULL_HANDLE;
    static VkDeviceMemory smem = VK_NULL_HANDLE;
    static Buffer bufs[3];
    static u64 serials[3] = {}, presents[3] = {};
    static bool pending[3] = {};
    static int slot = 0;
    static FILE* index = nullptr;
    VkCommandBuffer cb = g_rec.cmd();
    if (!simg) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {SW, SH, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(dev(), &ci, nullptr, &simg) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev(), simg, &req);
        const auto& mp = display::device().mem;
        u32 type = 0;
        for (u32 i = 0; i < mp.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        vkAllocateMemory(dev(), &ai, nullptr, &smem);
        vkBindImageMemory(dev(), simg, smem, 0);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = simg;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        for (auto& bf : bufs) bf = create_buffer(SW * SH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false, true);
        CreateDirectoryA(dir, nullptr);
        char path[512];
        snprintf(path, sizeof(path), "%s/index.txt", dir);
        index = fopen(path, "w");
    }
    int sl = slot;
    slot = (slot + 1) % 3;
    if (pending[sl]) {
        wait_serial(serials[sl]);
        char path[512];
        snprintf(path, sizeof(path), "%s/f_%06llu.raw", dir, (unsigned long long)presents[sl]);
        if (FILE* f = fopen(path, "wb")) {
            fwrite(bufs[sl].map, 1, SW * SH * 4, f);
            fclose(f);
        }
        pending[sl] = false;
    }
    full_barrier(cb);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(s32)w, (s32)h, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(s32)SW, (s32)SH, 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, simg, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    full_barrier(cb);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {SW, SH, 1};
    vkCmdCopyImageToBuffer(cb, simg, VK_IMAGE_LAYOUT_GENERAL, bufs[sl].buf, 1, &c);
    full_barrier(cb);
    serials[sl] = g_rec.serial();
    static u64 cap_seq = 0;  // capture sequence: several outputs per game frame with interpolation
    presents[sl] = cap_seq;
    pending[sl] = true;
    if (index) {
        fprintf(index, "%llu %.3f %s present=%llu\n", (unsigned long long)cap_seq, (GetTickCount64() - g_start_ms) / 1000.0, g_strip_tag,
                (unsigned long long)g_s.present_count);
        fflush(index);
    }
    cap_seq++;
}

void VkRenderer::present(const FramebufferInfo& fb) {
    VkImage image = VK_NULL_HANDLE;
    u32 w = fb.width, h = fb.height;
    LARGE_INTEGER pt0, pt1, pt2, ptf;
    QueryPerformanceFrequency(&ptf);
    QueryPerformanceCounter(&pt0);
    {
        std::lock_guard<std::recursive_mutex> l(m_);
        QueryPerformanceCounter(&pt1);
        process_invalidations();
        // Several images can share the presented address (a 3D render target, a VIC/CPU-written movie
        // frame, a differently sized alias). Take the most recently written one, and if the CPU wrote
        // the buffer after the last GPU render, re-upload it rather than showing the stale render
        // (that was the "previous scene flashes for a frame" symptom).
        Image* img = tc_.find_newest(fb.address);
        if (img && img->cpu_dirty) tc_.validate(img);
        if (!img || !img->gpu_modified || img->level_width(0) < w || img->level_height(0) < h ||
            img->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            ImageInfo info;
            info.fmt = android_format(fb.format);
            info.width = fb.width;
            info.height = fb.height;
            info.block_linear = fb.block_linear;
            info.bh = fb.block_height_log2;
            info.pitch = fb.stride * info.fmt.bpb;
            if (!fb.block_linear && !info.pitch) info.pitch = fb.width * info.fmt.bpb;
            Subresource sub;
            img = tc_.get(fb.address, info, &sub);
            if (!img->gpu_modified) tc_.validate_now(img);  // a presented guest buffer is new every frame
        }
        w = (u32)(w * img->scale + 0.5f);
        h = (u32)(h * img->scale_y + 0.5f);
        QueryPerformanceCounter(&pt2);
        if ((pt2.QuadPart - pt0.QuadPart) * 1000.0 / ptf.QuadPart > 20.0)
            hw_log("vk: slow: present prologue %.0f ms (renderer lock %.0f, image lookup/validate %.0f) %s%s", (pt2.QuadPart - pt0.QuadPart) * 1000.0 / ptf.QuadPart,
                   (pt1.QuadPart - pt0.QuadPart) * 1000.0 / ptf.QuadPart, (pt2.QuadPart - pt1.QuadPart) * 1000.0 / ptf.QuadPart,
                   img->gpu_modified ? "gpu" : "cpu", img->cpu_dirty ? " dirty" : "");
        {   // HWDER_PRESENT_TRACE=1: one line per present (address switches, stale re-presents, movie cadence)
            static const bool ptrace = getenv("HWDER_PRESENT_TRACE") != nullptr;
            if (ptrace) {
                static u64 last_addr = 0, last_hash = 0, same_hash_run = 0;
                u64 ch = img->gpu_modified ? img->write_seq : hash_bytes((const u8*)img->addr, std::min<u64>(img->readable, 64 << 10));
                same_hash_run = (ch == last_hash) ? same_hash_run + 1 : 0;
                hw_log("ptrace: %llx %s wseq %llu pseq %llu %s%s draws %llu", (unsigned long long)fb.address,
                       img->gpu_modified ? "gpu" : "cpu", (unsigned long long)img->write_seq, (unsigned long long)img->presented_seq,
                       fb.address != last_addr ? "SWITCH " : "", same_hash_run ? "REPEAT " : "", (unsigned long long)draws_);
                last_addr = fb.address;
                last_hash = ch;
            }
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            hw_log("vk: present %ux%u fmt %u from %s image (draws %llu, skipped %llu)", w, h, fb.format,
                   img->gpu_modified ? "GPU-rendered" : "guest-memory", (unsigned long long)draws_,
                   (unsigned long long)skipped_);
        }
        image = img->image;
        {
            // Diagnostic: draws landing in this buffer after this present (before its next clear) are
            // counted by TextureCache::mark_modified and reported with the frame statistics.
            img->presented_seq = img->write_seq;
            static u64 last_late = 0;
            if (g_s.present_count % 120 == 1 && tc_.late_writes_ != last_late) {
                hw_log("vk: %llu draws into already-presented buffers since the last report (present ran before the frame finished)",
                       (unsigned long long)(tc_.late_writes_ - last_late));
                last_late = tc_.late_writes_;
            }
        }
        if (dumping()) {
            hw_log("fd: present %llx %ux%u f%d %s", (unsigned long long)fb.address, w, h, (int)img->info.fmt.vk,
                   img->gpu_modified ? "gpu-rendered" : "from guest memory");
            // Contents of every render target of this frame, for finding the stage that goes wrong.
            CreateDirectoryA("fd", nullptr);
            // In a multi-present window (HWDER_VK_FRAME_DUMP_COUNT) only the last present writes the RTs
            // (each write-out stalls ~1 s); the earlier presents just log.
            bool last_of_window = g_s.present_count + 2 >= dump_frame_ + dump_count_;
            if (last_of_window) for (Image* rt : dump_rts_) {
                char path[256];
                bool depth = rt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
                snprintf(path, sizeof(path), "fd/%s_%llx_%ux%u_f%d.bmp", depth ? "depth" : "rt", (unsigned long long)rt->addr,
                         rt->info.width, rt->info.height, (int)rt->info.fmt.vk);
                write_image_bmp(rt->image, rt->level_width(0), rt->level_height(0), rt->info.fmt.vk, path,
                                depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
            }
            if (last_of_window) {
                hw_log("fd: wrote %zu render targets to fd/", dump_rts_.size());
                dump_rts_.clear();
            }
        }
        {
            static const char* strip_dir = getenv("HWDER_FILMSTRIP");
            if (strip_dir) filmstrip_capture(image, w, h, strip_dir);
        }
        // Fill ultrawide: the orthographic HUD (and movies, which the game draws as a 2D quad) stays in
        // the centred 16:9; when nothing full-width (a 3D frame's post passes, a full clear) wrote this
        // frame, the side bands hold stale memory. Clear them to black before presenting.
        // "Since it was last presented", not "during this present's frame number": with frames queued ahead
        // (stage b's triple buffering) the GPU thread draws the next frame before this one is presented,
        // and a frame-number check blacked out side bands that were in fact drawn (flashing bars).
        const bool full_width = img->full_width_since_present;
        img->full_width_since_present = false;
        {
            // Every 600 presents: how many had their side bands blacked out (should be ~0 in 3D scenes).
            static u32 n = 0, cleared = 0;
            if (settings::video().unlocked_res && settings::video().ultrawide && img->scale > img->scale_y) {
                n++;
                if (!full_width) cleared++;
                if (n == 600) {
                    hw_log("vk: ultrawide side bands blacked out on %u of %u presents", cleared, n);
                    n = cleared = 0;
                }
            }
        }
        if (settings::video().unlocked_res && settings::video().ultrawide && img->scale > img->scale_y &&
            !full_width && img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            u32 iw = img->level_width(0), ih = img->level_height(0);
            u32 center = std::min(iw, (u32)(iw * (img->scale_y / img->scale) + 0.5f));
            u32 band = (iw - center) / 2;
            if (band > 0) {
                Framebuffer f;
                f.color[0] = img;
                f.color_format[0] = img->info.fmt.vk;
                f.color_view[0] = tc_.view(img, img->info.fmt.vk, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1);
                f.num_colors = 1;
                f.width = iw;
                f.height = ih;
                f.scale = img->scale;
                f.scale_y = img->scale_y;
                if (f.color_view[0]) {
                    VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0};
                    a.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                    VkClearRect rr[2] = {{{{0, 0}, {band, ih}}, 0, 1}, {{{(s32)(iw - band), 0}, {band, ih}}, 0, 1}};
                    begin_rendering(f);
                    vkCmdClearAttachments(g_rec.cmd_in_pass(), 1, &a, 2, rr);
                }
            }
        }
        dump_frame(image, w, h, img->info.fmt.vk);
        g_rec.flush();
        g_s.present_count++;
        {
            static u64 draws_at_last = 0;
            hw_last_frame_draws = (u32)(draws_ - draws_at_last);
            draws_at_last = draws_;
        }
        {   // F1 menu changed the internal resolution: apply between frames
            static const bool env_scale = getenv("HWDER_RES_SCALE") || getenv("HWDER_RES_SCALE_X") || getenv("HWDER_RES_SCALE_Y");
            if (!env_scale) {
                float wx = settings::video().res_scale, wy = settings::video().res_scale;
                if (settings::video().unlocked_res) {  // largest 16:9 that fits the window; the blit pillarboxes the rest
                    unsigned cw = 0, chh = 0;
                    display::client_size(&cw, &chh);
                    if (!cw || !chh) cw = GetSystemMetrics(SM_CXSCREEN), chh = GetSystemMetrics(SM_CYSCREEN);
                    wx = wy = std::min(cw / 1920.0f, chh / 1080.0f);  // uneven X/Y squeezes the game's 16:9 projection
                    // Fill ultrawide: the game's camera is widened to the window's aspect (runtime/src/game/
                    // widescreen.cpp), so the targets take the window's shape too.
                    if (settings::video().ultrawide && cw * 1080 > chh * 1920) wx = cw / 1920.0f, wy = chh / 1080.0f;
                }
                wx = std::clamp(wx, 0.5f, 4.0f);
                wy = std::clamp(wy, 0.5f, 4.0f);
                if (wx != g_res_scale || wy != g_res_scale_y) {
                    tc_.apply_scale(wx, wy);
                    fb_cache_valid_ = false;
                }
            }
        }
        if (g_s.present_count % 120 == 1) {
            hw_log("vk: cache: tic %llu/%llu vb %llu/%llu idx %llu/%llu (hit/miss since last report)", (unsigned long long)st_.tic_hit,
                   (unsigned long long)st_.tic_miss, (unsigned long long)st_.vb_hit, (unsigned long long)st_.vb_miss,
                   (unsigned long long)st_.idx_hit, (unsigned long long)st_.idx_miss);
            st_.tic_hit = st_.tic_miss = st_.vb_hit = st_.vb_miss = st_.idx_hit = st_.idx_miss = 0;
            hw_log("vk: frame %llu: draws %llu (skipped %llu) clears %llu dispatches %llu blits %llu | %zu programs %zu "
                   "pipelines %zu shaders | tex: %u uploads (%llu KiB) %u downloads %u created", (unsigned long long)g_s.present_count,
                   (unsigned long long)st_.draws, (unsigned long long)st_.skipped, (unsigned long long)st_.clears,
                   (unsigned long long)st_.dispatches, (unsigned long long)st_.blits, programs_.size(), pipelines_.size(),
                   shaders_.size(), tc_.n_uploads, (unsigned long long)(tc_.upload_bytes >> 10), tc_.n_downloads, tc_.n_creates);
            hw_log("vk: gpu thread busy %.1f ms per frame over the last 120 frames", gpu::gpu_busy_ms_reset() / 120.0);
        }
        const u64 audit_now = GetTickCount64();
        if (audit_now - audit_report_ms_ >= 10000) {
            audit_report_ms_ = audit_now;
            log_warmup("periodic");
            u64 pending_max;
            { std::lock_guard<std::mutex> l(pipe_m_); pending_max = audit_pending_max_ms_; }
            hw_log("gfx-audit: frame %llu cumulative: fence_timeouts %llu skipped_draws %llu pending_max_ms %llu cpu_image_writes %llu disabled_scaled_viewports %llu tex_dynamic_offsets %llu depth_bias_formats %zu",
                   (unsigned long long)g_s.present_count, (unsigned long long)audit::fence_timeouts.load(std::memory_order_relaxed),
                   (unsigned long long)audit::skipped_draws, (unsigned long long)pending_max,
                   (unsigned long long)audit::cpu_image_writes, (unsigned long long)audit::unscaled_viewports,
                   (unsigned long long)audit::tex_dynamic_offsets, audit_depth_bias_.size());
            for (const auto& [format, count] : audit_depth_bias_)
                hw_log("gfx-audit: depth bias format %u cumulative draws %llu", format, (unsigned long long)count);
            for (const auto& [program, count] : audit_skips_by_program_)
                hw_log("gfx-audit: program %016llx cumulative skipped draws %llu", (unsigned long long)program, (unsigned long long)count);
        }
        if (dump_time_ >= 0 && GetTickCount64() - g_start_ms >= (u64)(dump_time_ * 1000.0)) {
            dump_time_ = -1;
            dump_frame_ = g_s.present_count + 1;
            hw_log("fd: dumping frame %llu (time trigger)", (unsigned long long)dump_frame_);
        }
        // HWDER_VK_FRAME_DUMP_MIN_DRAWS overrides the 600-draw threshold (e.g. 30 to catch a menu frame).
        // "30-200" also sets an upper bound (skips the hundreds-of-draws cutscene frames).
        static const char* auto_env = getenv("HWDER_VK_FRAME_DUMP_MIN_DRAWS");
        static const u64 auto_min_draws = auto_env ? strtoull(auto_env, nullptr, 0) : 600;
        static const u64 auto_max_draws = auto_env && strchr(auto_env, '-') ? strtoull(strchr(auto_env, '-') + 1, nullptr, 0) : ~0ull;
        if (dump_auto_ && st_.draws >= auto_min_draws && st_.draws <= auto_max_draws &&
            GetTickCount64() - g_start_ms >= (u64)(dump_auto_after_ * 1000.0)) {
            dump_auto_ = false;
            // HWDER_VK_FRAME_DUMP_SKIP=n dumps the n-th present after the trigger instead (menu frames
            // straddle presents: a 44-draw "frame" is followed by a 7-draw one, then the next full one).
            static const u64 skip = getenv("HWDER_VK_FRAME_DUMP_SKIP") ? strtoull(getenv("HWDER_VK_FRAME_DUMP_SKIP"), nullptr, 0) : 0;
            // HWDER_VK_FRAME_DUMP_DELAY=<secs>: arm the time trigger that long after the draw-count trigger
            // (e.g. the Select Mode menu is complete ~3 s after its first 44-draw parchment frame).
            static const double delay = getenv("HWDER_VK_FRAME_DUMP_DELAY") ? atof(getenv("HWDER_VK_FRAME_DUMP_DELAY")) : 0.0;
            if (delay > 0) {
                dump_time_ = (GetTickCount64() - g_start_ms) / 1000.0 + delay;
                hw_log("fd: draw-count trigger (previous frame had %llu draws); dumping at t=%.2f", (unsigned long long)st_.draws, dump_time_);
            } else {
                dump_frame_ = g_s.present_count + 1 + skip;
                hw_log("fd: dumping frame %llu (previous frame had %llu draws)", (unsigned long long)dump_frame_, (unsigned long long)st_.draws);
            }
        }
        st_ = {};
        tc_.n_uploads = tc_.n_downloads = tc_.n_creates = 0;
        tc_.upload_bytes = 0;
        tc_.next_epoch();
        run_graveyard();
        if (g_s.present_count % 600 == 0) save_pipeline_cache();
    }
    // Includes the frame limiter's wait (a full 33 ms at swap interval 2), hence the high threshold.
    SlowTimer timer("present", 60.0);
    ProfScope ps(kProfPresent);
    display::present_image(image, w, h, fb.swap_interval, g_s.timeline, g_rec.serial() - 1, fb.release_hz);
}

}  // namespace vk::detail
}  // namespace gpu

namespace gpu {
using namespace vk::detail;
Renderer* create_renderer(MemoryManager* mm) { return new VkRenderer(mm); }

}  // namespace gpu
