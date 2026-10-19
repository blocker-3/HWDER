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
#include "../../../third_party/renderdoc/renderdoc_app.h"
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
#include "hwder_version.h"
#include "vk_common.h"
#include "post_spv.h"

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

// ------------------------------------------------------------------ scheduler
namespace {
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
    u64 pin_serial = 0;  // ring segments written at/after this serial hold a recorded frame: do not recycle
    bool replay_unsafe = false;  // this frame used per-draw transient resources (dedicated staging buffers, texel views, written SSBOs)
    u64 present_count = 0;
    std::vector<std::tuple<u64, u64, std::function<void()>>> graveyard;  // serial, present count, fn
} g_s;

Recorder g_rec;

Frame* new_frame() {
    Frame* f = nullptr;
    {
        std::unique_lock<std::mutex> l(g_s.m);
        // bound the number of frames in flight
        while (g_s.free_frames.empty() && g_s.in_flight.size() >= 16) g_s.cv.wait(l);
        if (!g_s.free_frames.empty()) {
            f = g_s.free_frames.back();
            g_s.free_frames.pop_back();
        }
    }
    if (!f) {
        f = new Frame;
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        ci.queueFamilyIndex = display::device().queue_family;
        VK_CHECK(vkCreateCommandPool(dev(), &ci, nullptr, &f->pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f->pool;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev(), &ai, &f->cb));
    } else {
        vkResetCommandPool(dev(), f->pool, 0);
        for (VkDescriptorPool p : f->dpools) vkResetDescriptorPool(dev(), p, 0);
    }
    f->dpool_index = 0;
    f->serial = g_rec.serial_;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f->cb, &bi));
    return f;
}

void wait_serial(u64 serial) {
    if (g_s.completed >= serial) return;
    SlowTimer timer("wait_serial", 30.0);
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g_s.timeline;
    wi.pValues = &serial;
    VkResult r = vkWaitSemaphores(dev(), &wi, 5000000000ull);
    if (r == VK_TIMEOUT) {
        hw_log("vk: GPU timeout waiting for serial %llu (device hang?)", (unsigned long long)serial);
        vkWaitSemaphores(dev(), &wi, UINT64_MAX);
    } else if (r != VK_SUCCESS) {
        hw_log("vk: vkWaitSemaphores failed %d (device lost?)", (int)r);
        hw_fatal("vk: device lost");
    }
}

void waiter_thread() {
    SetThreadDescription(GetCurrentThread(), L"HWDER vk waiter");
    for (;;) {
        Frame* f = nullptr;
        {
            std::unique_lock<std::mutex> l(g_s.m);
            g_s.cv.wait(l, [] { return !g_s.in_flight.empty(); });
            f = g_s.in_flight.front();
        }
        wait_serial(f->serial);
        u64 v = 0;
        vkGetSemaphoreCounterValue(dev(), g_s.timeline, &v);
        for (auto& fn : f->deferred) fn();
        f->deferred.clear();
        std::vector<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> l(g_s.m);
            g_s.in_flight.pop_front();
            g_s.free_frames.push_back(f);
            if (f->serial > g_s.completed) g_s.completed = f->serial;
            while (!g_s.idle.empty() && g_s.idle.front().first <= g_s.completed) {
                run.push_back(std::move(g_s.idle.front().second));
                g_s.idle.pop_front();
            }
        }
        g_s.cv.notify_all();
        for (auto& fn : run) fn();
    }
}
}  // namespace

Recorder& rec() { return g_rec; }

VkCommandBuffer Recorder::cmd_in_pass() {
    if (!g_s.cur) {
        g_s.cur = new_frame();
        cb_ = g_s.cur->cb;
    }
    return cb_;
}

VkCommandBuffer Recorder::cmd() {
    VkCommandBuffer cb = cmd_in_pass();
    if (rendering) end_rendering();
    return cb;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

void Recorder::end_rendering() {
    if (!rendering) return;
    vkCmdEndRendering(cb_);
    rendering = false;
    full_barrier(cb_);
}

void Recorder::barrier() {
    VkCommandBuffer cb = cmd();
    full_barrier(cb);
}

u64 Recorder::completed() const { return g_s.completed; }

void Recorder::defer(std::function<void()> fn) {
    cmd_in_pass();
    g_s.cur->deferred.push_back(std::move(fn));
}

Slice Recorder::stage(u64 size, u64 align) {
    cmd_in_pass();
    Slice s;
    if (size > kSegSize / 2) {
        g_s.replay_unsafe = true;  // destroyed with the frame: a replay could not reference it
        Buffer* b = new Buffer(create_buffer(size, kRingUsage, true));
        s.buf = b->buf;
        s.ptr = b->map;
        s.offset = 0;
        g_s.cur->deferred.push_back([b] {
            destroy_buffer(*b);
            delete b;
        });
        return s;
    }
    u64 head = (g_s.ring_head + align - 1) & ~(align - 1);
    u32 seg = (u32)(g_s.ring_head / kSegSize);
    // A head exactly at the ring end reads as segment 16: that is a wrap to segment 0 too.
    if (seg >= kRingSegments || head + size > (u64)(seg + 1) * kSegSize) {
        seg = seg >= kRingSegments ? 0 : (seg + 1) % kRingSegments;
        if (g_s.pin_serial) {  // skip segments still referenced by the recorded (replayable) frame
            u32 tries = 0;
            while (tries < kRingSegments && g_s.seg_serial[seg] >= g_s.pin_serial && g_s.seg_serial[seg] < serial_) {
                seg = (seg + 1) % kRingSegments;
                tries++;
            }
            if (tries == kRingSegments) g_s.pin_serial = 0;  // everything pinned: give the frame up
        }
        head = (u64)seg * kSegSize;
        u64 last = g_s.seg_serial[seg];
        if (last >= serial_) {  // ring exhausted by the current command buffer: submit it first
            flush();
            cmd_in_pass();
        }
        SlowTimer timer("staging ring wrap wait", 30.0);
        wait_serial(last);
    }
    g_s.seg_serial[seg] = serial_;
    g_s.ring_head = head + size;
    s.buf = g_s.ring.buf;
    s.offset = head;
    s.ptr = g_s.ring.map + head;
    return s;
}

void Recorder::flush() {
    if (!g_s.cur) return;
    end_rendering();
    full_barrier(cb_);
    VK_CHECK(vkEndCommandBuffer(cb_));
    Frame* f = g_s.cur;
    g_s.cur = nullptr;
    cb_ = VK_NULL_HANDLE;
    u64 serial = serial_++;
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.signalSemaphoreValueCount = 1;
    ts.pSignalSemaphoreValues = &serial;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_s.timeline;
    {
        SlowTimer timer("vkQueueSubmit", 30.0);
        std::lock_guard<std::mutex> q(display::device().queue_lock);
        VkResult r = vkQueueSubmit(display::device().queue, 1, &si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            hw_log("vk: vkQueueSubmit failed %d", (int)r);
            hw_fatal("vk: submit");
        }
    }
    {
        std::lock_guard<std::mutex> l(g_s.m);
        g_s.in_flight.push_back(f);
        g_s.submitted = serial;
    }
    g_s.cv.notify_all();
}

void Recorder::wait_idle() {
    flush();
    wait_serial(serial_ - 1);
}

// Run after the GPU finished the current work AND a few frames were presented (presenter safety).
static void retire(std::function<void()> fn) {
    g_s.graveyard.emplace_back(g_rec.serial(), g_s.present_count, std::move(fn));
}

static void run_graveyard() {
    auto& g = g_s.graveyard;
    size_t w = 0;
    for (size_t i = 0; i < g.size(); i++) {
        auto& [serial, pc, fn] = g[i];
        if (serial <= g_s.completed && g_s.present_count >= pc + 3) {
            fn();
        } else {
            if (w != i) g[w] = std::move(g[i]);
            w++;
        }
    }
    g.resize(w);
}

}  // namespace vk

