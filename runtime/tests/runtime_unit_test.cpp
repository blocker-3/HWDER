// Unit tests for the runtime's pure functions: the renderer's format and pipeline-key mappers, the guest
// texture layout arithmetic, HIPC request parsing / reply framing and the kernel handle table.
//
// These are REGRESSION PINS: where the Maxwell or Vulkan enum gives the answer the expectation comes from
// the enum; everywhere else the expected value was read off the current implementation (commit 9a1b790 of
// code-structure) and asserts that it does not change by accident. No Vulkan instance, device or game data
// is needed: the few symbols the linked units reach for (the display device, one Vulkan entry point, the
// logger, the kernel lock and current thread) are stubbed below.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "gpu/vk/texture_layout.h"
#include "gpu/vk/vk_renderer.h"
#include "kernel/kernel.h"
#include "service/hipc.h"

// ------------------------------------------------------------------ stubs for the linked units
extern "C" void hw_log(const char*, ...) {}
extern "C" void hw_fatal(const char* why) {
    fprintf(stderr, "hw_fatal: %s\n", why);
    exit(2);
}
namespace display {
Device& device() {
    static Device d;
    return d;
}
}  // namespace display
// formats.cpp asks the device whether a vertex format is usable; the test flips the answer.
static bool g_vertex_formats_supported = true;
static void VKAPI_PTR stub_format_properties(VkPhysicalDevice, VkFormat, VkFormatProperties* p) {
    *p = {};
    p->optimalTilingFeatures = ~0u;
    p->bufferFeatures = g_vertex_formats_supported ? VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT : 0;
}
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = stub_format_properties;
namespace kern {
KernelLock g_kernel_lock;
void KernelLock::lock() {}
void KernelLock::unlock() {}
Thread* current_thread() { return nullptr; }
}  // namespace kern

// ------------------------------------------------------------------ harness
static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        g_checks++;                                                                   \
        if (!(cond)) {                                                                \
            g_failures++;                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                             \
    } while (0)
#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        g_checks++;                                                                                     \
        auto va_ = (a);                                                                                 \
        auto vb_ = (b);                                                                                 \
        if (!(va_ == vb_)) {                                                                            \
            g_failures++;                                                                               \
            fprintf(stderr, "FAIL %s:%d: %s == %s: %lld vs %lld\n", __FILE__, __LINE__, #a, #b,        \
                    (long long)va_, (long long)vb_);                                                    \
        }                                                                                               \
    } while (0)

using namespace gpu::vk;

