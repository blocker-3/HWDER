// Private declarations shared by the Vulkan renderer's translation units (runtime/src/gpu/vk/vk_*.cpp):
// the VkRenderer class, the structs it works with, the command scheduler's state and the helpers that
// more than one unit uses. All of it used to live in vk_renderer.cpp; it moved here text-for-text.
#pragma once
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shader/shader.h"
#include "gpu/gpu.h"
#include "gpu/vk/pipeline_warmup_cache.h"
#include "gpu/vk/pipeline_catalog.h"
#include "vk_common.h"

namespace gpu::vk {

// ------------------------------------------------------------------ scheduler (vk_sched.cpp)
constexpr u64 kRingSize = 256ull << 20;
constexpr u32 kRingSegments = 16;
constexpr u64 kSegSize = kRingSize / kRingSegments;
constexpr VkBufferUsageFlags kRingUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> dpools;
    u32 dpool_index = 0;
    std::vector<std::function<void()>> deferred;
    u64 serial = 0;
};

struct Sched {
    std::mutex m;  // guards in_flight / free_frames / idle callbacks
    std::condition_variable cv;
    std::deque<Frame*> in_flight;
    std::vector<Frame*> free_frames;
    std::deque<std::pair<u64, std::function<void()>>> idle;
    Frame* cur = nullptr;
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::atomic<u64> completed{0};
    u64 submitted = 0;
    Buffer ring;
    u64 ring_head = 0;
    u64 seg_serial[kRingSegments] = {};
    u64 present_count = 0;
    std::vector<std::tuple<u64, u64, std::function<void()>>> graveyard;  // serial, present count, fn
};
extern Sched g_s;
extern Recorder g_rec;

void wait_serial(u64 serial);
void waiter_thread();
void full_barrier(VkCommandBuffer cb);
void run_graveyard();  // present: destroy what the graveyard holds once the GPU and presenter are done with it

}  // namespace gpu::vk