// ================================================================== renderer
namespace {
using namespace vk;

constexpr u32 kConstBinding = 31;  // vertex binding holding constant attribute values

struct CbufBinding {
    u64 va = 0;
    u32 size = 0;
    bool valid = false;
};

VkCompareOp compare_op(u32 v) {
    if (v >= 1 && v <= 8) return (VkCompareOp)(v - 1);
    if (v >= 0x200 && v <= 0x207) return (VkCompareOp)(v - 0x200);
    return VK_COMPARE_OP_ALWAYS;
}
VkStencilOp stencil_op(u32 v) {
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
VkBlendOp blend_op(u32 v) {
    switch (v) {
    case 1: case 0x8006: return VK_BLEND_OP_ADD;
    case 2: case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 3: case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: case 0x8007: return VK_BLEND_OP_MIN;
    case 5: case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
VkBlendFactor blend_factor(u32 v) {
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
VkPrimitiveTopology topology(u32 t) {
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
VkFormat cached_vertex_format(u32 raw, u32* size) {
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

VkLogicOp logic_op(u32 v) { return v >= 0x1500 && v <= 0x150F ? (VkLogicOp)(v - 0x1500) : VK_LOGIC_OP_COPY; }

VkImageViewType view_type(shader::TextureType t) {
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

VkComponentSwizzle tic_swizzle(u32 s) {
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

shader::AttributeType format_numeric_type(VkFormat f) {
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
static const PictureShader kPictureShaders[] = {
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

// Draws in the last presented frame (read by the HWDER_AUTOPLAY=2 input script to tell battle from menus).
extern "C" std::atomic<u32> hw_last_frame_draws{0};

// HWDER_DRAWLOG=1 (with HWDER_FILMSTRIP): one line per draw into <filmstrip dir>/drawlog.txt while the
// filmstrip capture window is open, keyed by present number, to find the draw behind a bad frame.
static bool g_drawlog_window = false;
static FILE* g_drawlog = nullptr;
static bool drawlog_active() {
    static const bool on = getenv("HWDER_DRAWLOG") && getenv("HWDER_FILMSTRIP");
    if (!on || !g_drawlog_window) return false;
    if (!g_drawlog) {
        std::string path = std::string(getenv("HWDER_FILMSTRIP")) + "/drawlog.txt";
        g_drawlog = fopen(path.c_str(), "w");
    }
    return g_drawlog != nullptr;
}

static const char* kPipelineCacheFile = "pipeline_cache.bin";
static const char* kWarmupFile = "pipeline_warmup.bin";
static const char* kCatalogFile = "pipeline_catalog.bin";
static catalog::GameSources catalog_sources;
static bool catalog_recording = false, catalog_deferred = false;
static bool catalog_enabled() {
    const char* value = getenv("HWDER_PIPELINE_CATALOG");
    return !value || strcmp(value,"0") != 0;
}
static u64 catalog_build() {
    constexpr char epoch[] = "HWDER metadata pipeline catalog ABI 1";
    u64 tag = hash_bytes(epoch,sizeof(epoch));
    return hash_bytes(HWDER_COMMIT,sizeof(HWDER_COMMIT),tag);
}
static u32 catalog_game_version() { return hwfs::nsp::active() && hwfs::nsp::info().has_version ? hwfs::nsp::info().version : 0; }
static bool catalog_debug_translation() { return getenv("HWDER_SHADER_NO_KIL") || getenv("HWDER_SHADER_FALLBACK"); }

static bool prewarm_enabled() {
    const char* value = getenv("HWDER_PIPELINE_PREWARM");
    return !value || strcmp(value, "0") != 0;
}
static u64 warmup_compatibility() {
    // Bump the renderer ABI epoch for changes to translation/layout semantics not
    // already separated by the compiled build identity. No guest path is trusted.
    constexpr char epoch[] = "HWDER graphics warmup ABI 1";
    u64 tag = hash_bytes(epoch, sizeof(epoch));
    tag = hash_bytes(HWDER_COMMIT, sizeof(HWDER_COMMIT), tag);
    const auto& p = display::device().props;
    const u32 host[] = {p.vendorID, p.deviceID, p.driverVersion, sizeof(shader::PushConstants)};
    tag = hash_bytes(host, sizeof(host), tag);
    return hash_bytes(p.pipelineCacheUUID, sizeof(p.pipelineCacheUUID), tag);
}
static const u64 g_start_ms = GetTickCount64();

using gpu::vk::g_prof;
using gpu::vk::g_prof_draws;
static double prof_flush() {
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
    // ---- frame interpolation: a frame's draws/clears recorded at the Vulkan level, replayed with
    // constant buffers lerped against the previous frame to synthesise in-between frames.
    struct WriteRec {
        VkDescriptorType type;
        u32 binding, count, first;  // index into buffers/images/texels of the StageRec
    };
    struct CamHit {  // a run of the frame's camera matrix in a constant buffer (camera mode)
        u32 word;        // float index into CbufRec::data
        u8 mat, layout;  // kCamView/kCamViewProj/kCamInv, kCamLayouts index
    };
    struct CbufRec {
        u32 buffer_index;      // StageRec::buffers entry to repoint
        std::vector<u8> data;  // staged bytes (what the shader saw)
        std::vector<CamHit> cam_hits;
    };
    struct TexelRec {
        u32 index;  // StageRec::texels entry to replace
        VkFormat fmt;
        std::vector<u8> data;
    };
    struct ImageRef {  // parallel to StageRec::images: which cache image a sampled descriptor came from
        Image* img = nullptr;
        ViewParams vp;
    };
    struct StageRec {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        std::vector<ImageRef> image_refs;
        std::vector<TexelRec> texels_rec;
        std::vector<WriteRec> writes;
        std::vector<VkDescriptorBufferInfo> buffers;
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkBufferView> texels;
        std::vector<CbufRec> cbufs;
    };
    struct DrawRec {
        Framebuffer fb;
        VkPipeline pipe;
        VkPipelineLayout layout;
        bool has_stage[5] = {};
        StageRec stages[5];
        VkBuffer vb[32];
        VkDeviceSize vo[32];
        u32 streams_used;
        bool need_const;
        VkViewport vp;
        VkRect2D sc;
        float bias[3], bc[4];
        u32 st[6];
        float lw, bounds[2];
        shader::PushConstants pc;
        bool indexed;
        VkBuffer ib;
        VkDeviceSize io;
        VkIndexType it;
        u32 count, first_vertex, base_instance;
        s32 vertex_offset;
        u8 color_mask[8];
        bool depth_write;
        u64 match_key;  // program + guest vertex/index addresses + counts
    };
    struct ClearRec {
        Framebuffer fb;
        std::vector<VkClearAttachment> att;
        VkClearRect rect;
        u32 rt;
        bool color, depth;
    };
    struct OpRec {
        bool is_clear;
        u64 serial;
        DrawRec draw;
        ClearRec clear;
    };
    std::vector<OpRec> rec_frame_;                   // the frame being presented (split off rec_cur_ at present entry)
    std::unordered_map<Image*, Image*> shadows_;      // real render target -> replay twin
    u64 shadow_destroy_count_ = ~0ull;
    Image* shadow_of(Image* img);
    Framebuffer shadow_fb(const Framebuffer& fb);
    void sync_shadows(const std::vector<OpRec>& ops);
    bool recording_ = false;
    bool draw_samples_rt_ = false;  // the draw being built samples a render target (post-process pass)
    std::vector<OpRec> rec_cur_, rec_prev_;
    u64 rec_destroy_count_ = 0, frame_first_serial_ = 0;
    double interp_acc_ = 0;
    bool prev_unsafe_ = false;
    VkImage saved_img_ = VK_NULL_HANDLE;
    VkDeviceMemory saved_mem_ = VK_NULL_HANDLE;
    u32 saved_w_ = 0, saved_h_ = 0;
    VkFormat saved_fmt_ = VK_FORMAT_UNDEFINED, pres_fmt_ = VK_FORMAT_UNDEFINED;
    void replay_frame(float t);
    // ---- camera mode (HWDER_INTERP_MODE=camera): replays substitute only the camera's view and
    // viewProj, found in the frame's constant buffers by exact match against the tick snapshots.
    static constexpr int kCamView = 0, kCamViewProj = 1, kCamInv = 2, kCamMats = 3, kCamLayouts = 4;
    struct CamFrame {
        bool matched = false, have_prev = false;
        interp::CamSnap cur, prev;  // the frame's tick and the tick before it
    };
    CamFrame cam_frame_;
    bool cam_replay_ = false;          // replay_draw substitutes cam_words_ at the CamHits
    int cur_stage_ = 0;                // stage being bound (cbuf dump)
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
    struct ObjStats { u64 draws = 0, matched = 0, blended = 0, mats = 0, jumps = 0, ticks = 0; double ms = 0; } obj_st_;
    void obj_frame_begin(u64 tag);
    void obj_transforms(u32 index, u8* data, u64 bytes);
    bool obj_candidate(u32 index, u64 bytes) const {
        return cur_stage_ == 0 && index == 3 && bytes >= 64 && bytes <= 4096 && gpu::exec_gi_tag();
    }
    u64 cur_draw_key_ = 0;             // program + vertex/index addresses + counts of the draw being bound
    u32 dump_draw_ = 0;                // draws so far in the dumped frame
    u32 cam_words_[kCamMats][kCamLayouts][16];
    void cam_identify();
    bool cam_prepare(float alpha);
    void replay_draw(const DrawRec& d, const DrawRec* prev, float t);
    void replay_clear(const ClearRec& c);
    bool ensure_saved_image(u32 w, u32 h, VkFormat fmt);

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
    void cbuf_dump(u32 index, u64 va, const u8* data, u64 bytes);
    bool bind_resources(const shader::Resources& res, StageLayout* sl, VkDescriptorSet set,
                        const std::function<u32(u32, u32)>& cbuf, const std::function<bool(u32, u64&, u32&)>& cb_range,
                        const u32* tic_pool_regs, bool linked_tsc, std::vector<std::function<void()>>& post, StageRec* rec_out = nullptr);
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

// ------------------------------------------------------------------ resources
void VkRenderer::create_dummies() {
    static u8 zeros[64 * 6] = {};
    if (getenv("HWDER_VK_WHITE_TEX")) memset(zeros, 0xFF, sizeof(zeros));
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
        ProfScope* fps = new ProfScope(kProfFbGet);
        Image* img = tc_.get((u64)p, info, &sub);
        delete fps;
        u32 layer = sub.layer + (info.type == ImageType::e3D ? 0 : rt[8]);
        fb.color[i] = img;
        fps = new ProfScope(kProfFbView);
        fb.color_view[i] = tc_.view(img, fi.vk, VK_IMAGE_VIEW_TYPE_2D, sub.level, 1, layer, 1);
        delete fps;
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

StageLayout* VkRenderer::stage_layout(const shader::Resources& r, VkShaderStageFlags stages, bool live) {
    StageLayout tmp;
    auto add = [&](VkDescriptorType t, u32 n) {
        tmp.types.push_back(t);
        tmp.counts.push_back(std::max(1u, n));
    };
    for (size_t i = 0; i < r.const_buffers.size(); i++) add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
    for (size_t i = 0; i < r.storage_buffers.size(); i++) add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1);
    for (auto& t : r.texel_buffers) add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, t.count);
    for (auto& t : r.textures) add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, t.count);
    for (size_t i = 0; i < r.images.size(); i++) add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1);
    u64 key = hash_bytes(tmp.types.data(), tmp.types.size() * 4, stages);
    key = hash_bytes(tmp.counts.data(), tmp.counts.size() * 4, key);
    auto it = set_layouts_.find(key);
    if (live && it != set_layouts_.end()) return it->second;
    std::vector<VkDescriptorSetLayoutBinding> b(tmp.types.size());
    for (size_t i = 0; i < b.size(); i++) {
        b[i] = {};
        b[i].binding = (u32)i;
        b[i].descriptorType = tmp.types[i];
        b[i].descriptorCount = tmp.counts[i];
        b[i].stageFlags = stages;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (u32)b.size();
    ci.pBindings = b.data();
    StageLayout* sl = new StageLayout(tmp);
    VkResult result = vkCreateDescriptorSetLayout(dev(), &ci, nullptr, &sl->layout);
    if (!live && result != VK_SUCCESS) { delete sl; return nullptr; }
    VK_CHECK(result);
    if (live) set_layouts_[key] = sl;
    return sl;
}

ShaderEntry* VkRenderer::get_shader(GuestEnv& env, shader::Stage stage, const shader::Options& opt, GuestEnv* va) {
    u64 h = shader::hash_program(env, stage);
    if (va) h = h * 31 + shader::hash_program(*va, shader::Stage::VertexA);
    u64 key = h ^ ((u64)stage << 56) ^ ((u64)opt.convert_depth_mode << 60) ^ ((u64)opt.flip_y << 61);
    if (stage == shader::Stage::VertexB) key = hash_bytes(&env.attr_types, 8, key);
    if (stage == shader::Stage::Fragment) key = hash_bytes(&env.rt_types, 4, key);
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return it->second;
    ShaderEntry* e = new ShaderEntry;
    shaders_[key] = e;
    e->key = key;
    shader::Options topt = opt;
    for (const PictureShader& ps : kPictureShaders)
        if (stage == shader::Stage::Fragment && key == ps.key) {
            topt.picture_controls = true;
            topt.bloom_cbuf = ps.bloom_cbuf;
            topt.bloom_offset = ps.bloom_offset;
            hw_log("vk: shader %016llx is the final tonemap pass: contrast/bloom controls apply", (unsigned long long)key);
        }
    {
        SlowTimer timer("shader translation", 30.0);
        e->prog = shader::translate(env, stage, topt, va);
    }
    if (catalog_recording && !va && env.catalog_safe && !catalog_debug_translation()) {
        auto& captured = e->catalog_stage;
        if (catalog_sources.locate(env.code, env.code_avail, (u32)stage, captured.source) &&
            0x50ull + env.catalog_max_read + 8 <= captured.source.size) {
            captured.present = true; captured.attributes = env.attr_types;
            captured.render_targets = env.rt_types; captured.bound_buffer = env.bound_buffer;
            captured.options = topt;
        } else hw_log("catalog: stage %016llx unresolved in supported local resources; normal recording retained", (unsigned long long)key);
    }
    if (e->prog.spirv.empty()) {
        hw_log("vk: shader %016llx (stage %u) translated to empty SPIR-V", (unsigned long long)key, (u32)stage);
        return e;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = e->prog.spirv.size() * 4;
    ci.pCode = e->prog.spirv.data();
    if (vkCreateShaderModule(dev(), &ci, nullptr, &e->module) != VK_SUCCESS) {
        hw_log("vk: shader module creation failed (%016llx)", (unsigned long long)key);
        return e;
    }
    if (getenv("HWDER_DUMP_SHADERS")) {
        char name[128];
        snprintf(name, sizeof(name), "shader_%016llx_%u.spv", (unsigned long long)key, (u32)stage);
        if (FILE* f = fopen(name, "wb")) {
            fwrite(e->prog.spirv.data(), 4, e->prog.spirv.size(), f);
            fclose(f);
        }
        snprintf(name, sizeof(name), "shader_%016llx_%u.maxwell.txt", (unsigned long long)key, (u32)stage);
        if (FILE* f = fopen(name, "w")) {
            std::string l = shader::listing(env, stage);
            fwrite(l.data(), 1, l.size(), f);
            fclose(f);
        }
    }
    e->ok = true;
    return e;
}

GfxProgram* VkRenderer::get_program(const u32* regs, u64* key_out) {
    // Fast path: the program-selecting registers match a state seen this frame. Shader code at the
    // same address is re-validated (quick hash) once per frame per state by the slow path.
    u32 rtf[8];
    for (u32 i = 0; i < 8; i++) rtf[i] = regs[RenderTarget + i * 16 + 4];
    u64 skey = hash_bytes(regs + Pipelines, 6 * 16 * 4, 0x5151);
    skey = hash_bytes(regs + VertexAttribFormat, 32 * 4, skey);
    skey = hash_bytes(regs + ProgramRegion, 2 * 4, skey ^ regs[RtControl] ^ ((u64)regs[DepthMode] << 32));
    skey = hash_bytes(rtf, sizeof(rtf), skey ^ regs[BindlessTextureConstBufferSlot]);
    auto sit = prog_state_cache_.find(skey);
    if (sit != prog_state_cache_.end() && sit->second.frame == g_s.present_count) {
        *key_out = sit->second.key;
        return sit->second.prog;
    }
    GfxProgram* gp = get_program_slow(regs, key_out);
    if (gp) {
        if (prog_state_cache_.size() > 16384) prog_state_cache_.clear();
        prog_state_cache_[skey] = {*key_out, gp, g_s.present_count};
    }
    return gp;
}

GfxProgram* VkRenderer::get_program_slow(const u32* regs, u64* key_out) {
    u64 region = addr(regs, ProgramRegion);
    GuestEnv envs[6];
    bool enabled[6] = {};
    u64 vas[6] = {};
    shader::Options opt;
    opt.convert_depth_mode = regs[DepthMode] == 0;
    u64 key = 0x12345;
    for (u32 i = 0; i < 6; i++) {
        u32 cfg = regs[Pipelines + i * 16];
        enabled[i] = i == 1 || (cfg & 1);
        if (!enabled[i]) continue;
        u64 va = region + regs[Pipelines + i * 16 + 1];
        vas[i] = va;
        u8* code = host(va);
        if (!code) {
            if (i == 1) return nullptr;
            enabled[i] = false;
            continue;
        }
        GuestEnv& e = envs[i];
        e.code = code;
        e.code_avail = mm_->contiguous_size(va, 1 << 20);
        u32 stage = i == 0 ? 0 : i - 1;  // bind group
        e.cbuf = [this, stage](u32 index, u32 offset) -> u32 {
            if (index >= 18) return 0;
            const CbufBinding& b = cbufs_[stage][index];
            if (!b.valid || offset + 4 > b.size) return 0;
            return read_u32(b.va + offset);
        };
        e.bound_buffer = regs[BindlessTextureConstBufferSlot];
        e.tex_type = [this, regs](u32 handle) -> shader::TextureType {
            u32 tic[8];
            if (!read_tic(addr(regs, TexHeader), regs[TexHeader + 2], handle & 0xFFFFF, tic))
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
    }
    // numeric types of vertex attributes and render targets (baked into the SPIR-V)
    u64 attr_types = 0;
    for (u32 a = 0; a < 32; a++) {
        u32 raw = regs[VertexAttribFormat + a];
        if ((raw >> 6) & 1) continue;
        VkFormat f = cached_vertex_format(raw, nullptr);
        attr_types |= (u64)format_numeric_type(f) << (a * 2);
    }
    u32 rt_types = 0;
    {
        u32 ctrl = regs[RtControl];
        for (u32 i = 0; i < (ctrl & 0xF) && i < 8; i++) {
            u32 slot = (ctrl >> (4 + i * 3)) & 7;
            FormatInfo fi = rt_format(regs[RenderTarget + slot * 16 + 4]);
            if (fi.valid()) rt_types |= (u32)format_numeric_type(fi.vk) << (i * 2);
        }
    }
    envs[0].attr_types = envs[1].attr_types = attr_types;
    envs[5].rt_types = rt_types;
    // stage hashes
    // Hashing a whole program per draw is too slow: memoize by code address, validated with a hash
    // of the header and the first instructions (a rewritten shader at the same address changes those).
    u64 hashes[6] = {};
    for (u32 i = 0; i < 6; i++) {
        if (!enabled[i]) continue;
        GuestEnv& e = envs[i];
        u64 quick = hash_bytes(e.code, std::min<u64>(e.code_avail, 0x50 + 512), (u64)i);
        auto it = prog_hash_cache_.find(vas[i] ^ ((u64)i << 56));
        if (it != prog_hash_cache_.end() && it->second.first == quick) {
            hashes[i] = it->second.second;
        } else {
            hashes[i] = shader::hash_program(e, (shader::Stage)i);
            prog_hash_cache_[vas[i] ^ ((u64)i << 56)] = {quick, hashes[i]};
        }
    }
    key = hash_bytes(hashes, sizeof(hashes), opt.convert_depth_mode);
    key = hash_bytes(&attr_types, 8, key);
    key = hash_bytes(&rt_types, 4, key);
    *key_out = key;
    auto it = programs_.find(key);
    if (it != programs_.end()) return it->second;
    GfxProgram* gp = new GfxProgram;
    programs_[key] = gp;
    static const VkShaderStageFlagBits vk_stages[5] = {VK_SHADER_STAGE_VERTEX_BIT,
                                                       VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                       VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    // the last pre-rasterization stage converts the depth range
    int last_vtg = 1;
    for (int i = 2; i <= 4; i++)
        if (enabled[i]) last_vtg = i;
    for (u32 i = 1; i < 6; i++) {
        if (!enabled[i]) continue;
        shader::Options o = opt;
        if ((int)i != last_vtg) o.convert_depth_mode = false;
        if (i == 5) o.convert_depth_mode = false;
        ShaderEntry* se = get_shader(envs[i], (shader::Stage)i, o, i == 1 && enabled[0] ? &envs[0] : nullptr);
        if (!se->ok) {
            hw_log("vk: program %016llx stage %u unusable - draws skipped", (unsigned long long)key, i);
            return gp;
        }
        gp->stages[i - 1] = se;
    }
    if (!gp->stages[0]) return gp;
    gp->attributes = gp->stages[0]->prog.attributes_read;
    VkDescriptorSetLayout sets[5];
    for (u32 s = 0; s < 5; s++) {
        shader::Resources empty;
        gp->layouts[s] = stage_layout(gp->stages[s] ? gp->stages[s]->prog.resources : empty, vk_stages[s]);
        sets[s] = gp->layouts[s]->layout;
    }
    VkPushConstantRange pc{VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(shader::PushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 5;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev(), &pli, nullptr, &gp->layout));
    gp->ok = true;
    prepare_program(gp, key);
    return gp;
}

static size_t recipe_bytes(const warmup::ProgramRecord& record) {
    size_t bytes = sizeof(record);
    for (const auto& stage : record.stages) {
        if (!stage.present) continue;
        const auto& r = stage.program.resources;
        bytes += stage.program.spirv.size() * sizeof(u32) +
            r.const_buffers.size() * sizeof(shader::ConstBufferUse) +
            r.storage_buffers.size() * sizeof(shader::StorageBufferUse) +
            (r.texel_buffers.size() + r.textures.size()) * sizeof(shader::TextureUse) +
            r.images.size() * sizeof(shader::ImageUse);
    }
    return bytes;
}

void VkRenderer::ensure_warmup_records() {
    if (warmup_records_loaded_) return;
    warmup_records_loaded_ = true;
    const auto status = warmup::load(kWarmupFile, warmup_compatibility(), warmup_records_);
    if (status != warmup::Status::Ok) {
        hw_log("vk: prewarm recording starts empty (%s)", warmup::name(status));
        return;
    }
    warmup_loaded_ = warmup_records_.pipelines.size();
    warmup_storage_bytes_ = warmup_records_.pipelines.size() * sizeof(warmup::PipelineRecord);
    for (const auto& program : warmup_records_.programs) warmup_storage_bytes_ += recipe_bytes(program);
    for (u32 i = 0; i < warmup_records_.pipelines.size(); i++) {
        const auto& key = warmup_records_.pipelines[i].key;
        recipe_lookup_.emplace(hash_bytes(&key, sizeof(key)), i);
    }
    hw_log("vk: prewarm retained %zu historical recipes; prebuild %s", warmup_records_.pipelines.size(),
           prewarm_enabled() ? "not started early" : "disabled (recording enabled)");
}

void VkRenderer::start_pipeline_worker() {
    if (pipe_workers_.empty()) pipe_workers_.emplace_back([this] { pipeline_worker(); });
}

void VkRenderer::start_prewarm(warmup::Cache&& cache, u64 load_ms) {
    const u64 begin = GetTickCount64();
    warmup_start_ms_ = begin;
    warmup_records_ = std::move(cache);
    warmup_records_loaded_ = true;
    warmup_loaded_ = warmup_records_.pipelines.size();
    warmup_storage_bytes_ = warmup_records_.pipelines.size() * sizeof(warmup::PipelineRecord);
    for (const auto& program : warmup_records_.programs) warmup_storage_bytes_ += recipe_bytes(program);
    try {
        std::vector<WarmProgram*> by_index(warmup_records_.programs.size(), nullptr);
        static const VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT,
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
            VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        for (u32 i = 0; i < warmup_records_.programs.size(); i++) {
            const auto& record = warmup_records_.programs[i];
            // A valid cache can contain repeated recipes; share immutable hydration and
            // never queue two warm jobs for the same full program/static state.
            for (u32 previous = 0; previous < i; previous++) {
                const auto& candidate = warmup_records_.programs[previous];
                if (by_index[previous] && candidate.fingerprint == record.fingerprint && warmup::equal(candidate, record)) {
                    by_index[i] = by_index[previous];
                    break;
                }
            }
            if (by_index[i]) continue;
            auto program = std::make_unique<WarmProgram>();
            program->record_index = i;
            bool ok = true;
            VkDescriptorSetLayout layouts[5]{};
            for (u32 s = 0; s < 5 && ok; s++) {
                const auto& stage = record.stages[s];
                if (stage.present) {
                    auto* entry = new ShaderEntry;
                    program->gfx.stages[s] = entry;
                    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    ci.codeSize = stage.program.spirv.size() * sizeof(u32);
                    ci.pCode = stage.program.spirv.data();
                    ok = vkCreateShaderModule(dev(), &ci, nullptr, &entry->module) == VK_SUCCESS;
                    if (!ok) { entry->module = VK_NULL_HANDLE; break; }
                    entry->ok = true;
                }
                const shader::Resources empty;
                // Warm layouts are private: they cannot populate a live layout cache.
                auto* layout = stage_layout(stage.present ? stage.program.resources : empty, stages[s], false);
                program->gfx.layouts[s] = layout;
                ok = layout != nullptr;
                if (ok) layouts[s] = layout->layout;
            }
            if (ok) {
                VkPushConstantRange pc{VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(shader::PushConstants)};
                VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                ci.setLayoutCount = 5;
                ci.pSetLayouts = layouts;
                ci.pushConstantRangeCount = 1;
                ci.pPushConstantRanges = &pc;
                ok = vkCreatePipelineLayout(dev(), &ci, nullptr, &program->gfx.layout) == VK_SUCCESS;
                if (!ok) program->gfx.layout = VK_NULL_HANDLE;
            }
            if (!ok) {
                hw_log("vk: prewarm program %016llx hydration failed; live translation will be used",
                       (unsigned long long)record.key);
                continue;
            }
            program->gfx.attributes = record.stages[0].program.attributes_read;
            program->gfx.ok = true;
            by_index[i] = program.get();
            warm_programs_.push_back(std::move(program));
        }
        {
            std::lock_guard<std::mutex> l(pipe_m_);
            for (u32 i = 0; i < warmup_records_.pipelines.size(); i++) {
                const auto& record = warmup_records_.pipelines[i];
                const u64 hash = hash_bytes(&record.key, sizeof(record.key));
                recipe_lookup_.emplace(hash, i);
                WarmProgram* program = by_index[record.program_index];
                if (!program) { ++warmup_failed_; continue; }
                bool duplicate = false;
                auto [first, last] = warm_lookup_.equal_range(hash);
                for (; first != last; ++first) {
                    if (first->second->program == program && memcmp(&first->second->key, &record.key, sizeof(record.key)) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;
                auto pipeline = std::make_unique<WarmPipeline>();
                pipeline->program = program;
                pipeline->key = record.key;
                WarmPipeline* entry = pipeline.get();
                warm_pipelines_.push_back(std::move(pipeline));
                warm_lookup_.emplace(hash, entry);
                pipe_jobs_.push_back({hash, &program->gfx, record.key, GetTickCount64(), entry});
                ++warmup_queued_;
            }
        }
    } catch (const std::exception& error) {
        // No worker has started: all optional objects and partially queued jobs can
        // be discarded safely. Retain the loaded historical recipes for recording.
        pipe_jobs_.clear();
        warm_lookup_.clear();
        warm_pipelines_.clear();
        warm_programs_.clear();
        warmup_canceled_ += warmup_queued_;
        warmup_queued_ = 0;
        ++warmup_failed_;
        hw_log("vk: prewarm hydration aborted (%s); using live compilation", error.what());
        return;
    }
    try {
        std::lock_guard<std::mutex> l(pipe_m_);
        if (!pipe_jobs_.empty()) start_pipeline_worker();
        pipe_cv_.notify_one();
    } catch (const std::exception& error) {
        // emplace_back either constructed the sole worker or threw before it exists.
        // No running thread is hidden by this optional startup fallback.
        if (!pipe_workers_.empty()) throw;
        pipe_jobs_.clear();
        warm_lookup_.clear();
        warm_pipelines_.clear();
        warm_programs_.clear();
        warmup_canceled_ += warmup_queued_;
        warmup_queued_ = 0;
        ++warmup_failed_;
        hw_log("vk: prewarm worker unavailable (%s); using lazy live compilation", error.what());
        return;
    }
    hw_log("vk: prewarm startup: load %llu ms, hydrate/queue %llu ms, %zu programs, %llu recipes queued, one worker",
           (unsigned long long)load_ms, (unsigned long long)(GetTickCount64() - begin), warm_programs_.size(),
           (unsigned long long)warmup_queued_);
    log_warmup("startup_queued");
}

void VkRenderer::cancel_warm_program(WarmProgram* prog) {
    prog->canceled = true;
    for (auto& entry : warm_pipelines_) {
        if (entry->program != prog || entry->state == WarmState::Adopted || entry->state == WarmState::Canceled) continue;
        ++warmup_canceled_;
        if (entry->state != WarmState::Building) {
            if (entry->pipeline) vkDestroyPipeline(dev(), entry->pipeline, nullptr);
            entry->pipeline = VK_NULL_HANDLE;
            entry->state = WarmState::Canceled;
        }
        // An in-flight compile keeps immutable modules/layouts alive and discards its result.
    }
    pipe_jobs_.erase(std::remove_if(pipe_jobs_.begin(), pipe_jobs_.end(), [prog](const PipeJob& job) {
        return job.warm && job.warm->program == prog;
    }), pipe_jobs_.end());
    pipe_done_cv_.notify_one();
}

void VkRenderer::prepare_program(GfxProgram* prog, u64 key) {
    ensure_warmup_records();
    auto record = std::make_shared<warmup::ProgramRecord>();
    record->key = key;
    for (u32 s = 0; s < 5; s++) {
        if (!prog->stages[s]) continue;
        const auto& source = prog->stages[s]->prog;
        const auto& r = source.resources;
        if (source.spirv.size() > warmup::kMaxSpirvWords || r.const_buffers.size() > warmup::kMaxResources ||
            r.storage_buffers.size() > warmup::kMaxResources || r.texel_buffers.size() > warmup::kMaxResources ||
            r.textures.size() > warmup::kMaxResources || r.images.size() > warmup::kMaxResources) {
            hw_log("vk: prewarm program %016llx exceeds recipe limits; using live compilation", (unsigned long long)key);
            return;
        }
        record->stages[s].present = true;
        record->stages[s].program = source;
    }
    record->fingerprint = warmup::fingerprint(*record);
    prog->recipe = record;
    std::lock_guard<std::mutex> l(pipe_m_);
    for (u32 i = 0; i < warmup_records_.programs.size(); i++) {
        const auto& saved = warmup_records_.programs[i];
        if (saved.key != key || invalid_warm_programs_.count(i)) continue;
        const bool matches = saved.fingerprint == record->fingerprint && warmup::equal(saved, *record);
        if (matches && prog->recipe_index == ~0u) prog->recipe_index = i;
        else {
            invalid_warm_programs_.insert(i);
            warmup_dirty_ = true;
            ++warmup_mismatched_;
        }
        for (auto& warm : warm_programs_) {
            if (warm->record_index != i) continue;
            if (matches) prog->matched_warm = warm.get();
            else cancel_warm_program(warm.get());
        }
    }
}

void VkRenderer::record_pipeline(GfxProgram* prog, const PipelineKey& key) {
    record_catalog_pipeline(prog,key);
    ++warmup_observed_;
    if (!prog->recipe) return;
    if (warmup_records_.pipelines.size() >= warmup::kMaxPipelines) return;
    if (prog->recipe_index == ~0u) {
        if (warmup_records_.programs.size() >= warmup::kMaxPrograms) return;
        const size_t bytes = recipe_bytes(*prog->recipe);
        if (bytes + sizeof(warmup::PipelineRecord) > warmup::kMaxFileBytes ||
            warmup_storage_bytes_ > warmup::kMaxFileBytes - bytes - sizeof(warmup::PipelineRecord)) return;
        prog->recipe_index = (u32)warmup_records_.programs.size();
        warmup_records_.programs.push_back(*prog->recipe);
        warmup_storage_bytes_ += bytes;
    }
    const u64 hash = hash_bytes(&key, sizeof(key));
    auto [first, last] = recipe_lookup_.equal_range(hash);
    for (; first != last; ++first) {
        const auto& saved = warmup_records_.pipelines[first->second];
        if (saved.program_index == prog->recipe_index && memcmp(&saved.key, &key, sizeof(key)) == 0) return;
    }
    if (warmup_storage_bytes_ > warmup::kMaxFileBytes - sizeof(warmup::PipelineRecord)) return;
    warmup::PipelineRecord record;
    record.key = key;
    record.program_index = prog->recipe_index;
    record.program_fingerprint = prog->recipe->fingerprint;
    recipe_lookup_.emplace(hash, (u32)warmup_records_.pipelines.size());
    warmup_records_.pipelines.push_back(record);
    warmup_storage_bytes_ += sizeof(record);
    ++warmup_recorded_;
    warmup_dirty_ = true;
}

void VkRenderer::record_catalog_pipeline(GfxProgram* prog, const PipelineKey& key) {
    if (!catalog_recording || !prog->recipe || catalog_records_.pipelines.size() >= warmup::kMaxPipelines) return;
    catalog::Program record; record.key = key.program; record.expected_translation = prog->recipe->fingerprint;
    for (u32 i=0;i<5;i++) if (prog->stages[i]) {
        if (!prog->stages[i]->catalog_stage.present) return;
        record.stages[i] = prog->stages[i]->catalog_stage;
    }
    u32 index = ~0u;
    for (u32 i=0;i<catalog_records_.programs.size();i++) {
        const auto& p = catalog_records_.programs[i];
        if (p.key==record.key && p.expected_translation==record.expected_translation) { index=i; break; }
    }
    if (index==~0u) {
        if (catalog_records_.programs.size()>=warmup::kMaxPrograms) return;
        index=(u32)catalog_records_.programs.size(); catalog_records_.programs.push_back(std::move(record));
    }
    auto words=warmup::key_words(key);
    for (const auto& p:catalog_records_.pipelines) if(p.program==index && warmup::key_words(p.key)==words)return;
    catalog_records_.build=catalog_build(); catalog_records_.game_version=catalog_game_version();
    const char* scope=getenv("HWDER_PIPELINE_CATALOG_RECORD");
    catalog_records_.scope=scope ? scope : "unspecified";
    catalog_records_.pipelines.push_back({index,key}); catalog_dirty_=true;
}
void VkRenderer::save_catalog_records() {
    if (!catalog_dirty_) return;
    auto status=catalog::save(kCatalogFile,catalog_records_);
    if (status==catalog::Status::Ok) catalog_dirty_=false;
    hw_log("catalog: metadata export %s: %zu programs / %zu successful pipeline recipes, scope=%s; contains no shader code",
           warmup::name(status),catalog_records_.programs.size(),catalog_records_.pipelines.size(),catalog_records_.scope.c_str());
}
void VkRenderer::wait_catalog() {
    const u64 start=GetTickCount64();
    for (;;) {
        u64 done,total,success;
        {std::lock_guard<std::mutex> lock(pipe_m_);done=warmup_finished_;total=warmup_queued_;success=warmup_succeeded_;}
        prelaunch::preparation_frame("Compiling known graphics pipelines from your game...",(u32)done,(u32)total);
        if(done>=total){hw_log("catalog: preparation complete: %llu / %llu compiled successfully before guest boot",(unsigned long long)success,(unsigned long long)total);break;}
        if(GetTickCount64()-start>120000){hw_log("catalog: preparation wait expired; remaining work uses normal demand fallback");break;}
        Sleep(15);
    }
}

void VkRenderer::save_warmup_records() {
    save_catalog_records();
    if (!warmup_dirty_) return;
    const auto begin = std::chrono::steady_clock::now();
    // Preserve historical successes across early quits. Only a freshly translated
    // mismatch invalidates a historical program group; unvisited battle recipes stay.
    warmup::Cache snapshot;
    std::vector<u32> remap(warmup_records_.programs.size(), ~0u);
    for (u32 i = 0; i < warmup_records_.programs.size(); i++) {
        if (invalid_warm_programs_.count(i)) continue;
        remap[i] = (u32)snapshot.programs.size();
        snapshot.programs.push_back(warmup_records_.programs[i]);
    }
    for (const auto& record : warmup_records_.pipelines) {
        if (remap[record.program_index] == ~0u) continue;
        snapshot.pipelines.push_back(record);
        snapshot.pipelines.back().program_index = remap[record.program_index];
    }
    const auto copied = std::chrono::steady_clock::now();
    warmup::SaveTimings timings;
    const auto status = warmup::save(kWarmupFile, warmup_compatibility(), snapshot, getenv("HWDER_CACHE_PROFILE") ? &timings : nullptr);
    if (status == warmup::Status::Ok) warmup_dirty_ = false;
    hw_log("vk: prewarm save: %s, %zu recipes retained, %llu newly recorded this run",
           warmup::name(status), snapshot.pipelines.size(), (unsigned long long)warmup_recorded_);
    if (getenv("HWDER_CACHE_PROFILE")) hw_log("cache-save: snapshot %.3f ms, validation %.3f ms, serialization %.3f ms, disk %.3f ms",
        std::chrono::duration<double, std::milli>(copied-begin).count(), timings.validation_ms, timings.serialization_ms, timings.disk_ms);
}

void VkRenderer::log_warmup(const char* phase) {
    std::lock_guard<std::mutex> l(pipe_m_);
    hw_log("vk: prewarm %s: loaded %llu queued %llu completed %llu succeeded %llu adopted %llu demand_hits %llu mismatched_programs %llu failed %llu canceled %llu new_records %llu observed_successes %llu warm_pending_max_ms %llu | present %llu draws %llu elapsed %llu ms",
           phase, (unsigned long long)warmup_loaded_, (unsigned long long)warmup_queued_,
           (unsigned long long)warmup_finished_, (unsigned long long)warmup_succeeded_, (unsigned long long)warmup_adopted_,
           (unsigned long long)warmup_demand_hits_, (unsigned long long)warmup_mismatched_,
           (unsigned long long)warmup_failed_, (unsigned long long)warmup_canceled_,
           (unsigned long long)warmup_recorded_, (unsigned long long)warmup_observed_,
           (unsigned long long)warmup_pending_max_ms_,
           (unsigned long long)g_s.present_count, (unsigned long long)st_.draws,
           (unsigned long long)(warmup_start_ms_ ? GetTickCount64() - warmup_start_ms_ : 0));
}

void VkRenderer::adopt_pipeline_results() {
    for (auto& [jh, jp, prog, key] : pipe_done_) {
        pipe_pending_.erase(jh);
        if (!jp && pipe_retried_.insert(jh).second) continue;  // next draw retries synchronously once
        pipelines_[jh] = jp;
        if (jp) record_pipeline(prog, key);
    }
    pipe_done_.clear();
}

VkPipeline VkRenderer::get_pipeline(GfxProgram* prog, const PipelineKey& key) {
    u64 h = hash_bytes(&key, sizeof(key));
    {   // adopt pipelines finished by the worker threads
        std::lock_guard<std::mutex> l(pipe_m_);
        adopt_pipeline_results();
    }
    auto it = pipelines_.find(h);
    if (it != pipelines_.end()) return it->second;
    // A driver compile takes 50-250 ms. Instead of stalling the whole frame, build new pipelines on
    // worker threads and skip the draw until the pipeline exists (a one-or-two-frame pop-in, the usual
    // "async shaders" trade). HWDER_SYNC_PIPELINES=1 restores the synchronous behaviour.
    static const bool sync_pipes = getenv("HWDER_SYNC_PIPELINES") != nullptr;
    // Light frames (menus, the first frames of a scene) compile synchronously: a 100 ms hitch behind a
    // fade is invisible, while skipping their draws shows a half-drawn scene. Heavy battle frames go
    // async so a new effect's pipeline does not stall the whole frame.
    // Draws that sample a render target (screen copies, effect composites, post-processing) also
    // compile synchronously: skipping one leaves its output, or the copy a later pass reads, empty,
    // which showed as a one-frame black box the first time an attack effect was used.
    // HWDER_ASYNC_RT_PIPELINES=1 restores async compiles for them (for comparison).
    static const bool async_rt = getenv("HWDER_ASYNC_RT_PIPELINES") != nullptr;
    const bool synchronous = sync_pipes || st_.draws < 64 || pipe_retried_.count(h) || (draw_samples_rt_ && !async_rt);
    if (prog->matched_warm) {
        std::unique_lock<std::mutex> l(pipe_m_);
        auto [first, last] = warm_lookup_.equal_range(h);
        for (; first != last; ++first) {
            WarmPipeline* warm = first->second;
            if (warm->program != prog->matched_warm || memcmp(&warm->key, &key, sizeof(key)) != 0 ||
                warm->state == WarmState::Canceled || warm->state == WarmState::Adopted) continue;
            if (!warm->demanded) {
                warm->demanded = true;
                warm->demanded_ms = GetTickCount64();
                ++warmup_demand_hits_;
            }
            auto queued = std::find_if(pipe_jobs_.begin(), pipe_jobs_.end(),
                                      [warm](const PipeJob& job) { return job.warm == warm; });
            if (queued != pipe_jobs_.end() && queued != pipe_jobs_.begin()) {
                std::rotate(pipe_jobs_.begin(), queued, std::next(queued));
                pipe_cv_.notify_one();
            }
            if (synchronous) {
                pipe_done_cv_.wait(l, [warm] {
                    return warm->state != WarmState::Queued && warm->state != WarmState::Building;
                });
            }
            if (warm->state == WarmState::Ready) {
                VkPipeline pipe = warm->pipeline;
                warm->pipeline = VK_NULL_HANDLE;  // transfer exactly once into live ownership
                warm->state = WarmState::Adopted;
                ++warmup_adopted_;
                pipelines_[h] = pipe;
                record_pipeline(prog, key);
                return pipe;
            }
            if (warm->state == WarmState::Failed) {
                warm->state = WarmState::Canceled;
                pipe_retried_.insert(h);  // one synchronous retry, as for failed live async jobs
                break;
            }
            if (warm->state == WarmState::Queued || warm->state == WarmState::Building) return VK_NULL_HANDLE;
        }
    }
    if (synchronous || pipe_retried_.count(h)) {
        if (pipe_pending_.count(h)) {
            // A heavy frame may have queued this key before a light frame or RT-sampling
            // draw needs it synchronously. Reuse that job rather than create twice and
            // overwrite/leak one pipeline. Workers publish using pipe_m_ only; wait
            // releases it and is safe while the caller owns the renderer mutex.
            std::unique_lock<std::mutex> l(pipe_m_);
            auto queued = std::find_if(pipe_jobs_.begin(), pipe_jobs_.end(),
                                       [h](const PipeJob& job) { return job.hash == h; });
            if (queued != pipe_jobs_.end() && queued != pipe_jobs_.begin()) {
                // A synchronous demand should not wait behind unrelated queued keys.
                // Move the whole job so its original enqueue timestamp is preserved.
                std::rotate(pipe_jobs_.begin(), queued, std::next(queued));
                pipe_cv_.notify_one();
            }
            pipe_done_cv_.wait(l, [this, h] {
                return std::any_of(pipe_done_.begin(), pipe_done_.end(),
                                   [h](const auto& done) { return done.hash == h; });
            });
            adopt_pipeline_results();
            auto ready = pipelines_.find(h);
            if (ready != pipelines_.end()) return ready->second;
            // Failed async creation was marked for its one synchronous retry above.
        }
        VkPipeline pipe = create_pipeline(prog, key);
        pipelines_[h] = pipe;
        if (pipe) record_pipeline(prog, key);
        return pipe;
    }
    if (pipe_pending_.insert(h).second) {
        std::lock_guard<std::mutex> l(pipe_m_);
        // Real draws take priority over speculative startup recipes.
        auto speculative = std::find_if(pipe_jobs_.begin(), pipe_jobs_.end(),
                                       [](const PipeJob& job) { return job.warm && !job.warm->demanded; });
        pipe_jobs_.insert(speculative, {h, prog, key, GetTickCount64()});
        start_pipeline_worker();
        pipe_cv_.notify_one();
    }
    return VK_NULL_HANDLE;
}

void VkRenderer::pipeline_worker() {
    SetThreadDescription(GetCurrentThread(), L"HWDER pipelines");
    for (;;) {
        PipeJob job;
        {
            std::unique_lock<std::mutex> l(pipe_m_);
            pipe_cv_.wait(l, [this] { return !pipe_jobs_.empty(); });
            job = pipe_jobs_.front();
            pipe_jobs_.pop_front();
            if (job.warm) {
                if (job.warm->state == WarmState::Canceled) continue;
                job.warm->state = WarmState::Building;
            }
        }
        VkPipeline pipe = VK_NULL_HANDLE;
        try {
            pipe = create_pipeline(job.prog, job.key);
        } catch (const std::exception& e) {
            hw_log("vk: pipeline worker exception: %s", e.what());
        } catch (...) {
            hw_log("vk: pipeline worker exception");
        }
        std::lock_guard<std::mutex> l(pipe_m_);
        if (job.warm) {
            ++warmup_finished_;
            const u64 now = GetTickCount64();
            warmup_pending_max_ms_ = std::max(warmup_pending_max_ms_, now - job.enqueued_ms);
            if (job.warm->demanded)
                audit_pending_max_ms_ = std::max(audit_pending_max_ms_, now - job.warm->demanded_ms);
            if (!pipe) ++warmup_failed_;
            if (job.warm->program->canceled) {
                if (pipe) vkDestroyPipeline(dev(), pipe, nullptr);
                job.warm->state = WarmState::Canceled;
            } else {
                job.warm->pipeline = pipe;
                job.warm->state = pipe ? WarmState::Ready : WarmState::Failed;
                if (pipe) ++warmup_succeeded_;
            }
            pipe_done_cv_.notify_one();
            continue;  // unverified warm results never enter the live completion/pending maps
        }
        const u64 pending_ms = GetTickCount64() - job.enqueued_ms;
        audit_pending_max_ms_ = std::max(audit_pending_max_ms_, pending_ms);
        static bool first = true;
        if (first) {
            first = false;
            hw_log("gfx-audit: first async completion: pipeline %016llx enqueue-to-finish %llu ms success %d",
                   (unsigned long long)job.hash, (unsigned long long)pending_ms, pipe != VK_NULL_HANDLE);
        }
        pipe_done_.push_back({job.hash, pipe, job.prog, job.key});
        pipe_done_cv_.notify_one();
    }
}

VkPipeline VkRenderer::create_pipeline(GfxProgram* prog, const PipelineKey& key) {
    static const VkShaderStageFlagBits vk_stages[5] = {VK_SHADER_STAGE_VERTEX_BIT,
                                                       VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                       VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    bool tess = false;
    for (u32 s = 0; s < 5; s++) {
        if (!prog->stages[s]) continue;
        VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        si.stage = vk_stages[s];
        si.module = prog->stages[s]->module;
        si.pName = "main";
        stages.push_back(si);
        if (s == 1 || s == 2) tess = true;
    }
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attrs;
    u32 used_bindings = 0;
    for (u32 a = 0; a < 32; a++) {
        if (!key.attrib_format[a]) continue;
        attrs.push_back({a, key.attrib_binding[a], (VkFormat)key.attrib_format[a], key.attrib_offset[a]});
        used_bindings |= 1u << key.attrib_binding[a];
    }
    for (u32 b = 0; b < 32; b++)
        if (used_bindings & (1u << b)) bindings.push_back({b, key.binding_stride[b], VK_VERTEX_INPUT_RATE_VERTEX});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (u32)bindings.size();
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = (u32)attrs.size();
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = (VkPrimitiveTopology)key.topology;
    if (tess) ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    ia.primitiveRestartEnable = key.prim_restart;
    VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    ts.patchControlPoints = std::max(1u, key.patch_points);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = key.depth_clamp;
    rs.rasterizerDiscardEnable = key.discard;
    rs.polygonMode = (VkPolygonMode)key.polygon_mode;
    rs.cullMode = key.cull_enable ? key.cull_mode : VK_CULL_MODE_NONE;
    rs.frontFace = (VkFrontFace)key.front_face;
    rs.depthBiasEnable = key.depth_bias;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = key.depth_test;
    ds.depthWriteEnable = key.depth_write;
    ds.depthCompareOp = (VkCompareOp)key.depth_func;
    ds.depthBoundsTestEnable = key.depth_bounds;
    ds.stencilTestEnable = key.stencil_enable;
    ds.front = {(VkStencilOp)key.front[0], (VkStencilOp)key.front[2], (VkStencilOp)key.front[1],
                (VkCompareOp)key.front[3], 0, 0, 0};
    ds.back = {(VkStencilOp)key.back[0], (VkStencilOp)key.back[2], (VkStencilOp)key.back[1],
               (VkCompareOp)key.back[3], 0, 0, 0};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState cba[8] = {};
    for (u32 i = 0; i < key.num_colors; i++) {
        cba[i].blendEnable = key.blend_enable[i];
        cba[i].colorBlendOp = (VkBlendOp)key.blend[i][0];
        cba[i].srcColorBlendFactor = (VkBlendFactor)key.blend[i][1];
        cba[i].dstColorBlendFactor = (VkBlendFactor)key.blend[i][2];
        cba[i].alphaBlendOp = (VkBlendOp)key.blend[i][3];
        cba[i].srcAlphaBlendFactor = (VkBlendFactor)key.blend[i][4];
        cba[i].dstAlphaBlendFactor = (VkBlendFactor)key.blend[i][5];
        cba[i].colorWriteMask = key.color_mask[i];
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = key.logic_enable;
    cb.logicOp = (VkLogicOp)key.logic;
    cb.attachmentCount = key.num_colors;
    cb.pAttachments = cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_DEPTH_BIAS,        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_LINE_WIDTH,
                            VK_DYNAMIC_STATE_DEPTH_BOUNDS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = (u32)(sizeof(dyn) / sizeof(dyn[0]));
    dy.pDynamicStates = dyn;
    VkFormat cfmt[8];
    for (u32 i = 0; i < 8; i++) cfmt[i] = (VkFormat)key.color_formats[i];
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = key.num_colors;
    rci.pColorAttachmentFormats = cfmt;
    VkFormat df = (VkFormat)key.depth_format;
    rci.depthAttachmentFormat = df;
    if (df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT) rci.stencilAttachmentFormat = df;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci};
    ci.stageCount = (u32)stages.size();
    ci.pStages = stages.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pTessellationState = tess ? &ts : nullptr;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = prog->layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    SlowTimer timer("graphics pipeline creation", 30.0);
    VkResult r;
    {
        // Driver workaround: serialize graphics creation (two workers racing here
        // crashed a driver). The flags=0 cache itself is internally synchronized.
        static std::mutex create_m;
        std::lock_guard<std::mutex> l(create_m);
        r = vkCreateGraphicsPipelines(dev(), pcache_, 1, &ci, nullptr, &pipe);
    }
    if (r != VK_SUCCESS) {
        hw_log("vk: graphics pipeline creation failed (%d)", (int)r);
        pipe = VK_NULL_HANDLE;
    }
    return pipe;
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



// Farthest an object or bone may move in one tick and still be blended (world units; a running
// character moves about 10 per tick). Larger moves are teleports, respawns or a different object.
constexpr float kObjMaxMove = 80.0f;

// A 3x4 world transform (rows: rotation/scale, translation in the 4th column) in world space.
static bool is_world_3x4(const float* m) {
    // Cheapest tests first (most constant data fails on the translation column).
    float t0 = std::fabs(m[3]), t1 = std::fabs(m[7]), t2 = std::fabs(m[11]);
    if (!(t0 >= 1.0f && t0 < 1e6f && t1 >= 1.0f && t1 < 1e6f && t2 >= 1.0f && t2 < 1e6f)) return false;
    for (int r = 0; r < 3; r++) {
        float n2 = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(n2 > 0.0025f && n2 < 100.0f)) return false;
    }
    return true;
}

static bool obj_first_pos(const float* m, u64 nf, float out[3]) {
    for (u64 i = 0; i + 12 <= nf; i += 4)
        if (is_world_3x4(m + i)) {
            out[0] = m[i + 3], out[1] = m[i + 7], out[2] = m[i + 11];
            return true;
        }
    return false;
}

void VkRenderer::obj_frame_begin(u64 tag) {
    if (tag == obj_tag_) return;
    // A real frame ended: it becomes the reference for the next in-between frame.
    if (obj_tag_ && !(obj_tag_ & 1)) {
        std::swap(obj_prev_, obj_cur_);
        obj_prev_.tag = obj_tag_;
    }
    obj_cur_.reset();
    obj_used_.clear();
    obj_occ_.clear();
    obj_tag_ = tag;
    if (!(tag & 1)) obj_st_.ticks++;
    static u64 frames = 0;
    if ((tag & 1) && ++frames % 600 == 0) {
        hw_log("gi: stage c over 600 extra frames: %llu c3 blocks, %llu matched the previous frame, %llu blended "
               "(%llu matrices), %llu matrices jumped too far; cost %.2f ms per tick", (unsigned long long)obj_st_.draws,
               (unsigned long long)obj_st_.matched, (unsigned long long)obj_st_.blended, (unsigned long long)obj_st_.mats,
               (unsigned long long)obj_st_.jumps, obj_st_.ticks ? obj_st_.ms / obj_st_.ticks : 0.0);
        obj_st_ = ObjStats{};
    }
}

void VkRenderer::obj_transforms(u32 index, u8* data, u64 bytes) {
    if (cur_stage_ != 0 || index != 3 || bytes < 64 || bytes > 4096) return;  // world matrix / bone palette sizes
    struct Timer {
        double& acc;
        LARGE_INTEGER t0;
        explicit Timer(double& a) : acc(a) { QueryPerformanceCounter(&t0); }
        ~Timer() {
            static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            acc += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        }
    } timer(obj_st_.ms);
    static const bool off = getenv("HWDER_GI_NO_OBJECTS") != nullptr;
    if (off) return;
    u64 tag = gpu::exec_gi_tag();
    if (!tag) return;
    obj_frame_begin(tag);
    const u64 nf = bytes / 4;
    float pb[3];
    const bool has = obj_first_pos((const float*)data, nf, pb);
    if (!(tag & 1)) {  // real frame: remember this draw's transforms
        if (!has) return;  // nothing to blend in it
        ObjEntry e{(u32)obj_cur_.arena.size(), (u32)bytes, {pb[0], pb[1], pb[2]}, true};
        obj_cur_.arena.insert(obj_cur_.arena.end(), data, data + bytes);
        obj_cur_.idx[cur_draw_key_].push_back(e);
        return;
    }
    // In-between frame of tick n: blend against real frame n-1. Many objects share a model (the same
    // draw key), and their order changes between frames: pair each block with the previous frame's
    // block of the same key whose first world transform is nearest (each used once).
    obj_st_.draws++;
    if (!has || obj_prev_.tag != ((tag >> 1) - 1) << 1) return;
    auto it = obj_prev_.idx.find(cur_draw_key_);
    if (it == obj_prev_.idx.end() || it->second.empty()) return;
    auto& used = obj_used_[cur_draw_key_];
    if (used.size() < it->second.size()) used.resize(it->second.size(), false);  // cleared per frame, capacity kept
    // Same draw order first (the common case), else the nearest unused block among nearby occurrences.
    const u32 occ = obj_occ_[cur_draw_key_]++;
    const auto& prevs = it->second;
    auto dist = [&](size_t k) {
        const ObjEntry& e = prevs[k];
        return (e.pos[0] - pb[0]) * (e.pos[0] - pb[0]) + (e.pos[1] - pb[1]) * (e.pos[1] - pb[1]) + (e.pos[2] - pb[2]) * (e.pos[2] - pb[2]);
    };
    int best = -1;
    float bd = kObjMaxMove * kObjMaxMove;
    if (occ < prevs.size() && !used[occ] && prevs[occ].size == bytes && dist(occ) <= bd * 0.0625f) {
        best = (int)occ;  // within a quarter of the limit at the same position in the order
    } else {
        size_t lo = occ > 32 ? occ - 32 : 0, hi = std::min(prevs.size(), (size_t)occ + 33);
        if (prevs.size() <= 96) lo = 0, hi = prevs.size();
        for (size_t k = lo; k < hi; k++) {
            if (used[k] || prevs[k].size != bytes) continue;
            float d = dist(k);
            if (d < bd) bd = d, best = (int)k;
            if (d == 0.0f) break;
        }
    }
    if (best < 0) return;
    used[best] = true;
    obj_st_.matched++;
    const float* a = (const float*)(obj_prev_.arena.data() + it->second[best].off);
    float* b = (float*)data;
    auto d2 = [](const float* x, const float* y) {
        float dx = x[3] - y[3], dy = x[7] - y[7], dz = x[11] - y[11];
        return dx * dx + dy * dy + dz * dz;
    };
    // The previous block's transforms, listed once (offset in floats); the fallback search only walks these.
    u32 prev_list[1024];
    u32 nprev = 0;
    bool listed = false;  // built on the first fallback only
    bool any = false;
    for (u64 i = 0; i + 12 <= nf; i += 4) {
        if (!is_world_3x4(b + i)) continue;
        // The same slot first (a bone palette keeps its order); else the nearest transform in the
        // previous block (blocks of instances may reorder them).
        const float* src = nullptr;
        if (is_world_3x4(a + i) && d2(a + i, b + i) <= kObjMaxMove * kObjMaxMove) {
            src = a + i;
        } else {
            if (!listed) {
                listed = true;
                for (u64 j = 0; j + 12 <= nf && nprev < 1024; j += 4)
                    if (is_world_3x4(a + j)) prev_list[nprev++] = (u32)j, j += 8;
            }
            float bd2 = kObjMaxMove * kObjMaxMove;
            for (u32 k = 0; k < nprev; k++) {
                float d = d2(a + prev_list[k], b + i);
                if (d < bd2) bd2 = d, src = a + prev_list[k];
            }
        }
        if (!src) {  // nothing near enough: a teleport, a respawn or a new instance - keep tick n
            obj_st_.jumps++;
            i += 8;
            continue;
        }
        for (int k = 0; k < 12; k++) b[i + k] = src[k] + (b[i + k] - src[k]) * 0.5f;
        obj_st_.mats++;
        any = true;
        i += 8;  // the rest of this matrix
    }
    if (any) obj_st_.blended++;
}

// HWDER_CBUF_DUMP=<present number> (stage c research): for every draw of that frame, log each bound
// constant buffer: size, where the newest tick's camera viewProj/view sit (4 layouts), and the offsets
// of 3x4 affine transforms (three rows of rotation/scale with a translation in the 4th column).
void VkRenderer::cbuf_dump(u32 index, u64 va, const u8* data, u64 bytes) {
    static const u64 target = getenv("HWDER_CBUF_DUMP") ? strtoull(getenv("HWDER_CBUF_DUMP"), nullptr, 10) : 0;
    if (!target || g_s.present_count != target) return;
    static u64 last_pc = 0;
    static u32 lines = 0;
    if (lines > 4000) return;
    interp::CamSnap cs[1];
    bool cam = interp::snapshots(cs, 1) == 1;
    const float* f = (const float*)data;
    u64 nf = bytes / 4;
    std::string camhits, aff;
    if (cam) {
        float vpt[16], vt[16];
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) vpt[r * 4 + k] = cs[0].viewproj[k * 4 + r], vt[r * 4 + k] = cs[0].view[k * 4 + r];
        const float* pats[4] = {cs[0].viewproj, vpt, cs[0].view, vt};
        const char* names[4] = {"VP", "VPt", "V", "Vt"};
        for (u64 i = 0; i + 12 <= nf; i++)
            for (int k = 0; k < 4; k++)
                if (!memcmp(f + i, pats[k], 48)) camhits += " " + std::string(names[k]) + "@" + std::to_string(i * 4);
    }
    int naff = 0;
    for (u64 i = 0; i + 12 <= nf; i += 4) {
        const float* m = f + i;
        bool ok = true;
        for (int r = 0; r < 3 && ok; r++) {
            float n2 = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
            ok = n2 > 0.04f && n2 < 25.0f && std::isfinite(m[r * 4 + 3]);
        }
        if (ok && (std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11])) > 10.0f) {
            if (naff < 8) aff += " " + std::to_string(i * 4);
            naff++;
            i += 8;  // skip the rest of this matrix
        }
    }
    hw_log("cbufdump: draw %u key %016llx stage %d c%u va %llx %llu bytes | camera%s | %d affine 3x4:%s%s", dump_draw_,
           (unsigned long long)cur_draw_key_, cur_stage_, index, (unsigned long long)va, (unsigned long long)bytes,
           camhits.empty() ? " -" : camhits.c_str(), naff, aff.c_str(), naff > 8 ? " ..." : "");
    lines++;
    (void)last_pc;
    // Raw floats of the first few vertex-stage c3 blocks of each size (object / bone transforms).
    static std::map<u64, int> shown;
    if (cur_stage_ == 0 && index == 3 && shown[bytes]++ < 3) {
        std::string row;
        for (u64 i = 0; i < std::min<u64>(nf, 40); i++) {
            char b[32];
            snprintf(b, sizeof b, "%s%g", (i % 4) ? " " : " | ", f[i]);
            row += b;
        }
        hw_log("cbufdump:   floats%s", row.c_str());
        if (cam) {
            const float* v = cs[0].viewproj;
            hw_log("cbufdump:   camera viewProj rows: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", v[0], v[1], v[2], v[3],
                   v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
        }
    }
}

// Fill descriptor set `set` for a program's resources. cbuf(index, offset) reads bound cbuf words;
// cb_range(index, va_lo/hi...) gives the bound range of c[index]. pools: [tic va, tic limit, tsc va, tsc limit].
bool VkRenderer::bind_resources(const shader::Resources& res, StageLayout* sl, VkDescriptorSet set,
                                const std::function<u32(u32, u32)>& cbuf,
                                const std::function<bool(u32, u64&, u32&)>& cb_range, const u32* pools,
                                bool linked_tsc, std::vector<std::function<void()>>& post, StageRec* rec_out) {
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
        // Stage c: vertex-stage c3 holds the object/bone transforms; build it in cached memory (the staging
        // slice is write-combined: reading it back is very slow), blend, then write it out once.
        const bool obj = bound && obj_candidate(c.index, bytes);
        alignas(16) static thread_local u8 obj_buf[4096];
        if (obj) {
            u64 n = std::min<u64>(bytes, size);
            mm_->read(va, obj_buf, n);
            if (n < bytes) memset(obj_buf + n, 0, bytes - n);
            obj_transforms(c.index, obj_buf, bytes);
            memcpy(s.ptr, obj_buf, bytes);
        } else if (bound) {
            u64 n = std::min<u64>(bytes, size);
            mm_->read(va, s.ptr, n);
            if (n < bytes) memset(s.ptr + n, 0, bytes - n);
        } else {
            memset(s.ptr, 0, bytes);
        }
        dw.buffers.push_back({s.buf, s.offset, bytes});
        add_write(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1).pBufferInfo = &dw.buffers.back();
        if (rec_out) rec_out->cbufs.push_back({(u32)dw.buffers.size() - 1, std::vector<u8>(s.ptr, s.ptr + bytes)});
        cbuf_dump(c.index, va, s.ptr, bytes);
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
        if (src && avail >= 48) cbuf_dump(100 + sb.cbuf_index, va, src, std::min<u64>(avail, size));
        if (sb.written && src) {
            g_s.replay_unsafe = true;
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
            const u8* rec_src = nullptr;
            u64 rec_bytes = 0;
            VkFormat rec_fmt = VK_FORMAT_R32_UINT;
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
                    rec_src = s.ptr;
                    rec_bytes = bytes;
                    rec_fmt = fi.vk;
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
            if (rec_out) {
                TexelRec tr{(u32)dw.texels.size() - 1, rec_fmt, {}};
                if (rec_src && rec_bytes) tr.data.assign(rec_src, rec_src + rec_bytes);
                else tr.data.assign(16, 0);
                rec_out->texels_rec.push_back(std::move(tr));
            }
        }
        w.pTexelBufferView = &dw.texels[first];
    }
    // sampled textures
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
            static bool white = getenv("HWDER_VK_WHITE_TEX") != nullptr;
            ImageRef iref_outer;
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
                ImageRef& iref = iref_outer;
                if (te) {
                    st_.tic_hit++;
                    tc_.validate(te->img);
                    if (te->view) ii.imageView = te->view;
                    iref = {te->img, te->vp};
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
                ProfScope* tps = new ProfScope(kProfTexGet);
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
                    delete tps;
                    tps = new ProfScope(kProfTexView);
                    VkImageView v = tc_.view(img, info.fmt.vk, vt, sub.level + bl, nl, sub.layer + blayer, layers, swz,
                                             aspect);
                    if (v) ii.imageView = v;
                    iref = {img, {info.fmt.vk, vt, sub.level + bl, nl, sub.layer + blayer, layers, swz, aspect}};
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
                        static bool dump_tex_bytes = getenv("HWDER_VK_DUMP_TEX") != nullptr;
                        if (dump_tex_bytes && img->readable) {
                            CreateDirectoryA("fd", nullptr);
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
                delete tps;
                }
            }
            {
                ProfScope sps(kProfSampler);
                if (read_tic(((u64)pools[3] << 32) | pools[4], pools[5], tsc_id, tsc)) ii.sampler = tc_.sampler(tsc);
            }
            if (rec_out) rec_out->image_refs.push_back(iref_outer);
            dw.images.push_back(ii);
        }
        w.pImageInfo = &dw.images[first];
    }
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
        if (rec_out) rec_out->image_refs.push_back({});
        dw.images.push_back(ii);
        w.pImageInfo = &dw.images.back();
    }
    if (rec_out) {
        rec_out->layout = sl->layout;
        rec_out->buffers = dw.buffers;
        rec_out->images = dw.images;
        rec_out->texels = dw.texels;
        rec_out->writes.clear();
        for (const VkWriteDescriptorSet& w : dw.writes) {
            u32 first = 0;
            if (w.pBufferInfo) first = (u32)(w.pBufferInfo - dw.buffers.data());
            else if (w.pImageInfo) first = (u32)(w.pImageInfo - dw.images.data());
            else if (w.pTexelBufferView) first = (u32)(w.pTexelBufferView - dw.texels.data());
            rec_out->writes.push_back({w.descriptorType, w.dstBinding, w.descriptorCount, first});
        }
    }
    {
        ProfScope ups(kProfDescUpdate);
        if (!dw.writes.empty()) vkUpdateDescriptorSets(dev(), (u32)dw.writes.size(), dw.writes.data(), 0, nullptr);
    }
    return true;
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
    OpRec* oprec = nullptr;
    if (recording_) {
        rec_cur_.emplace_back();
        oprec = &rec_cur_.back();
        oprec->is_clear = false;
        oprec->serial = g_rec.serial();
    }
    u32 pools[6] = {regs[TexHeader], regs[TexHeader + 1], regs[TexHeader + 2],
                    regs[TexSampler], regs[TexSampler + 1], regs[TexSampler + 2]};
    bool linked = regs[SamplerBinding] == 1;
    {
        u64 kw[5] = {prog_key, addr(regs, VertexStreams + 1), addr(regs, IndexBuffer), ((u64)p.count << 32) | p.first, p.topology};
        cur_draw_key_ = hash_bytes(kw, sizeof(kw), 0x1e1e);
        dump_draw_++;
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
        bind_resources(prog->stages[s]->prog.resources, sl, sets[s], cbuf, range, pools, linked, post, oprec ? &oprec->draw.stages[s] : nullptr);
        if (oprec) oprec->draw.has_stage[s] = true;
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
        if (oprec) rec_cur_.pop_back();
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
    if (oprec) {
        DrawRec& d = oprec->draw;
        d.fb = fb;
        d.pipe = pipe;
        d.layout = prog->layout;
        memcpy(d.vb, vbufs, sizeof(d.vb));
        memcpy(d.vo, voffs, sizeof(d.vo));
        d.streams_used = streams_used;
        d.need_const = need_const;
        d.vp = rs_.vp;
        d.sc = rs_.sc;
        memcpy(d.bias, rs_.bias, sizeof(d.bias));
        memcpy(d.bc, rs_.bc, sizeof(d.bc));
        memcpy(d.st, rs_.st, sizeof(d.st));
        d.lw = rs_.lw;
        memcpy(d.bounds, rs_.bounds, sizeof(d.bounds));
        d.pc = pc;
        d.indexed = indexed;
        d.ib = ibuf;
        d.io = ioff;
        d.it = itype;
        d.count = count;
        d.first_vertex = first_vertex;
        d.base_instance = p.base_instance;
        d.vertex_offset = vertex_offset;
        for (u32 i = 0; i < 8; i++) d.color_mask[i] = key.color_mask[i];
        d.depth_write = key.depth_write || key.stencil_enable;
        u64 kw[5] = {prog_key, addr(regs, VertexStreams + 1), addr(regs, IndexBuffer), ((u64)p.count << 32) | p.first, p.topology};
        d.match_key = hash_bytes(kw, sizeof(kw), 0x1e1e);
    }
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
    if (recording_) {
        rec_cur_.emplace_back();
        OpRec& o = rec_cur_.back();
        o.is_clear = true;
        o.serial = g_rec.serial();
        o.clear.fb = fb;
        o.clear.att = att;
        o.clear.rect = r;
        o.clear.rt = rt;
        o.clear.color = color && rt < fb.num_colors && fb.color_view[rt];
        o.clear.depth = (z || s) && fb.depth_view;
    }
    begin_rendering(fb);
    vkCmdClearAttachments(g_rec.cmd_in_pass(), (u32)att.size(), att.data(), 1, &r);
}

// ------------------------------------------------------------------ frame interpolation replay
// Lerp two staged constant buffers. Only values that look like ordinary floats in both frames move;
// integers, flags and anything that jumps are taken from the new frame.
static void lerp_cbuf(const u8* a, const u8* b, u8* out, size_t n, float t) {
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        u32 ua, ub;
        memcpy(&ua, a + i * 4, 4);
        memcpy(&ub, b + i * 4, 4);
        float fa, fb_;
        memcpy(&fa, &ua, 4);
        memcpy(&fb_, &ub, 4);
        u32 r = ub;
        if (ua != ub && std::isfinite(fa) && std::isfinite(fb_)) {
            float ma = fabsf(fa), mb = fabsf(fb_);
            bool floaty_a = fa == 0.0f || (ma > 1e-12f && ma < 1e12f);
            bool floaty_b = fb_ == 0.0f || (mb > 1e-12f && mb < 1e12f);
            if (floaty_a && floaty_b && fabsf(fa - fb_) <= 4.0f * std::max(ma, mb) + 16.0f) {
                float v = fa + (fb_ - fa) * t;
                memcpy(&r, &v, 4);
            }
        }
        memcpy(out + i * 4, &r, 4);
    }
    if (n % 4) memcpy(out + words * 4, b + words * 4, n % 4);
}

Image* VkRenderer::shadow_of(Image* img) {
    if (!img) return nullptr;
    auto it = shadows_.find(img);
    if (it != shadows_.end()) return it->second;
    Image* sh = tc_.make_shadow(img);
    shadows_[img] = sh;
    return sh;
}

Framebuffer VkRenderer::shadow_fb(const Framebuffer& fb) {
    Framebuffer f = fb;
    for (u32 i = 0; i < fb.num_colors; i++) {
        if (!fb.color[i]) continue;
        f.color[i] = shadow_of(fb.color[i]);
        f.color_view[i] = tc_.view(f.color[i], fb.color_format[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1);
    }
    if (fb.depth) {
        f.depth = shadow_of(fb.depth);
        f.depth_view = tc_.view(f.depth, f.depth->info.fmt.vk, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1);
    }
    return f;
}

// Give every shadow the current contents of its real target (targets that accumulate across frames,
// e.g. the exposure ping-pong, need their history; the rest is cleared by the replayed frame anyway).
void VkRenderer::sync_shadows(const std::vector<OpRec>& ops) {
    std::vector<Image*> done;
    auto sync = [&](Image* img) {
        if (!img || std::find(done.begin(), done.end(), img) != done.end()) return;
        done.push_back(img);
        Image* sh = shadow_of(img);
        VkImageCopy c{};
        c.srcSubresource = {img->aspect, 0, 0, 1};
        c.dstSubresource = {sh->aspect, 0, 0, 1};
        c.extent = {img->level_width(0), img->level_height(0), 1};
        vkCmdCopyImage(g_rec.cmd(), img->image, VK_IMAGE_LAYOUT_GENERAL, sh->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    };
    full_barrier(g_rec.cmd());
    for (const OpRec& o : ops) {
        const Framebuffer& fb = o.is_clear ? o.clear.fb : o.draw.fb;
        for (u32 i = 0; i < fb.num_colors; i++) sync(fb.color[i]);
        sync(fb.depth);
    }
    full_barrier(g_rec.cmd());
}

void VkRenderer::replay_clear(const ClearRec& c) {
    Framebuffer sfb = shadow_fb(c.fb);
    begin_rendering(sfb);
    VkClearRect r = c.rect;
    vkCmdClearAttachments(g_rec.cmd_in_pass(), (u32)c.att.size(), c.att.data(), 1, &r);
}

void VkRenderer::replay_draw(const DrawRec& d, const DrawRec* prev, float t) {
    VkDescriptorSet sets[5] = {};
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufs;
    const auto& props = display::device().props.limits;
    for (u32 s = 0; s < 5; s++) {
        if (!d.has_stage[s]) continue;
        const StageRec& sr = d.stages[s];
        sets[s] = alloc_set(sr.layout);
        bufs = sr.buffers;
        std::vector<VkBufferView> texels = sr.texels;
        std::vector<VkDescriptorImageInfo> images = sr.images;
        // sampled render targets read the shadow twin (the real one may already hold the next frame)
        for (size_t ii = 0; ii < images.size() && ii < sr.image_refs.size(); ii++) {
            const ImageRef& r = sr.image_refs[ii];
            if (!r.img || !shadows_.count(r.img)) continue;
            VkImageView v = tc_.view(shadows_[r.img], r.vp.fmt, r.vp.vt, r.vp.base_level, r.vp.levels, r.vp.base_layer, r.vp.layers, r.vp.swz, r.vp.aspect);
            if (v) images[ii].imageView = v;
        }
        for (size_t ti = 0; ti < sr.texels_rec.size(); ti++) {
            const TexelRec& tr = sr.texels_rec[ti];
            const TexelRec* ptr_prev = (prev && prev->has_stage[s] && ti < prev->stages[s].texels_rec.size() &&
                                        prev->stages[s].texels_rec[ti].data.size() == tr.data.size() &&
                                        prev->stages[s].texels_rec[ti].fmt == tr.fmt)
                                           ? &prev->stages[s].texels_rec[ti]
                                           : nullptr;
            Slice sl = g_rec.stage(std::max<u64>(tr.data.size(), 16), std::max<u64>(props.minTexelBufferOffsetAlignment, 16));
            if (ptr_prev) lerp_cbuf(ptr_prev->data.data(), tr.data.data(), sl.ptr, tr.data.size(), t);
            else memcpy(sl.ptr, tr.data.data(), tr.data.size());
            VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
            ci.buffer = sl.buf;
            ci.format = tr.fmt;
            ci.offset = sl.offset;
            ci.range = tr.data.size();
            VkBufferView v = VK_NULL_HANDLE;
            if (vkCreateBufferView(dev(), &ci, nullptr, &v) == VK_SUCCESS) {
                g_rec.defer([v] { vkDestroyBufferView(dev(), v, nullptr); });
                texels[tr.index] = v;
            }
        }
        // interpolated constant buffers: new staging slices
        for (size_t ci = 0; ci < sr.cbufs.size(); ci++) {
            const CbufRec& cr = sr.cbufs[ci];
            const CbufRec* pr = (prev && prev->has_stage[s] && ci < prev->stages[s].cbufs.size() &&
                                 prev->stages[s].cbufs[ci].data.size() == cr.data.size())
                                    ? &prev->stages[s].cbufs[ci]
                                    : nullptr;
            if (cam_replay_) {  // camera mode: the frame's bytes with only the camera matrices replaced
                if (cr.cam_hits.empty()) continue;
                Slice sl = g_rec.stage(std::max<u64>(cr.data.size(), 16), props.minUniformBufferOffsetAlignment);
                memcpy(sl.ptr, cr.data.data(), cr.data.size());
                for (const CamHit& h : cr.cam_hits)
                    memcpy(sl.ptr + h.word * 4, cam_words_[h.mat][h.layout], (h.layout < 2 ? 16 : 12) * 4);
                bufs[cr.buffer_index] = {sl.buf, sl.offset, (VkDeviceSize)cr.data.size()};
                continue;
            }
            if (!pr) continue;  // unchanged binding: the original slice is pinned in the ring
            Slice sl = g_rec.stage(std::max<u64>(cr.data.size(), 16), props.minUniformBufferOffsetAlignment);
            lerp_cbuf(pr->data.data(), cr.data.data(), sl.ptr, cr.data.size(), t);
            bufs[cr.buffer_index] = {sl.buf, sl.offset, (VkDeviceSize)cr.data.size()};
        }
        writes.clear();
        for (const WriteRec& w : sr.writes) {
            VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            ws.dstSet = sets[s];
            ws.dstBinding = w.binding;
            ws.descriptorType = w.type;
            ws.descriptorCount = w.count;
            switch (w.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: ws.pBufferInfo = &bufs[w.first]; break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: ws.pTexelBufferView = &texels[w.first]; break;
            default: ws.pImageInfo = &images[w.first]; break;
            }
            writes.push_back(ws);
        }
        if (!writes.empty()) vkUpdateDescriptorSets(dev(), (u32)writes.size(), writes.data(), 0, nullptr);
    }
    Framebuffer sfb = shadow_fb(d.fb);
    begin_rendering(sfb);
    VkCommandBuffer cb = g_rec.cmd_in_pass();
    if (rs_.serial != g_rec.serial()) {
        rs_ = RecState{};
        rs_.serial = g_rec.serial();
    }
    if (rs_.pipe != d.pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe);
        rs_.pipe = d.pipe;
    }
    for (u32 s = 0; s < 5; s++)
        if (sets[s]) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, s, 1, &sets[s], 0, nullptr);
    for (u32 s = 0; s < 16; s++)
        if ((d.streams_used & (1u << s)) && (rs_.vb[s] != d.vb[s] || rs_.vo[s] != d.vo[s])) {
            vkCmdBindVertexBuffers(cb, s, 1, &d.vb[s], &d.vo[s]);
            rs_.vb[s] = d.vb[s];
            rs_.vo[s] = d.vo[s];
        }
    if (d.need_const && (rs_.vb[kConstBinding] != d.vb[kConstBinding] || rs_.vo[kConstBinding] != d.vo[kConstBinding])) {
        vkCmdBindVertexBuffers(cb, kConstBinding, 1, &d.vb[kConstBinding], &d.vo[kConstBinding]);
        rs_.vb[kConstBinding] = d.vb[kConstBinding];
        rs_.vo[kConstBinding] = d.vo[kConstBinding];
    }
    if (memcmp(&rs_.vp, &d.vp, sizeof(d.vp)) != 0) { vkCmdSetViewport(cb, 0, 1, &d.vp); rs_.vp = d.vp; }
    if (memcmp(&rs_.sc, &d.sc, sizeof(d.sc)) != 0) { vkCmdSetScissor(cb, 0, 1, &d.sc); rs_.sc = d.sc; }
    if (memcmp(rs_.bias, d.bias, sizeof(d.bias)) != 0) { vkCmdSetDepthBias(cb, d.bias[0], d.bias[1], d.bias[2]); memcpy(rs_.bias, d.bias, sizeof(d.bias)); }
    if (memcmp(rs_.bc, d.bc, 16) != 0) { vkCmdSetBlendConstants(cb, d.bc); memcpy(rs_.bc, d.bc, 16); }
    if (memcmp(rs_.st, d.st, sizeof(d.st)) != 0) {
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, d.st[0]);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, d.st[1]);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, d.st[2]);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, d.st[3]);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, d.st[4]);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, d.st[5]);
        memcpy(rs_.st, d.st, sizeof(d.st));
    }
    if (rs_.lw != d.lw) { vkCmdSetLineWidth(cb, d.lw); rs_.lw = d.lw; }
    if (memcmp(rs_.bounds, d.bounds, sizeof(d.bounds)) != 0) { vkCmdSetDepthBounds(cb, d.bounds[0], d.bounds[1]); memcpy(rs_.bounds, d.bounds, sizeof(d.bounds)); }
    if (rs_.pc_layout != d.layout || memcmp(&rs_.pc, &d.pc, sizeof(d.pc)) != 0) {
        vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(d.pc), &d.pc);
        rs_.pc_layout = d.layout;
        rs_.pc = d.pc;
    }
    if (d.indexed) {
        if (rs_.ib != d.ib || rs_.io != d.io || rs_.it != d.it) {
            vkCmdBindIndexBuffer(cb, d.ib, d.io, d.it);
            rs_.ib = d.ib;
            rs_.io = d.io;
            rs_.it = d.it;
        }
        vkCmdDrawIndexed(cb, d.count, 1, 0, d.vertex_offset, d.base_instance);
    } else {
        vkCmdDraw(cb, d.count, 1, d.first_vertex, d.base_instance);
    }
}

void VkRenderer::replay_frame(float t) {
    // match draws of the previous frame by signature, in order of occurrence
    std::unordered_map<u64, std::vector<const DrawRec*>> prev_by_key;
    if (!cam_replay_)  // camera mode blends nothing from the previous frame
        for (const OpRec& o : rec_prev_)
            if (!o.is_clear) prev_by_key[o.draw.match_key].push_back(&o.draw);
    std::unordered_map<u64, u32> used;
    sync_shadows(rec_frame_);
    for (const OpRec& o : rec_frame_) {
        if (o.is_clear) {
            replay_clear(o.clear);
            continue;
        }
        const DrawRec* prev = nullptr;
        auto it = prev_by_key.find(o.draw.match_key);
        if (it != prev_by_key.end()) {
            u32& n = used[o.draw.match_key];
            if (n < it->second.size()) prev = it->second[n++];
        }
        replay_draw(o.draw, prev, t);
    }
}

// The camera matrix in layout l: 0 as stored (row-major 4x4), 1 transposed, 2 the first three rows
// as stored, 3 the first three rows of the transpose (a column-major 3x4).
static void cam_layout(const float m[16], int l, u32 out[16]) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) t[r * 4 + c] = (l & 1) ? m[c * 4 + r] : m[r * 4 + c];
    memcpy(out, t, (l < 2 ? 16 : 12) * 4);
}

// HWDER_CAM_INV=1: also replace the inverse view (matV2W) in replays. Off by default: it is
// searched and logged either way.
static bool cam_inv_enabled() {
    static const bool on = getenv("HWDER_CAM_INV") && atoi(getenv("HWDER_CAM_INV")) != 0;
    return on;
}

// Which tick's camera this frame was drawn with: exact search of every constant buffer for each
// recorded snapshot's view, viewProj and inverse view (all layouts). The snapshot with the most
// view/viewProj hits wins; its hits are stored on the CbufRecs for replay_draw to overwrite.
void VkRenderer::cam_identify() {
    cam_frame_ = CamFrame{};
    interp::CamSnap snaps[8];
    int ns = interp::snapshots(snaps, 8);
    struct Pat {
        u32 w[16];
        u8 len, snap, mat, layout;
    };
    std::vector<Pat> pats;
    for (int i = 0; i < ns; i++) {
        float inv[16];
        interp::view_inverse(snaps[i].view, inv);
        const float* mats[kCamMats] = {snaps[i].view, snaps[i].viewproj, inv};
        for (int m = 0; m < kCamMats; m++)
            for (int l = 0; l < kCamLayouts; l++) {
                Pat p{};
                p.len = l < 2 ? 16 : 12;
                p.snap = (u8)i, p.mat = (u8)m, p.layout = (u8)l;
                cam_layout(mats[m], l, p.w);
                // Skip near-trivial matrices (an identity-like view would match every identity model matrix).
                int distinct = 0;
                for (int k = 0; k < p.len; k++) {
                    float fl;
                    memcpy(&fl, &p.w[k], 4);
                    if (fl != 0.0f && fl != 1.0f && fl != -1.0f) distinct++;
                }
                if (distinct >= 4) pats.push_back(p);
            }
    }
    // 16-word layouts before their 12-word prefixes
    std::sort(pats.begin(), pats.end(), [](const Pat& a, const Pat& b) { return a.w[0] != b.w[0] ? a.w[0] < b.w[0] : a.len > b.len; });
    u64 bloom[64] = {};
    auto bslot = [](u32 w) { return (u32)((w * 0x9E3779B1u) >> 20); };  // 4096 bits
    for (const Pat& p : pats) bloom[bslot(p.w[0]) >> 6] |= 1ull << (bslot(p.w[0]) & 63);
    struct Found {
        CbufRec* cr;
        u32 op;
        CamHit h;
        u8 snap;
    };
    std::vector<Found> found;
    u32 per_snap[8] = {};
    u64 bytes = 0, nbufs = 0;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (u32 oi = 0; oi < rec_frame_.size(); oi++) {
        OpRec& o = rec_frame_[oi];
        if (o.is_clear) continue;
        for (u32 s = 0; s < 5; s++) {
            if (!o.draw.has_stage[s]) continue;
            for (CbufRec& cr : o.draw.stages[s].cbufs) {
                cr.cam_hits.clear();
                if (pats.empty()) continue;
                nbufs++;
                bytes += cr.data.size();
                const u32* w = (const u32*)cr.data.data();
                size_t n = cr.data.size() / 4;
                for (size_t i = 0; i + 12 <= n; i++) {
                    u32 b = bslot(w[i]);
                    if (!(bloom[b >> 6] & (1ull << (b & 63)))) continue;
                    auto it = std::lower_bound(pats.begin(), pats.end(), w[i], [](const Pat& p, u32 v) { return p.w[0] < v; });
                    for (; it != pats.end() && it->w[0] == w[i]; ++it) {
                        if (i + it->len > n || memcmp(w + i, it->w, it->len * 4) != 0) continue;
                        found.push_back({&cr, oi, {(u32)i, it->mat, it->layout}, it->snap});
                        if (it->mat != kCamInv) per_snap[it->snap]++;
                        i += it->len - 1;
                        break;
                    }
                }
            }
        }
    }
    QueryPerformanceCounter(&t1);
    int best = -1;
    for (int i = 0; i < ns; i++)
        if (per_snap[i] && (best < 0 || per_snap[i] > per_snap[best])) best = i;
    u32 hits[kCamMats][kCamLayouts] = {};
    u32 other_hits[kCamMats] = {}, other_older = 0, other_newer = 0;
    std::vector<u8> op_hit(rec_frame_.size(), 0);  // bit m: the draw holds matrix m of the frame's tick
    if (best >= 0) {
        cam_frame_.matched = true;
        cam_frame_.cur = snaps[best];
        for (int i = 0; i < ns; i++)
            if (snaps[i].tick + 1 == snaps[best].tick) {
                cam_frame_.have_prev = true;
                cam_frame_.prev = snaps[i];
            }
        for (const Found& fd : found) {
            if (fd.snap == best) {
                if (fd.h.mat != kCamInv || cam_inv_enabled()) fd.cr->cam_hits.push_back(fd.h);
                hits[fd.h.mat][fd.h.layout]++;
                op_hit[fd.op] |= 1 << fd.h.mat;
            } else {
                other_hits[fd.h.mat]++;
                (snaps[fd.snap].tick < snaps[best].tick ? other_older : other_newer)++;
            }
        }
        // Presented frames should use consecutive ticks. A tick that goes backwards or repeats means a
        // frame was drawn with another tick's camera (the one-frame camera jumps at native 60 fps).
        static u64 last_tick = 0;
        static bool have_last = false;
        u64 tk = snaps[best].tick;
        if (have_last && interp::cam_log_every() > 0 && tk != last_tick + 1)
            hw_log("cam: order: frame drawn with tick %llu after tick %llu (%s; newest tick %llu, %u other-tick hits)",
                   (unsigned long long)tk, (unsigned long long)last_tick,
                   tk <= last_tick ? (tk == last_tick ? "repeat" : "BACKWARDS") : "skip", (unsigned long long)snaps[0].tick,
                   other_hits[0] + other_hits[1] + other_hits[2]);
        last_tick = tk;
        have_last = true;
    }
    // Per-frame anomaly check: a frame whose view or viewProj hit count drops well below the running
    // average was drawn (at least partly) with matrices that are not any recorded tick's.
    if (interp::cam_log_every() > 0) {
        u32 hv = 0, hvp = 0;
        for (int l = 0; l < kCamLayouts; l++) hv += hits[kCamView][l], hvp += hits[kCamViewProj][l];
        static double avg_v = 0, avg_vp = 0;
        if (avg_v > 50 && (hv < avg_v * 0.6 || hvp < avg_vp * 0.6))
            hw_log("cam: anomaly: present %llu: view hits %u (avg %.0f), viewProj hits %u (avg %.0f), %u draws, tick %llu, other-tick hits %u/%u/%u",
                   (unsigned long long)g_s.present_count, hv, avg_v, hvp, avg_vp, (u32)rec_frame_.size(),
                   best >= 0 ? (unsigned long long)snaps[best].tick : 0ull, other_hits[0], other_hits[1], other_hits[2]);
        avg_v = avg_v * 0.9 + hv * 0.1;
        avg_vp = avg_vp * 0.9 + hvp * 0.1;
    }
    if (interp::cam_log_every() <= 0) return;
    // ---- HWDER_CAM_LOG diagnostics, once per second
    // Draws: how many carry the tick's view/viewProj (moved by the replay) or only its inverse view
    // (whose world reconstruction then disagrees with the moved geometry unless HWDER_CAM_INV=1).
    u32 draws = 0, draws_cam = 0, draws_inv_only = 0;
    for (u32 oi = 0; oi < rec_frame_.size(); oi++) {
        if (rec_frame_[oi].is_clear) continue;
        draws++;
        if (op_hit[oi] & 3) draws_cam++;
        else if (op_hit[oi] & 4) draws_inv_only++;
    }
    // Near matches: 3D draws (depth bound) with no exact camera hit, whose constant buffers hold a run
    // of 12+ floats within 0.1% of the tick's view / viewProj (any layout) - matrices the game builds
    // slightly differently, which a replay leaves at the tick's camera. Logged per second with the
    // most common draw signatures.
    static u64 near_draws = 0, unmatched_3d = 0, near_runs[2][kCamLayouts] = {};
    static std::unordered_map<u64, u32> near_keys, plain_keys;
    if (best >= 0) {
        u32 pat[2][kCamLayouts][16];
        for (int l = 0; l < kCamLayouts; l++) {
            cam_layout(snaps[best].view, l, pat[0][l]);
            cam_layout(snaps[best].viewproj, l, pat[1][l]);
        }
        for (u32 oi = 0; oi < rec_frame_.size(); oi++) {
            const OpRec& o = rec_frame_[oi];
            if (o.is_clear || op_hit[oi] || !o.draw.fb.depth) continue;
            unmatched_3d++;
            bool any = false;
            for (u32 st = 0; st < 5 && !any; st++) {
                if (!o.draw.has_stage[st]) continue;
                for (const CbufRec& cr : o.draw.stages[st].cbufs) {
                    const float* w = (const float*)cr.data.data();
                    size_t n = cr.data.size() / 4;
                    for (size_t i = 0; i + 12 <= n && !any; i++)
                        for (int m = 0; m < 2 && !any; m++)
                            for (int l = 0; l < kCamLayouts && !any; l++) {
                                int len = l < 2 ? 16 : 12;
                                if (i + len > n) continue;
                                const float* q = (const float*)pat[m][l];
                                int k = 0;
                                for (; k < len; k++) {
                                    float tol = 1e-3f * std::max(1.0f, std::fabs(q[k]));
                                    if (!(std::fabs(w[i + k] - q[k]) <= tol)) break;
                                }
                                if (k == len && memcmp(w + i, q, len * 4) != 0) {
                                    any = true;
                                    near_runs[m][l]++;
                                }
                            }
                }
            }
            (any ? near_keys : plain_keys)[o.draw.match_key >> 32]++;
            if (any) near_draws++;
        }
    }
    // History reads: render targets sampled before this frame writes them. A replay reads their
    // current contents (synced from the real target, which already holds this frame's or a later
    // result), so whatever accumulates across frames (exposure, TAA, ...) is applied twice.
    std::unordered_map<Image*, u32> first_write;
    for (u32 oi = 0; oi < rec_frame_.size(); oi++) {
        const OpRec& o = rec_frame_[oi];
        const Framebuffer& fb = o.is_clear ? o.clear.fb : o.draw.fb;
        for (u32 c = 0; c < fb.num_colors; c++)
            if (fb.color[c]) first_write.emplace(fb.color[c], oi);
        if (fb.depth) first_write.emplace(fb.depth, oi);
    }
    std::unordered_set<Image*> hist_imgs;
    u32 hist_draws = 0;
    for (u32 oi = 0; oi < rec_frame_.size(); oi++) {
        const OpRec& o = rec_frame_[oi];
        if (o.is_clear) continue;
        bool any = false;
        for (u32 s = 0; s < 5; s++) {
            if (!o.draw.has_stage[s]) continue;
            for (const ImageRef& r : o.draw.stages[s].image_refs) {
                auto it = r.img ? first_write.find(r.img) : first_write.end();
                if (it != first_write.end() && it->second >= oi) {
                    hist_imgs.insert(r.img);
                    any = true;
                }
            }
        }
        if (any) hist_draws++;
    }
    static u64 frames, matched, other_frames, lag[3], sum_hits[kCamMats][kCamLayouts], sum_bytes, sum_bufs;
    static u64 sum_other[kCamMats], sum_older, sum_newer, sum_draws, sum_draws_cam, sum_inv_only, sum_hist_imgs, sum_hist_draws;
    static double scan_ms, scan_max, last_ms;
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    double now = t1.QuadPart * 1000.0 / f.QuadPart;
    frames++;
    if (best >= 0) {
        matched++;
        lag[std::min(best, 2)]++;
        for (int i = 0; i < ns; i++)
            if (i != best && per_snap[i]) {
                other_frames++;
                break;
            }
    }
    for (int m = 0; m < kCamMats; m++) {
        for (int l = 0; l < kCamLayouts; l++) sum_hits[m][l] += hits[m][l];
        sum_other[m] += other_hits[m];
    }
    sum_older += other_older;
    sum_newer += other_newer;
    sum_draws += draws;
    sum_draws_cam += draws_cam;
    sum_inv_only += draws_inv_only;
    sum_hist_imgs += hist_imgs.size();
    sum_hist_draws += hist_draws;
    sum_bytes += bytes;
    sum_bufs += nbufs;
    scan_ms += ms;
    scan_max = std::max(scan_max, ms);
    if (!last_ms) last_ms = now;
    if (now - last_ms >= 1000.0) {
        double n = (double)frames;
        hw_log("cam: match: %llu/%llu frames matched (tick lag 0/1/2+: %llu/%llu/%llu, %llu also hold another tick) | "
               "hits per frame: view 4x4 %.1f 4x4T %.1f 3x4 %.1f 3x4T %.1f, viewProj 4x4 %.1f 4x4T %.1f 3x4 %.1f 3x4T %.1f, "
               "invView 4x4 %.1f 4x4T %.1f 3x4 %.1f 3x4T %.1f%s | scan %.0f cbufs %.0f KiB per frame, %.2f ms avg %.2f max",
               (unsigned long long)matched, (unsigned long long)frames, (unsigned long long)lag[0], (unsigned long long)lag[1],
               (unsigned long long)lag[2], (unsigned long long)other_frames, sum_hits[0][0] / n, sum_hits[0][1] / n,
               sum_hits[0][2] / n, sum_hits[0][3] / n, sum_hits[1][0] / n, sum_hits[1][1] / n, sum_hits[1][2] / n,
               sum_hits[1][3] / n, sum_hits[2][0] / n, sum_hits[2][1] / n, sum_hits[2][2] / n, sum_hits[2][3] / n,
               cam_inv_enabled() ? " (replaced)" : " (not replaced)", sum_bufs / n, sum_bytes / n / 1024.0, scan_ms / n, scan_max);
        hw_log("cam: frame: %.0f draws per frame, %.0f with the tick's view/viewProj, %.0f with only its inverse view | "
               "other ticks' matrices per frame: view %.1f viewProj %.1f invView %.1f (%.1f older, %.1f newer) | "
               "history reads per frame: %.1f render targets sampled before written, by %.1f draws",
               sum_draws / n, sum_draws_cam / n, sum_inv_only / n, sum_other[0] / n, sum_other[1] / n, sum_other[2] / n,
               sum_older / n, sum_newer / n, sum_hist_imgs / n, sum_hist_draws / n);
        {
            auto top = [](std::unordered_map<u64, u32>& m) {
                std::vector<std::pair<u32, u64>> v;
                for (auto& [k, c] : m) v.push_back({c, k});
                std::sort(v.rbegin(), v.rend());
                std::string t;
                for (size_t i = 0; i < v.size() && i < 6; i++) {
                    char b[48];
                    snprintf(b, sizeof(b), " %08llx x%u", (unsigned long long)v[i].second, v[i].first);
                    t += b;
                }
                m.clear();
                return t;
            };
            std::string nk = top(near_keys), pk = top(plain_keys);
            hw_log("cam: near: %.1f unmatched 3D draws per frame, %.1f hold a near-copy of the camera (view 4x4 %llu 4x4T %llu 3x4 %llu 3x4T %llu, "
                   "viewProj 4x4 %llu 4x4T %llu 3x4 %llu 3x4T %llu runs) | near-copy signatures:%s | other unmatched:%s",
                   unmatched_3d / n, near_draws / n, (unsigned long long)near_runs[0][0], (unsigned long long)near_runs[0][1],
                   (unsigned long long)near_runs[0][2], (unsigned long long)near_runs[0][3], (unsigned long long)near_runs[1][0],
                   (unsigned long long)near_runs[1][1], (unsigned long long)near_runs[1][2], (unsigned long long)near_runs[1][3], nk.c_str(),
                   pk.c_str());
            near_draws = unmatched_3d = 0;
            memset(near_runs, 0, sizeof(near_runs));
        }
        frames = matched = other_frames = sum_bytes = sum_bufs = 0;
        sum_older = sum_newer = sum_draws = sum_draws_cam = sum_inv_only = sum_hist_imgs = sum_hist_draws = 0;
        memset(lag, 0, sizeof(lag));
        memset(sum_hits, 0, sizeof(sum_hits));
        memset(sum_other, 0, sizeof(sum_other));
        scan_ms = scan_max = 0;
        last_ms = now;
    }
}

// The interpolated camera for one replay, in every layout. False: show the tick unblended.
// HWDER_CAM_REPLAY_ONLY=1 (test): replays use the tick's own camera, so each replay should look
// exactly like the real frame; any flashing left comes from the replay itself, not the blend.
bool VkRenderer::cam_prepare(float alpha) {
    if (!cam_frame_.matched || !cam_frame_.have_prev) return false;
    static const bool replay_only = getenv("HWDER_CAM_REPLAY_ONLY") && atoi(getenv("HWDER_CAM_REPLAY_ONLY")) != 0;
    float mats[kCamMats][16];
    if (replay_only) {
        memcpy(mats[kCamView], cam_frame_.cur.view, 64);
        memcpy(mats[kCamViewProj], cam_frame_.cur.viewproj, 64);
    } else if (!interp::lerp_camera(cam_frame_.prev, cam_frame_.cur, alpha, mats[kCamView], mats[kCamViewProj])) {
        return false;
    }
    interp::view_inverse(mats[kCamView], mats[kCamInv]);
    for (int m = 0; m < kCamMats; m++)
        for (int l = 0; l < kCamLayouts; l++) cam_layout(mats[m], l, cam_words_[m][l]);
    return true;
}

bool VkRenderer::ensure_saved_image(u32 w, u32 h, VkFormat fmt) {
    if (saved_img_ && saved_w_ == w && saved_h_ == h && saved_fmt_ == fmt) return true;
    if (saved_img_) {
        VkImage im = saved_img_;
        VkDeviceMemory mem = saved_mem_;
        g_rec.defer([im, mem] {
            vkDestroyImage(dev(), im, nullptr);
            vkFreeMemory(dev(), mem, nullptr);
        });
        saved_img_ = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(dev(), &ci, nullptr, &saved_img_) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev(), saved_img_, &req);
    const auto& mp = display::device().mem;
    u32 type = 0;
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev(), &ai, nullptr, &saved_mem_) != VK_SUCCESS) return false;
    vkBindImageMemory(dev(), saved_img_, saved_mem_, 0);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = saved_img_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(g_rec.cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    saved_w_ = w;
    saved_h_ = h;
    saved_fmt_ = fmt;
    return true;
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

// Filmstrip index tag of the next capture: what the output is (real frame or replay, and its alpha).
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
        pres_fmt_ = img->info.fmt.vk;
        g_rec.flush();
        g_s.present_count++;
        {
            static u64 draws_at_last = 0;
            hw_last_frame_draws = (u32)(draws_ - draws_at_last);
            draws_at_last = draws_;
        }
        {   // HWDER_RENDERDOC_AT=<present>[:<count>]: when running under RenderDoc (renderdoccmd capture),
            // capture <count> consecutive frames starting at that present.
            static const char* rd = getenv("HWDER_RENDERDOC_AT");
            if (rd && g_s.present_count == strtoull(rd, nullptr, 10)) {
                u32 n = strchr(rd, ':') ? (u32)atoi(strchr(rd, ':') + 1) : 1;
                RENDERDOC_API_1_1_2* api = nullptr;
                HMODULE m = GetModuleHandleA("renderdoc.dll");
                auto get = m ? (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI") : nullptr;
                if (get && get(eRENDERDOC_API_Version_1_1_2, (void**)&api) && api) {
                    api->TriggerMultiFrameCapture(n);
                    hw_log("renderdoc: capturing %u frames from present %llu", n, (unsigned long long)g_s.present_count);
                } else {
                    hw_log("renderdoc: HWDER_RENDERDOC_AT set but RenderDoc is not loaded");
                }
            }
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
    int out_hz = settings::kInterpEnabled ? settings::video().interp_hz : 0;
    static const char* env_interp = getenv("HWDER_INTERP");  // Hz, or -1 for the monitor's rate
    if (env_interp) out_hz = atoi(env_interp);
    if (out_hz < 0) out_hz = (int)display::monitor_hz();
    // Game interpolation already queues its own extra frames and owns their pacing.
    if (fb.release_hz || interp::game_interp()) {
        static bool reported = false;
        if (out_hz && !reported) {
            hw_log("vk: legacy replay override ignored while game-drawn interpolation is enabled");
            reported = true;
        }
        out_hz = 0;
    }
    const bool cam_mode = interp::camera_mode();
    std::unique_lock<std::recursive_mutex> interp_lock(m_);
    Image* final_img = nullptr;
    if (out_hz > 0 && image) {
        // Split the recording: everything up to the last write into the presented buffer is this
        // frame; later ops already belong to the next one (the renderer thread runs ahead).
        final_img = tc_.find_newest(fb.address);
        size_t cut = 0;
        for (size_t i = 0; i < rec_cur_.size(); i++) {
            const Framebuffer& f = rec_cur_[i].is_clear ? rec_cur_[i].clear.fb : rec_cur_[i].draw.fb;
            if (final_img && f.num_colors && f.color[0] == final_img) cut = i + 1;
        }
        rec_frame_.assign(std::make_move_iterator(rec_cur_.begin()), std::make_move_iterator(rec_cur_.begin() + cut));
        rec_cur_.erase(rec_cur_.begin(), rec_cur_.begin() + cut);
        g_s.pin_serial = rec_frame_.empty() ? 0 : rec_frame_.front().serial;
        if (tc_.destroy_count != shadow_destroy_count_) {  // some real target may be gone: rebuild the twins
            for (auto& [img, sh] : shadows_) tc_.destroy(sh, false);
            shadows_.clear();
            shadow_destroy_count_ = tc_.destroy_count;
        }
    }
    if (out_hz > 0 && image) {
        // ---- frame interpolation: k outputs per game frame at the display rate
        display::set_output_hz(out_hz);
        u32 iv = std::max(1u, fb.swap_interval);
        interp_acc_ += out_hz * iv / 60.0;
        int k = (int)interp_acc_;
        interp_acc_ -= k;
        if (k < 1) k = 1;
        // Camera mode reads nothing from the previous frame: rec_prev_ and its safety don't matter.
        bool can = recording_ && (cam_mode || (!rec_prev_.empty() && !prev_unsafe_)) && !rec_frame_.empty() &&
                   tc_.destroy_count == rec_destroy_count_ && g_s.pin_serial != 0 && !g_s.replay_unsafe && final_img &&
                   shadows_.count(final_img) == 0 + shadows_.count(final_img);
        {
            static u32 skipped_frames = 0, total_frames = 0, why[8] = {};
            total_frames++;
            if (!can) {
                skipped_frames++;
                if (!recording_) why[0]++;
                else if (!cam_mode && rec_prev_.empty()) why[1]++;
                else if (rec_frame_.empty()) why[2]++;
                else if (tc_.destroy_count != rec_destroy_count_) why[3]++;
                else if (g_s.pin_serial == 0) why[4]++;
                else if (g_s.replay_unsafe) why[5]++;
                else if (!cam_mode && prev_unsafe_) why[6]++;
                else why[7]++;
            }
            if (total_frames % 600 == 0) {
                hw_log("interp: %u of the last 600 game frames could not be interpolated (not-recording %u no-prev %u no-cur %u destroys %u unpinned %u unsafe %u prev-unsafe %u saved-img %u)",
                       skipped_frames, why[0], why[1], why[2], why[3], why[4], why[5], why[6], why[7]);
                skipped_frames = 0;
                memset(why, 0, sizeof(why));
            }
        }
        interp_lock.unlock();  // the replays render into shadow targets: the renderer thread may continue
        // rec_frame_ belongs to this thread until the swap below: scan it unlocked.
        if (cam_mode && !rec_frame_.empty()) cam_identify();
        for (int j = 1; j < k; j++) {
            VkImage out = image;
            bool replay = can;
            snprintf(g_strip_tag, sizeof(g_strip_tag), "replay %d/%d t=%.3f", j, k, (float)j / k);
            if (can && cam_mode) {
                // Render one tick behind: this frame's tick b is shown unblended at the last of its k
                // output slots, so output j is (k - j) slots earlier on the game clock, measured
                // against the host-time spacing of ticks a and b (not the fixed j/k).
                const interp::CamSnap& a = cam_frame_.prev;
                const interp::CamSnap& b = cam_frame_.cur;
                double dt = b.t_ms - a.t_ms;
                float alpha = dt > 0 ? (float)std::clamp(1.0 - (k - j) * (1000.0 / out_hz) / dt, 0.0, 1.0) : 1.0f;
                replay = cam_prepare(alpha);
                snprintf(g_strip_tag, sizeof(g_strip_tag), "replay %d/%d a=%.3f tick=%llu", j, k, alpha, (unsigned long long)b.tick);
                // HWDER_CAM_LOG: replay outcomes once per second
                if (interp::cam_log_every() > 0) {
                    static u64 blended, nomatch, noprev, gap, cut, last;
                    static double asum;
                    if (replay) blended++, asum += alpha;
                    else if (!cam_frame_.matched) nomatch++;
                    else if (!cam_frame_.have_prev) noprev++;
                    else if (dt > 250.0 || b.tick != a.tick + 1) gap++;
                    else cut++;  // a cut, a projection change or alpha 1
                    u64 now = GetTickCount64();
                    if (!last) last = now;
                    if (now - last >= 1000) {
                        hw_log("cam: replays: %llu blended (mean alpha %.2f), unblended: %llu no match, %llu no previous tick, "
                               "%llu tick gap, %llu cut/projection change/alpha 1",
                               (unsigned long long)blended, blended ? asum / blended : 0.0, (unsigned long long)nomatch,
                               (unsigned long long)noprev, (unsigned long long)gap, (unsigned long long)cut);
                        blended = nomatch = noprev = gap = cut = 0;
                        asum = 0;
                        last = now;
                    }
                }
            }
            if (replay) {
                std::lock_guard<std::recursive_mutex> l(m_);  // the recorder itself is shared
                cam_replay_ = cam_mode;
                replay_frame((float)j / k);
                cam_replay_ = false;
                out = shadow_of(final_img)->image;
                static const char* strip_dir2 = getenv("HWDER_FILMSTRIP");
                if (strip_dir2) filmstrip_capture(out, w, h, strip_dir2);
                g_rec.flush();
            } else if (getenv("HWDER_FILMSTRIP")) {  // the real frame shown early: capture it too, tagged
                std::lock_guard<std::recursive_mutex> l(m_);
                snprintf(g_strip_tag, sizeof(g_strip_tag), "dup %d/%d", j, k);
                filmstrip_capture(out, w, h, getenv("HWDER_FILMSTRIP"));
                g_rec.flush();
            }
            display::present_image(out, w, h, 1, g_s.timeline, g_rec.serial() - 1);
        }
        snprintf(g_strip_tag, sizeof(g_strip_tag), "real");
        display::present_image(image, w, h, 1, g_s.timeline, g_rec.serial() - 1);
        recording_ = true;
    } else {
        interp_lock.unlock();
        display::set_output_hz(0);
        display::present_image(image, w, h, fb.swap_interval, g_s.timeline, g_rec.serial() - 1, fb.release_hz);
        recording_ = false;
        rec_cur_.clear();
        rec_prev_.clear();
    }
    // the presented frame becomes the previous one for the next interpolation window
    if (!interp_lock.owns_lock()) interp_lock.lock();
    rec_prev_.swap(rec_frame_);
    rec_frame_.clear();
    if (!recording_) {
        rec_prev_.clear();
        rec_cur_.clear();
    }
    rec_destroy_count_ = tc_.destroy_count;
    prev_unsafe_ = g_s.replay_unsafe;
    g_s.replay_unsafe = false;
    if (!recording_) g_s.pin_serial = 0;
}

}  // namespace
}  // namespace gpu