// ------------------------------------------------------------------ format mappers (formats.cpp)
static void test_formats() {
    // rt_format: Maxwell RenderTargetFormat -> host format (table pins)
    FormatInfo f = rt_format(0xD5);
    CHECK_EQ(f.vk, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(f.bpb, 4);
    CHECK(!f.depth && !f.stencil && !f.integer && !f.compressed && !f.astc && f.valid());
    CHECK_EQ(rt_format(0xCF).vk, VK_FORMAT_B8G8R8A8_UNORM);
    CHECK_EQ(rt_format(0xC0).vk, VK_FORMAT_R32G32B32A32_SFLOAT);
    CHECK_EQ(rt_format(0xC0).bpb, 16);
    CHECK_EQ(rt_format(0xE5).vk, VK_FORMAT_R32_SFLOAT);
    CHECK_EQ(rt_format(0xE8).bpb, 2);
    CHECK(rt_format(0xD9).integer);  // R8G8B8A8_UINT
    CHECK(!rt_format(0).valid());
    CHECK(!rt_format(0x12).valid());

    // zeta_format: depth attachments
    f = zeta_format(0x0A);
    CHECK_EQ(f.vk, VK_FORMAT_D32_SFLOAT);
    CHECK(f.depth && !f.stencil);
    f = zeta_format(0x14);
    CHECK_EQ(f.vk, VK_FORMAT_D24_UNORM_S8_UINT);
    CHECK(f.depth && f.stencil);
    CHECK_EQ(f.bpb, 4);
    CHECK_EQ(zeta_format(0x17).bpb, 1);  // S8 emulated as D24S8 with 1 guest byte per pixel
    CHECK_EQ(zeta_format(0x13).vk, VK_FORMAT_D16_UNORM);
    CHECK(!zeta_format(0).valid());

    // android_format: presented buffers
    CHECK_EQ(android_format(1).vk, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(android_format(4).vk, VK_FORMAT_R5G6B5_UNORM_PACK16);
    CHECK_EQ(android_format(4).bpb, 2);
    CHECK_EQ(android_format(5).vk, VK_FORMAT_B8G8R8A8_UNORM);
    CHECK_EQ(android_format(99).vk, VK_FORMAT_R8G8B8A8_UNORM);  // unknown: RGBA8

    // tic_format: TIC word 0 bits 0-6 = format code, 7-9 = component type; word 4 bit 22 = sRGB
    u32 tic[8] = {};
    tic[0] = 0x2F;  // Z32F
    f = tic_format(tic);
    CHECK_EQ(f.vk, VK_FORMAT_D32_SFLOAT);
    CHECK(f.depth);
    tic[0] = 0x3A;  // Z16
    CHECK_EQ(tic_format(tic).vk, VK_FORMAT_D16_UNORM);
    CHECK_EQ(tic_format(tic).bpb, 2);
    tic[0] = 0x29;  // S8Z24
    f = tic_format(tic);
    CHECK_EQ(f.vk, VK_FORMAT_D24_UNORM_S8_UINT);
    CHECK(f.depth && f.stencil);
    tic[0] = 0x40;  // ASTC 4x4: CPU-decoded to RGBA8, 16 guest bytes per 4x4 block
    f = tic_format(tic);
    CHECK(f.astc);
    CHECK_EQ(f.vk, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(f.bpb, 16);
    CHECK_EQ(f.bw, 4);
    CHECK_EQ(f.bh, 4);
    tic[4] = 1u << 22;  // sRGB
    CHECK_EQ(tic_format(tic).vk, VK_FORMAT_R8G8B8A8_SRGB);
    tic[4] = 0;
    tic[0] = 0x45;  // ASTC 10x10
    CHECK_EQ(tic_format(tic).bw, 10);
    tic[0] = 0x7F;  // not a format
    CHECK(!tic_format(tic).valid());

    // vk_format_bytes / vk_format_compressed (host texel or block sizes)
    CHECK_EQ(vk_format_bytes(VK_FORMAT_R8_UNORM), 1);
    CHECK_EQ(vk_format_bytes(VK_FORMAT_R8G8_UNORM), 2);
    CHECK_EQ(vk_format_bytes(VK_FORMAT_R8G8B8A8_UNORM), 4);
    CHECK_EQ(vk_format_bytes(VK_FORMAT_R16G16B16A16_SFLOAT), 8);
    CHECK_EQ(vk_format_bytes(VK_FORMAT_R32G32B32_SFLOAT), 12);
    CHECK_EQ(vk_format_bytes(VK_FORMAT_R32G32B32A32_SFLOAT), 16);
    CHECK_EQ(vk_format_bytes(VK_FORMAT_BC1_RGBA_UNORM_BLOCK), 8);
    CHECK_EQ(vk_format_bytes(VK_FORMAT_BC7_UNORM_BLOCK), 16);
    CHECK(vk_format_compressed(VK_FORMAT_BC1_RGBA_UNORM_BLOCK));
    CHECK(vk_format_compressed(VK_FORMAT_ASTC_12x12_SRGB_BLOCK));
    CHECK(!vk_format_compressed(VK_FORMAT_R8G8B8A8_UNORM));

    // vertex_format: attribute word bits 21-26 = size, 27-29 = type, 31 = BGRA
    auto attrib = [](u32 size, u32 type, bool bgra = false) { return (size << 21) | (type << 27) | (bgra ? 1u << 31 : 0); };
    u32 bytes = 0;
    CHECK_EQ(vertex_format(attrib(0x01, 7), &bytes), VK_FORMAT_R32G32B32A32_SFLOAT);
    CHECK_EQ(bytes, 16);
    CHECK_EQ(vertex_format(attrib(0x0A, 2), &bytes), VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(bytes, 4);
    CHECK_EQ(vertex_format(attrib(0x0A, 2, true), nullptr), VK_FORMAT_B8G8R8A8_UNORM);
    CHECK_EQ(vertex_format(attrib(0x12, 7), &bytes), VK_FORMAT_R32_SFLOAT);
    CHECK_EQ(bytes, 4);
    CHECK_EQ(vertex_format(attrib(0x1B, 1), nullptr), VK_FORMAT_R16_SNORM);
    CHECK_EQ(vertex_format(attrib(0x0A, 7), nullptr), VK_FORMAT_R8G8B8A8_UNORM);  // no RGBA8 float: falls back to unorm
    g_vertex_formats_supported = true;
    CHECK_EQ(vertex_format(attrib(0x13, 2), &bytes), VK_FORMAT_R8G8B8_UNORM);
    CHECK_EQ(bytes, 3);
    g_vertex_formats_supported = false;  // a device without 3-component vertex formats widens to 4
    CHECK_EQ(vertex_format(attrib(0x13, 2), &bytes), VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(bytes, 3);  // the guest stride stays 3 bytes
    g_vertex_formats_supported = true;
    CHECK_EQ(vertex_format(attrib(0x3F, 2), &bytes), VK_FORMAT_UNDEFINED);
    CHECK_EQ(bytes, 0);
}

// ------------------------------------------------------------------ pipeline-key mappers (vk_renderer.h)
static void test_pipeline_key_mappers() {
    using namespace gpu::vk::detail;
    // compare_op: Maxwell 1..8 and GL 0x200..0x207 both map onto VkCompareOp 0..7
    CHECK_EQ(compare_op(1), VK_COMPARE_OP_NEVER);
    CHECK_EQ(compare_op(8), VK_COMPARE_OP_ALWAYS);
    CHECK_EQ(compare_op(0x200), VK_COMPARE_OP_NEVER);
    CHECK_EQ(compare_op(0x204), VK_COMPARE_OP_GREATER);
    CHECK_EQ(compare_op(0x207), VK_COMPARE_OP_ALWAYS);
    CHECK_EQ(compare_op(0), VK_COMPARE_OP_ALWAYS);  // unknown: always
    CHECK_EQ(compare_op(0x300), VK_COMPARE_OP_ALWAYS);
    // stencil_op
    CHECK_EQ(stencil_op(1), VK_STENCIL_OP_KEEP);
    CHECK_EQ(stencil_op(0x1E00), VK_STENCIL_OP_KEEP);
    CHECK_EQ(stencil_op(0), VK_STENCIL_OP_ZERO);
    CHECK_EQ(stencil_op(0x1E01), VK_STENCIL_OP_REPLACE);
    CHECK_EQ(stencil_op(0x8507), VK_STENCIL_OP_INCREMENT_AND_WRAP);
    CHECK_EQ(stencil_op(99), VK_STENCIL_OP_KEEP);
    // blend_op
    CHECK_EQ(blend_op(1), VK_BLEND_OP_ADD);
    CHECK_EQ(blend_op(0x800A), VK_BLEND_OP_SUBTRACT);
    CHECK_EQ(blend_op(3), VK_BLEND_OP_REVERSE_SUBTRACT);
    CHECK_EQ(blend_op(0x8007), VK_BLEND_OP_MIN);
    CHECK_EQ(blend_op(5), VK_BLEND_OP_MAX);
    CHECK_EQ(blend_op(0), VK_BLEND_OP_ADD);
    // blend_factor: Maxwell 1..0x13 and GL 0x4000.. / 0xC001.. / 0xC900..
    CHECK_EQ(blend_factor(0x1), VK_BLEND_FACTOR_ZERO);
    CHECK_EQ(blend_factor(0x4001), VK_BLEND_FACTOR_ONE);
    CHECK_EQ(blend_factor(0x5), VK_BLEND_FACTOR_SRC_ALPHA);
    CHECK_EQ(blend_factor(0x4302), VK_BLEND_FACTOR_SRC_ALPHA);
    CHECK_EQ(blend_factor(0x6), VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    CHECK_EQ(blend_factor(0xC), VK_BLEND_FACTOR_SRC_ALPHA);  // SRC_ALPHA_SATURATE's neighbours: 0xC/0xD alias src alpha
    CHECK_EQ(blend_factor(0xD), VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    CHECK_EQ(blend_factor(0xC003), VK_BLEND_FACTOR_CONSTANT_ALPHA);
    CHECK_EQ(blend_factor(0x13), VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA);
    CHECK_EQ(blend_factor(0xC903), VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA);
    CHECK_EQ(blend_factor(0), VK_BLEND_FACTOR_ONE);  // unknown: one
    // topology: quads (7, 8) are converted to triangle lists by do_draw, so they map to TRIANGLE_LIST
    CHECK_EQ(topology(0), VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    CHECK_EQ(topology(1), VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    CHECK_EQ(topology(2), VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
    CHECK_EQ(topology(3), VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
    CHECK_EQ(topology(4), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    CHECK_EQ(topology(5), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    CHECK_EQ(topology(6), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
    CHECK_EQ(topology(7), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    CHECK_EQ(topology(8), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    CHECK_EQ(topology(9), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
    CHECK_EQ(topology(0xE), VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
    CHECK_EQ(topology(0x1F), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    // logic_op: GL 0x1500..0x150F -> VkLogicOp 0..15
    CHECK_EQ(logic_op(0x1500), VK_LOGIC_OP_CLEAR);
    CHECK_EQ(logic_op(0x1503), VK_LOGIC_OP_COPY);
    CHECK_EQ(logic_op(0x150F), VK_LOGIC_OP_SET);
    CHECK_EQ(logic_op(0), VK_LOGIC_OP_COPY);
    CHECK_EQ(logic_op(0x1510), VK_LOGIC_OP_COPY);
    // tic_swizzle: TIC component sources 0 zero, 2..5 R G B A, else one
    CHECK_EQ(tic_swizzle(0), VK_COMPONENT_SWIZZLE_ZERO);
    CHECK_EQ(tic_swizzle(2), VK_COMPONENT_SWIZZLE_R);
    CHECK_EQ(tic_swizzle(3), VK_COMPONENT_SWIZZLE_G);
    CHECK_EQ(tic_swizzle(4), VK_COMPONENT_SWIZZLE_B);
    CHECK_EQ(tic_swizzle(5), VK_COMPONENT_SWIZZLE_A);
    CHECK_EQ(tic_swizzle(1), VK_COMPONENT_SWIZZLE_ONE);
    CHECK_EQ(tic_swizzle(7), VK_COMPONENT_SWIZZLE_ONE);
    // view_type
    CHECK_EQ(view_type(shader::TextureType::Tex2D), VK_IMAGE_VIEW_TYPE_2D);
    CHECK_EQ(view_type(shader::TextureType::Cube), VK_IMAGE_VIEW_TYPE_CUBE);
    CHECK_EQ(view_type(shader::TextureType::Tex3D), VK_IMAGE_VIEW_TYPE_3D);
    CHECK_EQ(view_type(shader::TextureType::Buffer), VK_IMAGE_VIEW_TYPE_2D);  // not a sampled image type
    // format_numeric_type
    CHECK_EQ((int)format_numeric_type(VK_FORMAT_R8G8B8A8_UINT), (int)shader::AttributeType::UnsignedInt);
    CHECK_EQ((int)format_numeric_type(VK_FORMAT_R16G16_SINT), (int)shader::AttributeType::SignedInt);
    CHECK_EQ((int)format_numeric_type(VK_FORMAT_R8G8B8A8_UNORM), (int)shader::AttributeType::Float);
    // cached_vertex_format agrees with vertex_format and keys the cache on size/type/bgra only
    u32 b1 = 0, b2 = 0;
    VkFormat f1 = cached_vertex_format((0x0A << 21) | (2u << 27) | (5 << 7), &b1);  // offset bits ignored
    VkFormat f2 = cached_vertex_format((0x0A << 21) | (2u << 27), &b2);
    CHECK_EQ(f1, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(f2, f1);
    CHECK_EQ(b1, 4);
    CHECK_EQ(b2, 4);
}

// ------------------------------------------------------------------ texture layout (texture_layout.cpp)
static ImageInfo rgba8(u32 w, u32 h) {
    ImageInfo i;
    i.fmt = rt_format(0xD5);  // R8G8B8A8_UNORM, 4 bytes per texel
    i.width = w;
    i.height = h;
    return i;
}

static void test_texture_layout() {
    // mip_shift: the block height shift shrinks while a block would still cover the level (8-row GOBs)
    CHECK_EQ(mip_shift(4, 8, 256), 4);
    CHECK_EQ(mip_shift(4, 8, 64), 3);   // 8 << 3 = 64 >= 64: one step down
    CHECK_EQ(mip_shift(4, 8, 8), 0);
    CHECK_EQ(mip_shift(0, 8, 1), 0);
    CHECK_EQ(mip_shift(2, 1, 1), 0);    // 3D depth unit 1

    // pitch-linear 1920x1080 RGBA8: pitch defaults to the row bytes
    ImageInfo lin = rgba8(1920, 1080);
    lin.block_linear = false;
    LevelParams p = level_params(lin, 0);
    CHECK_EQ(p.width_bytes, 7680);
    CHECK_EQ(p.by, 1080);
    CHECK_EQ(p.size, 7680ull * 1080);
    CHECK_EQ(TextureCache::compute_layout(lin, nullptr, nullptr), 7680ull * 1080);
    lin.pitch = 8192;
    CHECK_EQ(level_params(lin, 0).size, 8192ull * 1080);

    // block-linear 256x256 RGBA8, block height 16 GOBs (bh = 4): exactly the texel bytes
    ImageInfo bl = rgba8(256, 256);
    bl.bh = 4;
    p = level_params(bl, 0);
    CHECK_EQ(p.gobs_w, 16);
    CHECK_EQ(p.bhs, 4);
    CHECK_EQ(p.size, 256ull * 256 * 4);
    // 100x100: 7 GOB columns, one 16-GOB block row -> 7 blocks of 8 KiB (padding to the block grid)
    ImageInfo odd = rgba8(100, 100);
    odd.bh = 4;
    p = level_params(odd, 0);
    CHECK_EQ(p.gobs_w, 7);
    CHECK_EQ(p.size, 7ull * 8192);
    CHECK_EQ(TextureCache::compute_layout(odd, nullptr, nullptr), 7ull * 8192);
    // mip chain 256x256, 3 levels: the block height shift drops to 3 at the 64x64 level (regression pins)
    ImageInfo mip = rgba8(256, 256);
    mip.bh = 4;
    mip.levels = 3;
    CHECK_EQ(level_params(mip, 0).size, 262144);
    CHECK_EQ(level_params(mip, 1).size, 65536);
    CHECK_EQ(level_params(mip, 2).bhs, 3);
    CHECK_EQ(level_params(mip, 2).size, 16384);
    u64 offs[16] = {}, stride = 0;
    CHECK_EQ(TextureCache::compute_layout(mip, offs, &stride), 262144 + 65536 + 16384);
    CHECK_EQ(offs[0], 0);
    CHECK_EQ(offs[1], 262144);
    CHECK_EQ(offs[2], 262144 + 65536);
    CHECK_EQ(stride, 262144 + 65536 + 16384);
    // two layers of 64x64, bh = 4: the layer stride aligns to the (shrunken) block size, here a no-op
    ImageInfo arr = rgba8(64, 64);
    arr.bh = 4;
    arr.layers = 2;
    CHECK_EQ(level_params(arr, 0).size, 32768);
    CHECK_EQ(TextureCache::compute_layout(arr, nullptr, &stride), 65536);
    CHECK_EQ(stride, 32768);
    // an explicit guest layer stride wins
    arr.layers = 3;
    arr.layer_stride = 100000;
    CHECK_EQ(TextureCache::compute_layout(arr, nullptr, &stride), 100000ull * 2 + 32768);
    CHECK_EQ(stride, 100000);
    // 3D 16x16x8 with block depth 2 (bd = 1) and block height 2 GOBs (bh = 1): exactly the texel bytes
    ImageInfo vol = rgba8(16, 16);
    vol.type = ImageType::e3D;
    vol.depth = 8;
    vol.bh = 1;
    vol.bd = 1;
    p = level_params(vol, 0);
    CHECK_EQ(p.d, 8);
    CHECK_EQ(p.bds, 1);
    CHECK_EQ(p.size, 16ull * 16 * 8 * 4);
    // compressed: BC-style 4x4 blocks of 8 bytes (a 64x64 image is 16x16 blocks = 2 KiB linear)
    ImageInfo bc = rgba8(64, 64);
    bc.fmt.bw = bc.fmt.bh = 4;
    bc.fmt.bpb = 8;
    bc.block_linear = false;
    p = level_params(bc, 0);
    CHECK_EQ(p.bx, 16);
    CHECK_EQ(p.by, 16);
    CHECK_EQ(p.size, 16ull * 16 * 8);
}

// ------------------------------------------------------------------ HIPC framing (hipc.cpp)
static void put32(u8* msg, size_t off, u32 v) { memcpy(msg + off, &v, 4); }
static u32 get32(const u8* msg, size_t off) {
    u32 v;
    memcpy(&v, msg + off, 4);
    return v;
}

static void test_hipc() {
    using namespace ipc;
    // Plain request: type 4, no descriptors; the CMIF body starts at the first 16-byte boundary after the
    // 8-byte header, holds SFCI, the command at +8 and the arguments at +16.
    {
        alignas(16) u8 msg[0x100] = {};
        put32(msg, 0, 4);                 // type 4 (Request), no X/A/B/W descriptors
        put32(msg, 4, 8);                 // raw words: 8 (2 of padding + 4 of header + 2 of arguments)
        put32(msg, 16, kSfci);
        put32(msg, 24, 0x2A);             // command
        put32(msg, 32, 0x11111111);
        put32(msg, 36, 0x22222222);
        Parsed p;
        parse(msg, p, false);
        CHECK_EQ(p.rq.type, 4);
        CHECK_EQ(p.rq.command, 0x2A);
        CHECK(!p.rq.send_pid);
        CHECK_EQ(p.rq.args_size, 8);
        CHECK_EQ(p.rq.pop<u32>(), 0x11111111u);
        CHECK_EQ(p.rq.pop<u32>(), 0x22222222u);
        CHECK_EQ(p.rq.pop<u32>(), 0u);  // past the end: zero
        CHECK(p.rq.a.empty() && p.rq.b.empty() && p.rq.c.empty() && p.rq.x.empty());
        CHECK(!p.domain_close);
    }
    // A descriptor (3 words: size low, address low, high bits) and an X descriptor (2 words) move the body.
    {
        alignas(16) u8 msg[0x100] = {};
        put32(msg, 0, 4 | (1u << 16) | (1u << 20));  // one X, one A
        // X: size 0x20, address bits 32-35 = 5 (word bits 12-15), counter 0x602 (bits 9-11 = 3, bits 0-5 = 2)
        put32(msg, 8, (0x20u << 16) | (0x5u << 12) | (0x3u << 9) | 0x2);
        put32(msg, 12, 0xABCDEF00);                         //    address low
        put32(msg, 16, 0x100);                              // A: size low
        put32(msg, 20, 0x12345678);                         //    address low
        put32(msg, 24, (0x3u << 28) | (0x1u << 24));        //    address bits 32-35 = 3, size bits 32-35 = 1
        // descriptors end at 28 -> body at 32; raw words cover 28..56
        put32(msg, 4, 7);
        put32(msg, 32, kSfci);
        put32(msg, 40, 7);
        put32(msg, 48, 0xAAAA);
        Parsed p;
        parse(msg, p, false);
        CHECK_EQ(p.rq.command, 7);
        CHECK_EQ(p.rq.x.size(), 1u);
        CHECK_EQ(p.rq.x[0].addr, 0x5ABCDEF00ull);
        CHECK_EQ(p.rq.x[0].size, 0x20u);
        // The counter's bits 9-11 come from word bits 9-11, not from the address bits 32-34 at word bits
        // 12-14 (the old (d0 >> 3) & 0xE00 read 2 | 0xA00 here); both are set so the two readings differ.
        CHECK_EQ(p.rq.x_index[0], 0x602u);
        CHECK_EQ(p.rq.a.size(), 1u);
        CHECK_EQ(p.rq.a[0].addr, 0x312345678ull);
        CHECK_EQ(p.rq.a[0].size, 0x100000100ull);
        CHECK_EQ(p.rq.args_size, 8);
        CHECK_EQ(p.rq.pop<u32>(), 0xAAAAu);
        CHECK_EQ(p.rq.in_buffer(0).addr, 0x312345678ull);  // A preferred over X
        CHECK_EQ(p.rq.out_buffer(0).addr, 0ull);            // no B or C
    }
    // Handle descriptor: send-PID flag, one copy handle, one move handle.
    {
        alignas(16) u8 msg[0x100] = {};
        put32(msg, 0, 4);
        put32(msg, 4, 7 | 0x80000000u);  // has handles; raw words cover 28..56
        put32(msg, 8, 1 | (1u << 1) | (1u << 5));
        u64 pid = 0x1234;
        memcpy(msg + 12, &pid, 8);
        put32(msg, 20, 0xAAAA);
        put32(msg, 24, 0xBBBB);
        put32(msg, 32, kSfci);
        put32(msg, 40, 3);
        Parsed p;
        parse(msg, p, false);
        CHECK(p.rq.send_pid);
        CHECK_EQ(p.rq.pid, 0x1234ull);
        CHECK_EQ(p.rq.copy_handles.size(), 1u);
        CHECK_EQ(p.rq.copy_handles[0], 0xAAAAu);
        CHECK_EQ(p.rq.move_handles.size(), 1u);
        CHECK_EQ(p.rq.move_handles[0], 0xBBBBu);
        CHECK_EQ(p.rq.command, 3);
        CHECK_EQ(p.rq.args_size, 8);
    }
    // Domain request: a 16-byte domain header (command, object count, payload length, object id) precedes
    // the CMIF body; the input object ids follow the payload.
    {
        alignas(16) u8 msg[0x100] = {};
        put32(msg, 0, 4);
        put32(msg, 4, 13);  // raw words cover 8..60
        msg[16] = 1;        // domain command: send
        msg[17] = 1;        // one input object
        u16 len = 24;       // CMIF header + 8 bytes of arguments
        memcpy(msg + 18, &len, 2);
        put32(msg, 20, 3);  // target object
        put32(msg, 32, kSfci);
        put32(msg, 40, 5);
        put32(msg, 48, 0x55);
        put32(msg, 52, 0x66);
        put32(msg, 56, 7);  // input object id
        Parsed p;
        parse(msg, p, true);
        CHECK_EQ(p.domain_cmd, 1u);
        CHECK_EQ(p.domain_object, 3u);
        CHECK(!p.domain_close);
        CHECK_EQ(p.rq.command, 5);
        CHECK_EQ(p.rq.in_objects.size(), 1u);
        CHECK_EQ(p.rq.in_objects[0], 7u);
        CHECK_EQ(p.rq.args_size, 12);  // pin: the raw words include the object id list
        CHECK_EQ(p.rq.pop<u32>(), 0x55u);
        CHECK_EQ(p.rq.pop<u32>(), 0x66u);
        msg[16] = 2;  // close object
        Parsed q;
        parse(msg, q, true);
        CHECK(q.domain_close);
    }
    // Reply framing: SFCO header at the aligned body, result at +8, data at +16; raw word count pins.
    {
        alignas(16) u8 msg[0x100];
        memset(msg, 0xCC, sizeof(msg));
        Response rs;
        rs.result = 0x1234;
        rs.push<u32>(7);
        rs.push<u32>(9);
        write_response(msg, rs, false, {}, {});
        CHECK_EQ(get32(msg, 0), 0u);
        CHECK_EQ(get32(msg, 4), 12u);  // ((40 - 8) + 3) / 4 + 4, no handles
        CHECK_EQ(get32(msg, 16), kSfco);
        CHECK_EQ(get32(msg, 20), 0u);
        CHECK_EQ(get32(msg, 24), 0x1234u);
        CHECK_EQ(get32(msg, 32), 7u);
        CHECK_EQ(get32(msg, 36), 9u);
        // with handles: descriptor word, copy handles, then the response's and the extra move handles
        memset(msg, 0xCC, sizeof(msg));
        rs.copy_handles = {0x66};
        rs.move_handles = {0x77};
        write_response(msg, rs, false, {}, {0x88});
        CHECK_EQ(get32(msg, 4), 12u | 0x80000000u);
        CHECK_EQ(get32(msg, 8), (1u << 1) | (2u << 5));
        CHECK_EQ(get32(msg, 12), 0x66u);
        CHECK_EQ(get32(msg, 16), 0x77u);
        CHECK_EQ(get32(msg, 20), 0x88u);
        CHECK_EQ(get32(msg, 32), kSfco);
        CHECK_EQ(get32(msg, 40), 0x1234u);
        CHECK_EQ(get32(msg, 48), 7u);
        // domain reply: 16-byte header with the returned object count, ids after the data
        memset(msg, 0xCC, sizeof(msg));
        Response rd;
        rd.push<u32>(1);
        rd.push<u32>(2);
        write_response(msg, rd, true, {5, 6}, {});
        CHECK_EQ(get32(msg, 4), 18u);
        CHECK_EQ(get32(msg, 16), 2u);   // object count
        CHECK_EQ(get32(msg, 20), 0u);
        CHECK_EQ(get32(msg, 32), kSfco);
        CHECK_EQ(get32(msg, 40), 0u);   // result success
        CHECK_EQ(get32(msg, 48), 1u);
        CHECK_EQ(get32(msg, 52), 2u);
        CHECK_EQ(get32(msg, 56), 5u);
        CHECK_EQ(get32(msg, 60), 6u);
    }
    // Response::push alignment: a u8 then a u64 lands the u64 at offset 8
    {
        Response r;
        r.push<u8>(1);
        r.push<u64>(2);
        CHECK_EQ(r.data.size(), 16u);
        u64 v;
        memcpy(&v, r.data.data() + 8, 8);
        CHECK_EQ(v, 2ull);
    }
}

// ------------------------------------------------------------------ kernel handle table (kernel/handles.cpp)
struct TestObject : kern::Object {
    const char* type_name() const override { return "TestObject"; }
};

static void test_handle_table() {
    using namespace kern;
    HandleTable t;
    auto a = std::make_shared<TestObject>();
    auto b = std::make_shared<TestObject>();
    Handle ha = t.add(a);
    Handle hb = t.add(b);
    CHECK_EQ(ha, 0x8010u);   // (1 << 15) | 0x10: HOS-like values, never zero
    CHECK_EQ(hb, 0x10010u);
    CHECK(ha != hb);
    CHECK(t.get(ha) == a);
    CHECK(t.get(hb) == b);
    CHECK(t.get_as<TestObject>(ha) == a);
    CHECK(t.get(0) == nullptr);
    CHECK(t.get(0x18010) == nullptr);
    CHECK(t.get(kCurrentThread) == nullptr);  // no current thread in the test: nothing to find
    CHECK(t.close(ha));
    CHECK(t.get(ha) == nullptr);
    CHECK(!t.close(ha));
    CHECK(t.get(hb) == b);
    CHECK_EQ(t.add(a), 0x18010u);  // the counter never reuses a value
    CHECK_EQ(a.use_count(), 2);     // held by the table once (the closed entry released its reference)
}

int main() {
    test_formats();
    test_pipeline_key_mappers();
    test_texture_layout();
    test_hipc();
    test_handle_table();
    if (g_failures) {
        fprintf(stderr, "runtime_unit_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("runtime_unit_test: %d checks passed\n", g_checks);
    return 0;
}
