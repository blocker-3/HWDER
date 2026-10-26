// Shared declarations of the Vulkan renderer (runtime/src/gpu/vk/*).
#pragma once
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "gpu/diag.h"
#include "gpu/display.h"
#include "gpu/maxwell_3d.h"
#include "gpu/renderer.h"

namespace gpu::vk {

#define VK_LOG_ONCE(...)              \
    do {                              \
        static bool once_ = false;    \
        if (!once_) {                 \
            once_ = true;             \
            hw_log(__VA_ARGS__);      \
        }                             \
    } while (0)

#define VK_CHECK(x)                                                                   \
    do {                                                                              \
        VkResult r_ = (x);                                                            \
        if (r_ != VK_SUCCESS) {                                                       \
            hw_log("vk: error %d at %s:%d: %s", (int)r_, __FILE__, __LINE__, #x);     \
            hw_fatal("vulkan");                                                       \
        }                                                                             \
    } while (0)

u64 readable_size(u64 addr, u64 size);  // committed host bytes starting at addr
inline VkDevice dev() { return display::device().dev; }
u32 find_memory_type(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0);

inline u64 hash_bytes(const void* data, u64 size, u64 seed = 0x9E3779B97F4A7C15ull) {
    const u8* p = (const u8*)data;
    u64 h = seed ^ (size * 0xff51afd7ed558ccdull);
    u64 a = 0x2545F4914F6CDD1Dull, b = 0x9E3779B97F4A7C15ull, c = 0xC2B2AE3D27D4EB4Full, d = 0x165667B19E3779F9ull;
    u64 i = 0;
    for (; i + 32 <= size; i += 32) {
        u64 w[4];
        memcpy(w, p + i, 32);
        a = (a ^ w[0]) * 0x9E3779B97F4A7C15ull; a ^= a >> 29;
        b = (b ^ w[1]) * 0xC2B2AE3D27D4EB4Full; b ^= b >> 31;
        c = (c ^ w[2]) * 0x165667B19E3779F9ull; c ^= c >> 27;
        d = (d ^ w[3]) * 0xff51afd7ed558ccdull; d ^= d >> 33;
    }
    h ^= a + (b << 1) + (c << 2) + (d << 3);
    for (; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

// ------------------------------------------------------------------ formats
struct FormatInfo {
    VkFormat vk = VK_FORMAT_UNDEFINED;  // host format of the VkImage
    u8 bpb = 4;                         // guest bytes per block
    u8 bw = 1, bh = 1;                  // guest block dimensions
    bool depth = false, stencil = false;
    bool astc = false;                  // guest ASTC, host RGBA8 (CPU decoded)
    bool compressed = false;            // host format is block compressed
    bool integer = false;
    VkComponentMapping swizzle{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    bool valid() const { return vk != VK_FORMAT_UNDEFINED; }
};
FormatInfo tic_format(const u32* tic);  // TIC words 0..7
FormatInfo rt_format(u32 rt_format);    // Maxwell RenderTargetFormat
FormatInfo zeta_format(u32 depth_format);
FormatInfo android_format(u32 pixel_format);
VkFormat vertex_format(u32 attrib, u32* size_bytes);
u32 vk_format_bytes(VkFormat f);  // host bytes per texel (uncompressed) or per block
bool vk_format_compressed(VkFormat f);
VkFormatFeatureFlags format_features(VkFormat f);  // optimal tiling, cached
void decode_astc(const u8* src, u32 width, u32 height, u32 depth, u32 bw, u32 bh, u8* dst_rgba8);

// ------------------------------------------------------------------ host buffers
struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    u8* map = nullptr;
    u64 size = 0;
};
// cached: host-cached memory for buffers the CPU READS (readbacks). Uncached/write-combined host memory
// reads at a few tens of MB/s, which made a 720p readback cost ~55 ms.
Buffer create_buffer(u64 size, VkBufferUsageFlags usage, bool host_visible, bool prefer_device_local = false, bool cached = false);
void destroy_buffer(Buffer& b);

// A slice of CPU-writable memory valid for the commands being recorded.
struct Slice {
    VkBuffer buf = VK_NULL_HANDLE;
    u64 offset = 0;
    u8* ptr = nullptr;
};

// The draw-path profile and the slow-scope log live in gpu/diag.h; the same names are available here.
using diag::ProfPhase;
using enum diag::ProfPhase;
using diag::kProfNames;
using diag::g_prof;
using diag::g_prof_draws;
using diag::g_prof_cbuf_bytes;
using diag::g_prof_cbufs;
using diag::g_prof_sets;
using diag::g_prof_sets_reused;
using diag::g_prof_vtx_bytes;
using diag::g_prof_vtx_copies;
using diag::g_prof_idx_bytes;
using diag::g_prof_idx_scanned;
using diag::ProfScope;
using diag::SlowTimer;
using diag::prof_flush;
extern float g_res_scale, g_res_scale_y;  // internal resolution scale (x, y) for large render targets

// ------------------------------------------------------------------ GPU timing (HWDER_GPU_TIMING, vk_sched.cpp)
namespace gputime {
enum Kind : u32 { kOther, kPass };
struct Seg {
    u32 kind = kOther, w = 0, h = 0, fmt = 0, dfmt = 0, ncol = 0;
    u64 prog = 0;  // fragment shader of the first draw
    u32 draws = 0, clears = 0;
};
bool on();
// Starts a new segment in the current command buffer (a timestamp after the work recorded so far).
void mark(u32 kind, u32 w = 0, u32 h = 0, u32 fmt = 0, u32 dfmt = 0, u32 ncol = 0);
void count_draw(u64 prog);
void count_clear();
}  // namespace gputime

// ------------------------------------------------------------------ command recording
// Vulkan commands are recorded on a worker thread ("HWDER vk record"): the GPU thread queues each command (or a
// draw's whole state) as a closure called with the command buffer, in order, and the worker records, ends and
// submits the command buffers. Everything a closure uses is captured by value. HWDER_VK_SYNC_SUBMIT=1 records
// and submits on the GPU thread instead (as before).
struct CmdChunk;
class Recorder {
public:
    void cmd();          // a command buffer outside any rendering instance (ends one)
    void cmd_in_pass();  // a command buffer, whatever the rendering state
    // Queue f(VkCommandBuffer) for the current command buffer.
    template <class F>
    void record(F&& f) {
        if (sync_record()) {
            f(sync_cb());
            return;
        }
        using Fn = std::decay_t<F>;
        static_assert(alignof(Fn) <= 16, "command closure alignment");
        void* body = alloc_cmd(sizeof(Fn), [](void* p, VkCommandBuffer cb) {
            Fn* fn = static_cast<Fn*>(p);
            (*fn)(cb);
            fn->~Fn();
        });
        new (body) Fn(std::forward<F>(f));
    }
    static bool sync_record();  // HWDER_VK_SYNC_SUBMIT=1
    void end_rendering();
    void barrier();                 // full memory barrier (also ends rendering)
    Slice stage(u64 size, u64 align = 256);
    u64 serial() const { return serial_; }
    u64 completed() const;
    void defer(std::function<void()> fn);  // run after the current serial completes (on the waiter thread)
    void flush();
    void wait_idle();
    bool rendering = false;

    u64 serial_ = 1;
    CmdChunk* chunk_ = nullptr;  // chunk being filled for the current command buffer

private:
    using Exec = void (*)(void* body, VkCommandBuffer cb);
    void* alloc_cmd(size_t size, Exec exec);  // storage for a closure of `size` bytes in the current chunk
    VkCommandBuffer sync_cb();                // synchronous recording: the current command buffer
};
Recorder& rec();
// Destroy after the GPU finished the current work and a few frames were presented.
void retire_resource(std::function<void()> fn);

// ------------------------------------------------------------------ textures
enum class ImageType : u8 { e1D, e2D, e3D };

struct ImageInfo {
    ImageType type = ImageType::e2D;
    FormatInfo fmt;
    u32 width = 1, height = 1, depth = 1;  // depth: 3D only
    u32 layers = 1, levels = 1;
    bool block_linear = true;
    u32 bh = 0, bd = 0, tws = 0;  // block height/depth log2, tile width spacing
    u32 pitch = 0;                // pitch linear only
    u32 layer_stride = 0;         // guest bytes between layers (0 = computed)
    bool cube = false;
    bool render_target = false;
};

struct ViewKey {
    VkFormat format;
    VkImageViewType type;
    u32 base_level, levels, base_layer, layers;
    u32 swizzle;  // packed VkComponentSwizzle x4
    u32 aspect;
    bool operator==(const ViewKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
};
struct ViewKeyHash {
    size_t operator()(const ViewKey& k) const { return (size_t)hash_bytes(&k, sizeof(k)); }
};

struct Image {
    ImageInfo info;
    u64 addr = 0, guest_size = 0;
    u64 readable = 0;  // committed guest bytes from addr (<= guest_size)
    u64 level_offset[16] = {};
    u64 layer_stride = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;  // as created (image views restrict it per format)
    std::unordered_map<ViewKey, VkImageView, ViewKeyHash> views;
    bool gpu_modified = false;   // GPU data newer than guest memory
    u64 write_seq = 0;           // order of the last GPU write (aliasing images are refreshed from the newest)
    u64 presented_seq = ~0ull;   // write_seq when this image was last presented (diagnostic: late writes)
    bool cpu_dirty = false;      // guest memory written (invalidate) since the last upload
    u64 hash = 0;
    u64 checked_epoch = 0;
    u64 watch_epoch = 0;  // epoch of the last write-watch check (writes reported since then are unseen)
    u64 last_used = 0;
    // Internal resolution scale: the VkImage of a scaled render target is scale x the guest size
    // (info keeps the guest dimensions for cache matching and layout). level_* are PHYSICAL sizes.
    float scale = 1.0f, scale_y = 1.0f;  // x / y factors (differ for "unlocked" ultrawide rendering)
    u64 full_width_frame = ~0ull;  // fill ultrawide: last frame a full-width pass or clear wrote this target
    bool full_width_since_present = false;  // full-width content written since this image's last presentation
    u32 phys_w() const { return scale == 1.0f ? info.width : std::max(1u, (u32)(info.width * scale + 0.5f)); }
    u32 phys_h() const { return scale_y == 1.0f ? info.height : std::max(1u, (u32)(info.height * scale_y + 0.5f)); }
    u32 level_width(u32 l) const { return std::max(1u, phys_w() >> l); }
    u32 level_height(u32 l) const { return std::max(1u, phys_h() >> l); }
    u32 level_depth(u32 l) const { return std::max(1u, info.depth >> l); }
};

struct Subresource {
    u32 level = 0, layer = 0;
};

class TextureCache {
public:
    explicit TextureCache(MemoryManager* mm) : mm_(mm) {}
    // Find or create an image for `info` at guest CPU address `addr`. sub receives the matching
    // mip level / layer when the request is a subresource of a larger image.
    Image* get(u64 addr, const ImageInfo& info, Subresource* sub);
    Image* find_exact(u64 addr);  // any image starting at addr (largest), no creation
    Image* find_newest(u64 addr);  // the image at addr with the most recent GPU write / CPU upload
    void validate_now(Image* img);  // like validate() but hashes regardless of interval and budget
    void apply_scale(float sx, float sy);  // change the internal resolution scale: scaled targets are recreated on next use
    static bool scalable(const ImageInfo& info);
    static bool scale_stale(const Image* img);  // physical size differs from what the current scale gives
    void destroy(Image* img, bool count = true);
    void alloc_image(Image* img);    // VkImage + memory + initial layout/clear for img->info/scale
    void validate(Image* img);              // re-upload if the CPU wrote it (cheap when nothing changed)
    bool has_same_addr_alias(Image* img);   // another image starts at the same address (padded RT vs sampled)
    VkImageView view(Image* img, VkFormat fmt, VkImageViewType type, u32 base_level, u32 levels, u32 base_layer,
                     u32 layers, VkComponentMapping swz = {}, VkImageAspectFlags aspect = 0);
    VkSampler sampler(const u32* tsc);
    void mark_modified(Image* img, bool clear = false) {
        // Diagnostic: a draw into a buffer that was presented and not cleared since means the present
        // happened before the frame's rendering was complete (fence reached too early).
        if (!clear && img->presented_seq == img->write_seq) late_writes_++;
        if (clear) img->presented_seq = ~0ull;
        img->gpu_modified = true;
        img->write_seq = ++write_counter_;
    }
    u64 write_counter_ = 0;
    u64 late_writes_ = 0;  // draws into an already-presented, not yet cleared buffer (see mark_modified)
    void invalidate(u64 addr, u64 size);
    void flush(u64 addr, u64 size);
    bool is_gpu_modified(u64 addr, u64 size);
    void audit_cpu_write(u64 addr, u64 size);
    void next_epoch() { epoch_++; }
    u32 n_uploads = 0, n_downloads = 0, n_creates = 0;  // per-frame statistics (reset by the renderer)
    u64 generation = 0;  // bumped whenever an image is created or destroyed (framebuffer cache validity)
    u64 destroy_count = 0;  // images destroyed (a recorded frame referencing one must not be replayed)
    u64 tex_gen = 0;  // bumps when a cached TIC->image mapping may change (image destroyed / alias created)
    u64 upload_bytes = 0;
    u64 hash_budget_epoch_ = 0, hash_budget_ = 0;
    int written_since_check(Image* img);  // 1 written, 0 clean, -1 memory not write-watched
    // The same for any guest range; `watch_epoch` is the caller's record of its last check (0 at first).
    int written_since(u64 addr, u64 size, u64& watch_epoch);
    u64 epoch() const { return epoch_; }
    void download(Image* img);  // write GPU data back to guest memory (waits for the GPU)
    void collect_garbage();
    // helpers for image info from TIC
    bool info_from_tic(const u32* tic, ImageInfo& info, u64& addr, u32& base_level, u32& levels, u32& base_layer);

    static u64 compute_layout(const ImageInfo& info, u64* level_offset, u64* layer_stride);

private:
    void upload(Image* img);
    Image* create(u64 addr, const ImageInfo& info);
    void collect_overlaps(u64 addr, u64 size, std::vector<Image*>& out);
    bool match_subresource(Image* img, u64 addr, const ImageInfo& info, Subresource* sub);
    void link(Image* img);
    void unlink(Image* img);

    MemoryManager* mm_;
    std::unordered_map<u64, std::vector<Image*>> pages_;  // 64 KiB pages -> images
    std::unordered_map<u64, VkSampler> samplers_;
    // Write-watch reports, page address -> epoch reported. GetWriteWatch resets what it returns, so
    // reports are kept here for every image sharing the page; entries older than watch_pruned_ are dropped.
    std::map<u64, u64> written_pages_;
    std::vector<void*> watch_buf_;
    static constexpr u64 kWatchBlock = 2ull << 20, kNotWatched = ~0ull;
    std::unordered_map<u64, u64> watch_blocks_;  // 2 MiB block -> recorder serial of its last query (kNotWatched: query per image)
    std::unordered_map<u64, u64> watch_written_;  // 2 MiB block -> latest epoch any of its pages was reported written
    u64 watch_pruned_ = 0;
    u64 epoch_ = 1;
};

}  // namespace gpu::vk