namespace gpu::vk {
// texture cache destruction goes through the graveyard (presenter may still read the image)
void retire_resource(std::function<void()> fn) { retire(std::move(fn)); }
}  // namespace gpu::vk

namespace gpu {
Renderer* create_renderer(MemoryManager* mm) { return new VkRenderer(mm); }

static void start_private_pipeline_prewarm() {
    if (!prewarm_enabled()) {
        hw_log("vk: prewarm disabled; live recipe recording remains enabled");return;
    }
    const u64 start=GetTickCount64();warmup::Cache cache;
    const auto status=warmup::load(kWarmupFile,warmup_compatibility(),cache);
    const u64 loaded=GetTickCount64();
    if(status!=warmup::Status::Ok || cache.pipelines.empty()) {
        hw_log("vk: prewarm early load: %s, %zu recipes, %llu ms; renderer remains lazy",warmup::name(status),cache.pipelines.size(),(unsigned long long)(loaded-start));return;
    }
    auto* target=static_cast<VkRenderer*>(renderer());
    hw_log("vk: prewarm early renderer allocation: %llu ms",(unsigned long long)(GetTickCount64()-loaded));
    target->start_prewarm(std::move(cache),loaded-start);
}
void start_pipeline_prewarm() {
    // Catalog preparation must use the game selected on the startup screen.
    // Defer only if a catalog exists; missing catalogs retain existing behavior.
    if(catalog_enabled() && prewarm_enabled() && GetFileAttributesA(kCatalogFile)!=INVALID_FILE_ATTRIBUTES) {
        catalog_deferred=true;hw_log("catalog: preparation deferred until game selection");return;
    }
    start_private_pipeline_prewarm();
}
void prepare_pipeline_catalog() {
    const char* record=getenv("HWDER_PIPELINE_CATALOG_RECORD");
    catalog_recording=record && *record && strlen(record)<=128 && !catalog_debug_translation();
    if(!catalog_recording && !catalog_deferred)return;
    prelaunch::preparation_frame("Finding graphics programs in your local game...");
    catalog_sources.index();
    if(catalog_deferred) {
        catalog::Catalog cat;
        auto status=catalog::load(kCatalogFile,catalog_build(),catalog_game_version(),cat);
        if(status==catalog::Status::Ok && hwfs::nsp::active() && hwfs::nsp::info().title_id!=catalog::kTitleId)status=catalog::Status::Incompatible;
        if(status==catalog::Status::Ok && !catalog_debug_translation()) {
            hw_log("catalog: imported metadata scope=%s, %zu programs / %zu recipes; resolving fresh selected-game bytes",cat.scope.c_str(),cat.programs.size(),cat.pipelines.size());
            auto cache=catalog::prepare(cat,[](const catalog::Locator& l,std::vector<u8>& bytes){return catalog_sources.read(l,bytes);},[](const catalog::Progress& p){
                prelaunch::preparation_frame("Preparing graphics programs from your game...",p.done,p.total);
                if(p.done==p.total)hw_log("catalog: local translation: %u resolved, %u unresolved of %u",p.resolved,p.unresolved,p.total);
            });
            if(!cache.pipelines.empty()) {
                // A catalog is partial. Preserve compatible private successes,
                // including programs whose catalog source could not be resolved.
                warmup::Cache retained;
                const auto private_status = warmup::load(kWarmupFile, warmup_compatibility(), retained);
                if (private_status == warmup::Status::Ok) {
                    const size_t historical = retained.pipelines.size();
                    const auto merged = warmup::merge(retained, cache);
                    hw_log("catalog: private history %zu recipes, union %s (%zu recipes); keeping private records on failure",
                           historical, warmup::name(merged), retained.pipelines.size());
                    cache = std::move(retained);
                } else {
                    hw_log("catalog: private history %s; preparing resolved catalog recipes", warmup::name(private_status));
                }
                auto* target=static_cast<VkRenderer*>(renderer());target->start_prewarm(std::move(cache),0);target->wait_catalog();
                prelaunch::preparation_frame(nullptr);return;
            }
            hw_log("catalog: no resolvable recipes; normal private-cache/lazy fallback");
        } else hw_log("catalog: import %s (or debug translation active); normal private-cache/lazy fallback",warmup::name(status));
        start_private_pipeline_prewarm();
    }
    prelaunch::preparation_frame(nullptr);
}
}  // namespace gpu