// ================================================================== renderer
namespace gpu::vk::detail {

constexpr u32 kConstBinding = 31;  // vertex binding holding constant attribute values

struct CbufBinding {
    u64 va = 0;
    u32 size = 0;
    bool valid = false;
};

inline VkCompareOp compare_op(u32 v) {
    if (v >= 1 && v <= 8) return (VkCompareOp)(v - 1);
    if (v >= 0x200 && v <= 0x207) return (VkCompareOp)(v - 0x200);
    return VK_COMPARE_OP_ALWAYS;
}
inline VkStencilOp stencil_op(u32 v) {
    switch (v) {
    case 1: case 0x1E00: return VK_STENCIL_OP_KEEP;
    case 2: case 0: return VK_STENCIL_OP_ZERO;
    case 3: case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 4: case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: case 0x150A: return VK_STENCIL_OP_INVERT;
    case 7: case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
inline VkBlendOp blend_op(u32 v) {
    switch (v) {
    case 1: case 0x8006: return VK_BLEND_OP_ADD;
    case 2: case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 3: case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: case 0x8007: return VK_BLEND_OP_MIN;
    case 5: case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
inline VkBlendFactor blend_factor(u32 v) {
    switch (v) {
    case 0x1: case 0x4000: return VK_BLEND_FACTOR_ZERO;
    case 0x2: case 0x4001: return VK_BLEND_FACTOR_ONE;
    case 0x3: case 0x4300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x4: case 0x4301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x5: case 0x4302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x6: case 0x4303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x7: case 0x4304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x8: case 0x4305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x9: case 0x4306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0xA: case 0x4307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: case 0x4308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0xC: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0xD: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0xE: case 0xC001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0xF: case 0xC002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0xC003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0xC004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 0x10: case 0xC900: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: case 0xC901: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: case 0xC902: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: case 0xC903: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
inline VkPrimitiveTopology topology(u32 t) {
    switch (t) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 7: case 8: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // converted
    case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
inline VkFormat cached_vertex_format(u32 raw, u32* size) {
    static std::unordered_map<u32, std::pair<VkFormat, u32>> cache;
    u32 k = raw & 0xFFE00000u;  // size, type, bgra
    auto it = cache.find(k);
    if (it == cache.end()) {
        u32 sz = 0;
        VkFormat f = vertex_format(raw, &sz);
        it = cache.emplace(k, std::make_pair(f, sz)).first;
    }
    if (size) *size = it->second.second;
    return it->second.first;
}

inline VkLogicOp logic_op(u32 v) { return v >= 0x1500 && v <= 0x150F ? (VkLogicOp)(v - 0x1500) : VK_LOGIC_OP_COPY; }

inline VkImageViewType view_type(shader::TextureType t) {
    switch (t) {
    case shader::TextureType::Tex1D: return VK_IMAGE_VIEW_TYPE_1D;
    case shader::TextureType::Tex1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case shader::TextureType::Tex2D: return VK_IMAGE_VIEW_TYPE_2D;
    case shader::TextureType::Tex2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case shader::TextureType::Tex3D: return VK_IMAGE_VIEW_TYPE_3D;
    case shader::TextureType::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case shader::TextureType::CubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

inline VkComponentSwizzle tic_swizzle(u32 s) {
    switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 2: return VK_COMPONENT_SWIZZLE_R;
    case 3: return VK_COMPONENT_SWIZZLE_G;
    case 4: return VK_COMPONENT_SWIZZLE_B;
    case 5: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

// ------------------------------------------------------------------ shader environments
class GuestEnv : public shader::Environment {
public:
    const u8* code = nullptr;  // program start (SPH for graphics)
    u64 code_avail = 0;
    bool graphics = true;
    std::function<u32(u32, u32)> cbuf;
    std::function<shader::TextureType(u32)> tex_type;
    u32 bound_buffer = 2;
    u32 lsize[3] = {1, 1, 1};
    u32 smem = 0;
    u64 attr_types = 0;  // 2 bits per attribute (shader::AttributeType)
    u32 rt_types = 0;    // 2 bits per render target
    bool catalog_safe = true;
    u32 catalog_max_read = 0;

    u64 read_instruction(u32 address) override {
        catalog_max_read = std::max(catalog_max_read, address);
        u64 off = (graphics ? 0x50 : 0) + (u64)address;
        if (off + 8 > code_avail) { catalog_safe = false; return 0; }
        u64 v;
        memcpy(&v, code + off, 8);
        return v;
    }
    const u8* sph() override { return graphics ? code : nullptr; }
    u32 read_cbuf(u32 index, u32 offset) override { catalog_safe = false; return cbuf ? cbuf(index, offset) : 0; }
    shader::TextureType texture_type(u32 handle) override {
        catalog_safe = false;
        return tex_type ? tex_type(handle) : shader::TextureType::Tex2D;
    }
    u32 texture_bound_buffer() override { return bound_buffer; }
    u32 local_size(int axis) override { return lsize[axis]; }
    u32 shared_memory_size() override { return smem; }
    shader::AttributeType attribute_type(u32 index) override {
        return index < 32 ? (shader::AttributeType)((attr_types >> (index * 2)) & 3) : shader::AttributeType::Float;
    }
    shader::AttributeType render_target_type(u32 rt) override {
        return rt < 8 ? (shader::AttributeType)((rt_types >> (rt * 2)) & 3) : shader::AttributeType::Float;
    }
};

inline shader::AttributeType format_numeric_type(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R16G16B16_UINT:
        return shader::AttributeType::UnsignedInt;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
    case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R16G16B16_SINT:
        return shader::AttributeType::SignedInt;
    default:
        return shader::AttributeType::Float;
    }
}

struct StageLayout {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorType> types;
    std::vector<u32> counts;
};

struct ShaderEntry {
    shader::Program prog;
    VkShaderModule module = VK_NULL_HANDLE;
    bool ok = false;
    u64 key = 0;  // HWDER_DUMP_SHADERS file name
    catalog::Stage catalog_stage;
};

// The game's final tonemap pass, by fragment shader key: exposure x scene -> filmic curve, then
// + bloom (the 240x135 blur chain) x c3[0x20], colour balance/saturation, gamma, into the frame the
// HUD is drawn over. Settings > Graphics contrast and bloom are patched into it (shader::Options).
struct PictureShader {
    u64 key;
    u32 bloom_cbuf, bloom_offset;
};
inline const PictureShader kPictureShaders[] = {
    {0x0fd5e74182e59ceaull, 3, 0x20},  // battles (Legend Mode, Sealed Grounds)
};

struct WarmProgram;
struct GfxProgram {
    ShaderEntry* stages[5] = {};  // VertexB, TessControl, TessEval, Geometry, Fragment
    StageLayout* layouts[5] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    u32 attributes = 0;
    bool ok = false;
    std::shared_ptr<warmup::ProgramRecord> recipe;
    u32 recipe_index = ~0u;
    WarmProgram* matched_warm = nullptr;
};

using warmup::PipelineKey;
struct WarmProgram {
    u32 record_index;
    GfxProgram gfx;  // separate modules/layouts; never inserted into live program/shader maps
    bool canceled = false;  // pipe_m_ protects cancellation and worker state
    ~WarmProgram() {
        if (gfx.layout) vkDestroyPipelineLayout(dev(), gfx.layout, nullptr);
        for (u32 s = 0; s < 5; s++) {
            if (gfx.stages[s]) {
                if (gfx.stages[s]->module) vkDestroyShaderModule(dev(), gfx.stages[s]->module, nullptr);
                delete gfx.stages[s];
            }
            if (gfx.layouts[s]) {
                vkDestroyDescriptorSetLayout(dev(), gfx.layouts[s]->layout, nullptr);
                delete gfx.layouts[s];
            }
        }
    }
};
enum class WarmState { Queued, Building, Ready, Failed, Adopted, Canceled };
struct WarmPipeline {
    WarmProgram* program;
    PipelineKey key;
    WarmState state = WarmState::Queued;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool demanded = false;
    u64 demanded_ms = 0;
};

struct Framebuffer {
    Image* color[8] = {};
    VkImageView color_view[8] = {};
    VkFormat color_format[8] = {};
    Image* depth = nullptr;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    bool has_stencil = false;
    u32 num_colors = 0;
    u32 width = 0, height = 0;  // physical (scaled) size
    float scale = 1.0f, scale_y = 1.0f;  // guest -> physical factors for viewport/scissor/clip
    bool operator==(const Framebuffer& o) const {
        if (num_colors != o.num_colors || width != o.width || height != o.height || depth_view != o.depth_view)
            return false;
        for (u32 i = 0; i < 8; i++)
            if (color_view[i] != o.color_view[i]) return false;
        return true;
    }
};

// Shared across the renderer's units (defined where the comment says).
extern const char* kPipelineCacheFile;  // vk_pipelines.cpp
double prof_flush();                    // vk_renderer.cpp
bool drawlog_active();                  // vk_renderer.cpp

class VkRenderer final : public Renderer {
public:
    explicit VkRenderer(MemoryManager* mm) : mm_(mm), tc_(mm) {
        g_s.ring = create_buffer(kRingSize, kRingUsage, true, false);
        VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tci.initialValue = 0;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
        VK_CHECK(vkCreateSemaphore(dev(), &sci, nullptr, &g_s.timeline));
        std::thread(waiter_thread).detach();
        // Pipeline cache persisted across runs (driver-validated blob; a stale one is just ignored).
        std::vector<u8> blob;
        if (FILE* f = fopen(kPipelineCacheFile, "rb")) {
            fseek(f, 0, SEEK_END);
            blob.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            if (fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
            fclose(f);
        }
        VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pci.initialDataSize = blob.size();
        pci.pInitialData = blob.data();
        if (vkCreatePipelineCache(dev(), &pci, nullptr, &pcache_) != VK_SUCCESS) {
            pci.initialDataSize = 0;
            vkCreatePipelineCache(dev(), &pci, nullptr, &pcache_);
        }
        if (!blob.empty()) hw_log("vk: loaded pipeline cache (%zu KiB)", blob.size() >> 10);
        create_dummies();
        hw_log("vk: renderer ready (ring %llu MiB)", (unsigned long long)(kRingSize >> 20));
    }

    void start_prewarm(warmup::Cache&& cache, u64 load_ms);
    void wait_catalog();

    // -------------------------------------------------------------- Renderer interface
    void bind_const_buffer(u32 stage, u32 slot, u64 va, u32 size, bool valid) override {
        std::lock_guard<std::recursive_mutex> l(m_);
        if (stage >= 5 || slot >= 18) return;
        cbufs_[stage][slot] = {va, size, valid};
    }

    void draw(const u32* regs, const DrawParams& p) override {
        ProfScope ps(kProfDrawTotal);
        std::lock_guard<std::recursive_mutex> l(m_);
        process_invalidations();
        do_draw(regs, p, nullptr, 0);
    }
    void draw_inline_indices(const u32* regs, const DrawParams& p, const void* indices, u32 index_size) override {
        std::lock_guard<std::recursive_mutex> l(m_);
        process_invalidations();
        do_draw(regs, p, indices, index_size);
    }
    void clear(const u32* regs, u32 v) override {
        std::lock_guard<std::recursive_mutex> l(m_);
        process_invalidations();
        ProfScope ps(kProfClear);
        do_clear(regs, v);
    }
    void dispatch(const u32* cregs, u64 qmd_va) override {
        std::lock_guard<std::recursive_mutex> l(m_);
        process_invalidations();
        do_dispatch(cregs, qmd_va);
    }
    bool blit_2d(const u32* r) override {
        std::lock_guard<std::recursive_mutex> l(m_);
        process_invalidations();
        return do_blit(r);
    }
    void invalidate_region(u64 addr, u64 size) override {
        if (!addr || !size) return;
        std::lock_guard<std::mutex> l(inv_m_);
        if (!invalidations_.empty() && invalidations_.back().first + invalidations_.back().second == addr)
            invalidations_.back().second += size;
        else
            invalidations_.emplace_back(addr, size);
    }
    void audit_cpu_write(u64 addr, u64 size) override {
        if (!addr || !size) return;
        std::lock_guard<std::recursive_mutex> l(m_);
        tc_.audit_cpu_write(addr, size);
    }
    void flush_region(u64 addr, u64 size) override {
        if (!addr || !size) return;
        std::lock_guard<std::recursive_mutex> l(m_);
        process_invalidations();
        tc_.flush(addr, size);
    }
    bool is_gpu_modified(u64 addr, u64 size) override {
        if (!addr || !size) return false;
        std::lock_guard<std::recursive_mutex> l(m_);
        return tc_.is_gpu_modified(addr, size);
    }
    void on_gpu_idle(std::function<void()> fn) override {
        {
            std::lock_guard<std::mutex> l(g_s.m);
            if (!g_s.idle.empty() || g_s.completed < g_s.submitted) {
                g_s.idle.emplace_back(g_s.submitted, std::move(fn));
                return;
            }
        }
        fn();
    }
    void flush_commands() override {
        std::lock_guard<std::recursive_mutex> l(m_);
        if (prof_flush() > 50.0)
            hw_log("vk: profile tex (frame so far): %u created, %u uploads (%llu KiB), %u downloads", tc_.n_creates, tc_.n_uploads,
                   (unsigned long long)(tc_.upload_bytes >> 10), tc_.n_downloads);
        g_rec.flush();
    }
    void wait_idle() override {
        std::lock_guard<std::recursive_mutex> l(m_);
        g_rec.wait_idle();
    }
    void present(const FramebufferInfo& fb) override;

private:
    struct ViewParams {
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        VkImageViewType vt = VK_IMAGE_VIEW_TYPE_2D;
        u32 base_level = 0, levels = 1, base_layer = 0, layers = 1;
        VkComponentMapping swz{};
        VkImageAspectFlags aspect = 0;
    };
    struct DescWrite {  // reserved before use: writes keep pointers into the vectors
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> buffers;
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkBufferView> texels;
    };
    bool draw_samples_rt_ = false;  // the draw being built samples a render target (post-process pass)
    int cur_stage_ = 0;                // stage being bound
    // ---- stage c (HWDER_GAME_INTERP): the game draws an in-between frame per tick (interp_draw.cpp)
    // with a blended camera; here every draw's vertex-stage transforms (c3: world matrix, bone palette)
    // are blended halfway between the previous real frame's copy of the same draw and this one.
    struct ObjEntry {
        u32 off, size;  // in the frame's arena
        float pos[3];   // first world translation
        bool has_pos;
    };
    struct ObjFrame {
        u64 tag = 0;  // (tick << 1) | extra
        std::vector<u8> arena;                                    // reused: no allocation per draw
        std::unordered_map<u64, std::vector<ObjEntry>> idx;       // draw key -> occurrences (vectors kept, cleared)
        void reset() {
            arena.clear();  // capacity kept
            idx.clear();    // keys come and go (addresses are part of them): never let the map grow
        }
    };
    ObjFrame obj_cur_, obj_prev_;
    std::unordered_map<u64, std::vector<bool>> obj_used_;  // in-between frame: previous blocks already paired
    std::unordered_map<u64, u32> obj_occ_;                  // in-between frame: blocks of each key so far
    u64 obj_tag_ = 0;
    struct ObjStats {
        u64 draws = 0, matched = 0, blended = 0, mats = 0, ticks = 0;
        u64 ambiguous = 0, offsets = 0, move = 0, rigid = 0, rotation = 0;  // blocks left at tick n, by reason
        u64 skin_matched = 0, skin_blended = 0;  // blocks of 8+ matrices (bone palettes)
        double ms = 0;
    } obj_st_;
    // HWDER_GI_OBJ_TRACE=1: once a second, log the in-between frame's c3 block nearest the
    // camera target (the player) among bone palettes and among all blocks, with its outcome.
    struct ObjTraceRec {
        bool valid = false;
        u8 outcome = 0;
        u32 bytes = 0, nm = 0, fail_n = ~0u, nprev = 0, occ = 0;
        float d_at = 0, rel[3] = {}, pair_d = -1, second_d = -1;
        u16 offs[48];
        float moves[48];
    };
    ObjTraceRec obj_tr_skin_, obj_tr_any_;
    float obj_at_[3] = {}, obj_eye_[3] = {};
    bool obj_cam_ = false;
    // Also: every c3 block of the frame within kObjNear of the player (the previous frame's bone palette
    // nearest the camera target), and other vertex-stage constant buffers holding transforms near him.
    struct ObjNear {
        u64 key;
        u32 bytes, nm;
        u8 outcome;
        float d;
    };
    std::vector<ObjNear> obj_near_cur_, obj_near_last_;
    float obj_link_[3] = {}, obj_link_next_[3] = {}, obj_link_next_d_ = INFINITY;
    bool obj_link_ok_ = false;
    struct CbufCensus { u32 blocks = 0, with_tf = 0, close = 0; } obj_cb_[18];
    void obj_cbuf_census(u32 index, const u8* data, u64 bytes);
    void obj_trace_log();
    void obj_frame_begin(u64 tag);
    void obj_transforms(u32 index, u8* data, u64 bytes);
    bool obj_candidate(u32 index, u64 bytes) const {
        return cur_stage_ == 0 && index == 3 && bytes >= 64 && bytes <= 4096 && gpu::exec_gi_tag();
    }
    u64 cur_draw_key_ = 0;             // program + vertex/index addresses + counts of the draw being bound

    void process_invalidations() {
        SlowTimer timer("process_invalidations", 30.0);
        ProfScope ps(kProfInvalidate);
        std::vector<std::pair<u64, u64>> inv;
        {
            std::lock_guard<std::mutex> l(inv_m_);
            inv.swap(invalidations_);
        }
        for (auto& [a, s] : inv) tc_.invalidate(a, s);
    }

    u8* host(u64 va) { return mm_->translate(va); }
    u32 read_u32(u64 va) {
        u32 v = 0;
        mm_->read(va, &v, 4);
        return v;
    }

    // ---------------------------------------------------------- resources
    void create_dummies();
    VkImageView dummy_view(VkImageViewType t);
    Framebuffer resolve_framebuffer(const u32* regs, bool for_clear, u32 clear_rt);
    void begin_rendering(const Framebuffer& fb);
    StageLayout* stage_layout(const shader::Resources& r, VkShaderStageFlags stages, bool live = true);
    ShaderEntry* get_shader(GuestEnv& env, shader::Stage stage, const shader::Options& opt, GuestEnv* va);
    GfxProgram* get_program(const u32* regs, u64* key_out);
    VkPipeline get_pipeline(GfxProgram* prog, const PipelineKey& key);
    VkPipeline create_pipeline(GfxProgram* prog, const PipelineKey& key);  // worker-thread safe
    void pipeline_worker();
    void adopt_pipeline_results();  // renderer thread, pipe_m_ held
    void start_pipeline_worker();  // pipe_m_ held; one worker for speculative and demanded work
    void prepare_program(GfxProgram* prog, u64 key);
    void record_pipeline(GfxProgram* prog, const PipelineKey& key);
    void ensure_warmup_records();
    void save_warmup_records();
    void record_catalog_pipeline(GfxProgram*, const PipelineKey&);
    void save_catalog_records();
    void log_warmup(const char* phase);
    void cancel_warm_program(WarmProgram* prog);  // pipe_m_ held
    struct PipeJob {
        u64 hash;
        GfxProgram* prog;
        PipelineKey key;
        u64 enqueued_ms;
        WarmPipeline* warm = nullptr;
    };
    struct PipeDone {
        u64 hash;
        VkPipeline pipe;
        GfxProgram* prog;
        PipelineKey key;
    };
    u64 audit_pending_max_ms_ = 0; // pipe_m_ protects worker writes and report reads
    u64 audit_report_ms_ = 0;
    std::unordered_map<u32, u64> audit_depth_bias_;
    std::unordered_map<u64, u64> audit_skips_by_program_;
    std::mutex pipe_m_;
    std::condition_variable pipe_cv_;
    std::condition_variable pipe_done_cv_;
    std::deque<PipeJob> pipe_jobs_;
    std::vector<PipeDone> pipe_done_;
    std::unordered_set<u64> pipe_pending_;  // GPU thread only
    std::unordered_set<u64> pipe_retried_;  // GPU thread only: async build failed, retried synchronously once
    std::vector<std::thread> pipe_workers_;
    catalog::Catalog catalog_records_;
    bool catalog_dirty_ = false;
    warmup::Cache warmup_records_;  // retained historical successes plus observed new successes
    warmup::SaveTask warmup_save_;
    bool warmup_records_loaded_ = false, warmup_dirty_ = false, warmup_first_draw_ = false;
    std::unordered_set<u32> invalid_warm_programs_;
    std::vector<std::unique_ptr<WarmProgram>> warm_programs_;
    std::vector<std::unique_ptr<WarmPipeline>> warm_pipelines_;
    std::unordered_multimap<u64, WarmPipeline*> warm_lookup_;
    std::unordered_multimap<u64, u32> recipe_lookup_;
    u64 warmup_start_ms_ = 0, warmup_loaded_ = 0, warmup_recorded_ = 0, warmup_observed_ = 0;
    u64 warmup_queued_ = 0, warmup_finished_ = 0, warmup_succeeded_ = 0, warmup_adopted_ = 0;
    u64 warmup_mismatched_ = 0, warmup_failed_ = 0, warmup_canceled_ = 0, warmup_demand_hits_ = 0;
    u64 warmup_pending_max_ms_ = 0;
    size_t warmup_storage_bytes_ = 0;
    VkDescriptorSet alloc_set(VkDescriptorSetLayout layout);
    bool bind_resources(const shader::Resources& res, StageLayout* sl, VkDescriptorSet set,
                        const std::function<u32(u32, u32)>& cbuf, const std::function<bool(u32, u64&, u32&)>& cb_range,
                        const u32* tic_pool_regs, bool linked_tsc, std::vector<std::function<void()>>& post);
    bool read_tic(u64 pool_va, u32 limit, u32 index, u32* out);

    void do_draw(const u32* regs, const DrawParams& p, const void* inline_idx, u32 inline_size);
    void do_clear(const u32* regs, u32 v);
    void do_dispatch(const u32* cregs, u64 qmd_va);
    bool do_blit(const u32* r);

    MemoryManager* mm_;
    TextureCache tc_;
    std::recursive_mutex m_;
    std::mutex inv_m_;
    std::vector<std::pair<u64, u64>> invalidations_;
    CbufBinding cbufs_[5][18];
    VkPipelineCache pcache_ = VK_NULL_HANDLE;
    std::unordered_map<u64, std::pair<u64, u64>> prog_hash_cache_;  // code va -> {quick hash, full hash}
    struct ProgState {
        u64 key;
        GfxProgram* prog;
        u64 frame;
    };
    std::unordered_map<u64, ProgState> prog_state_cache_;  // program-selecting registers -> program (per frame)
    GfxProgram* get_program_slow(const u32* regs, u64* key_out);
    DescWrite dw_;  // reused between draws (avoids re-allocating the deques per stage per draw)
    // Command-buffer state already recorded: skip vkCmd* calls that would set the same value again.
    struct RecState {
        u64 serial = ~0ull;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkViewport vp{};
        VkRect2D sc{};
        float bias[3] = {-12345.f, -12345.f, -12345.f};
        float bc[4] = {-12345.f, -12345.f, -12345.f, -12345.f};
        u32 st[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
        float lw = -1.f;
        float bounds[2] = {-1.f, -1.f};
        VkBuffer vb[32] = {};
        VkDeviceSize vo[32] = {};
        VkBuffer ib = VK_NULL_HANDLE;
        VkDeviceSize io = ~0ull;
        VkIndexType it = VK_INDEX_TYPE_MAX_ENUM;
        VkPipelineLayout pc_layout = VK_NULL_HANDLE;
        shader::PushConstants pc{};
    } rs_;
    // Same guest vertex/index range staged again within one submission (instanced crowds, multi-pass
    // meshes): reuse the staging slice instead of copying it again. Validated by a sample hash.
    struct SliceEntry {
        u64 serial, bytes, sample;
        Slice slice;
    };
    std::unordered_map<u64, SliceEntry> vb_cache_;
    struct IdxRange {
        u64 serial, sample;  // valid only within the submission that scanned it (the sample covers 128 bytes)
        u32 vmin, vmax;
    };
    std::unordered_map<u64, IdxRange> idx_range_cache_;  // index buffer scan results (vmin/vmax)
    static u64 sample_hash(const u8* p, u64 n) {
        if (!p || !n) return n;
        u64 h = hash_bytes(p, std::min<u64>(n, 64), n);
        if (n > 128) h = hash_bytes(p + n - 64, 64, h);
        return h;
    }
    // Per-TIC lookup cache: texture handle words -> image/view, valid while no image was created or
    // destroyed (tc_.generation). Only plain textures are cached; images with a same-address alias or
    // GPU-written content keep going through TextureCache::get() for the alias refresh.
    struct TicEntry {
        u32 tic[8];
        VkImageViewType vt;
        u64 gen;
        Image* img;
        VkImageView view;
        ViewParams vp;
    };
    std::unordered_map<u64, TicEntry> tic_cache_;
    Framebuffer fb_cache_;
    u64 fb_cache_key_ = 0, fb_cache_gen_ = 0;
    bool fb_cache_valid_ = false;
    size_t pcache_saved_ = 0;  // pipelines_.size() at the last save
    void save_pipeline_cache() {
        save_warmup_records();
        if (pipelines_.size() == pcache_saved_) return;
        const auto begin = std::chrono::steady_clock::now();
        size_t n = 0;
        std::vector<u8> blob;
        bool complete = false;
        // flags=0 caches synchronize internally. A worker may grow the cache between
        // queries; retry a bounded number of times, then leave saving to a later epoch.
        for (int attempt = 0; attempt < 3; attempt++) {
            if (vkGetPipelineCacheData(dev(), pcache_, &n, nullptr) != VK_SUCCESS || !n) return;
            blob.resize(n);
            const VkResult result = vkGetPipelineCacheData(dev(), pcache_, &n, blob.data());
            if (result == VK_INCOMPLETE) continue;
            if (result != VK_SUCCESS) return;
            complete = true;
            break;
        }
        if (!complete) return;
        const auto retrieved = std::chrono::steady_clock::now();
        if (FILE* f = fopen(kPipelineCacheFile, "wb")) {
            fwrite(blob.data(), 1, n, f);
            fclose(f);
            pcache_saved_ = pipelines_.size();
        }
        if (getenv("HWDER_CACHE_PROFILE")) hw_log("cache-save: native retrieval %.3f ms, disk %.3f ms, %zu bytes",
            std::chrono::duration<double, std::milli>(retrieved-begin).count(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-retrieved).count(), n);
    }
    std::unordered_map<u64, ShaderEntry*> shaders_;
    std::unordered_map<u64, GfxProgram*> programs_;
    std::unordered_map<u64, VkPipeline> pipelines_;
    std::unordered_map<u64, StageLayout*> set_layouts_;
    struct ComputeProgram {
        ShaderEntry* sh = nullptr;
        StageLayout* layout = nullptr;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
    };
    std::unordered_map<u64, ComputeProgram*> compute_;
    Image* dummy2d_ = nullptr;
    Image* dummy3d_ = nullptr;
    Image* dummy1d_ = nullptr;
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;
    // FXAA / sharpening on the tonemapped picture (post.comp), run right after the final tonemap pass.
    void post_picture(Image* img);
    struct {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkImage copy = VK_NULL_HANDLE;  // R8G8B8A8 copy of the picture, sampled by the pass
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView copy_view = VK_NULL_HANDLE;
        u32 w = 0, h = 0;
        bool failed = false;
    } post_;
    Framebuffer cur_fb_;
    u64 draws_ = 0, skipped_ = 0;
    struct Stats {
        u64 draws, skipped, clears, dispatches, blits, pipelines, shaders, images_created;
        u64 tic_hit, tic_miss, vb_hit, vb_miss, idx_hit, idx_miss;
    } st_{};
    bool verbose_ = getenv("HWDER_VK_VERBOSE") != nullptr;
    // HWDER_VK_FRAME_DUMP=<present index>: log every draw/clear/blit of that frame with its targets and sources.
    // "auto": the first frame following one with 600+ draws (a real 3D frame).
    bool dump_auto_ = getenv("HWDER_VK_FRAME_DUMP") && !strncmp(getenv("HWDER_VK_FRAME_DUMP"), "auto", 4);
    double dump_auto_after_ = dump_auto_ ? atof(getenv("HWDER_VK_FRAME_DUMP") + 4) : 0.0;  // "auto70": not before 70 s
    // "t<seconds>": the first frame presented after that many seconds of run time.
    double dump_time_ = getenv("HWDER_VK_FRAME_DUMP") && getenv("HWDER_VK_FRAME_DUMP")[0] == 't' ? atof(getenv("HWDER_VK_FRAME_DUMP") + 1) : -1.0;
    // "p<hex prefix>": the first present after a draw whose program key starts with that prefix (robust against
    // the +-2 s wall-clock jitter of menu screens; combine with HWDER_VK_FRAME_DUMP_COUNT).
    std::string dump_prog_ = getenv("HWDER_VK_FRAME_DUMP") && getenv("HWDER_VK_FRAME_DUMP")[0] == 'p' ? getenv("HWDER_VK_FRAME_DUMP") + 1 : "";
    u64 dump_frame_ = getenv("HWDER_VK_FRAME_DUMP") && !dump_auto_ && dump_time_ < 0 && dump_prog_.empty() ? strtoull(getenv("HWDER_VK_FRAME_DUMP"), nullptr, 0) : ~0ull;
    // HWDER_VK_FRAME_DUMP_COUNT=n dumps n consecutive presents from the trigger (menu frames straddle
    // presents nondeterministically: a 44-draw "frame" may be followed by a 7- or 0-draw one).
    u64 dump_count_ = getenv("HWDER_VK_FRAME_DUMP_COUNT") ? std::max(1ull, strtoull(getenv("HWDER_VK_FRAME_DUMP_COUNT"), nullptr, 0)) : 1;
    bool dumping() const { return g_s.present_count + 1 >= dump_frame_ && g_s.present_count + 1 < dump_frame_ + dump_count_; }
    std::string dump_tex_;  // texture sources of the current draw (filled by bind_resources)
    std::string drawlog_tex_;  // same, compact, for HWDER_DRAWLOG
    std::vector<Image*> dump_rts_;  // render targets drawn to during the dumped frame
};

}  // namespace gpu::vk::detail
