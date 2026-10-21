// Diagnostics of the Vulkan renderer: the per-draw log (HWDER_DRAWLOG), the draw-path profile (HWDER_PROFILE_MS),
// frame dumps (HWDER_DUMP_FRAMES, HWDER_VK_FRAME_DUMP) and the filmstrip capture (HWDER_FILMSTRIP). Moved
// text-for-text from vk_renderer.cpp.
#include "interp/interp.h"
#include "vk_renderer.h"
#include "debug_flags.h"

namespace gpu::vk::detail {

// HWDER_DRAWLOG=1 (with HWDER_FILMSTRIP): one line per draw into <filmstrip dir>/drawlog.txt while the
// filmstrip capture window is open, keyed by present number, to find the draw behind a bad frame.
static bool g_drawlog_window = false;
FILE* g_drawlog = nullptr;
bool drawlog_active() {
    static const bool on = debug::flag(debug::Drawlog) && debug::flag(debug::Filmstrip);
    if (!on || !g_drawlog_window) return false;
    if (!g_drawlog) {
        std::string path = std::string(debug::str(debug::Filmstrip)) + "/drawlog.txt";
        g_drawlog = fopen(path.c_str(), "w");
    }
    return g_drawlog != nullptr;
}

const u64 g_start_ms = GetTickCount64();

using gpu::vk::g_prof;
using gpu::vk::g_prof_draws;
double prof_flush() {
    double total = 0;
    for (double v : g_prof) total += v;
    static const double limit = debug::value(debug::ProfileMs, 200.0);
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

void dump_frame(VkImage image, u32 w, u32 h, VkFormat fmt) {
    static const char* dir = debug::str(debug::DumpFrames);
    static u32 n = 0;
    if (!dir) return;
    // Note: each dump waits for the GPU and writes an 8 MB BMP (a visible hitch) - keep `every` large.
    u32 every = (u32)debug::value(debug::DumpEvery, 600);
    // HWDER_DUMP_AFTER=<secs>: no dumps (and no dump stalls) before that time, so the run reaches
    // the interesting part at normal speed.
    static double after_s = debug::value(debug::DumpAfter, 0.0);
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
void write_image_bmp(VkImage image, u32 w, u32 h, VkFormat fmt, const char* path, VkImageAspectFlags aspect) {
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
void filmstrip_capture(VkImage src, u32 w, u32 h, const char* dir) {
    // HWDER_FILMSTRIP_W=<width> (default 96): thumbnail width, 16:9.
    static const u32 SW = debug::flag(debug::FilmstripW) ? std::max(16, atoi(debug::str(debug::FilmstripW))) : 96, SH = SW * 9 / 16;
    // HWDER_FILMSTRIP_BATTLE=<s>: start that long after the first battle-sized frame (> 450 draws);
    // HWDER_FILMSTRIP_AFTER=<s>: or at that time since boot; HWDER_FILMSTRIP_FOR=<s>: stop after that long.
    {
        static const char* battle = debug::str(debug::FilmstripBattle);
        static const double dur = debug::value(debug::FilmstripFor, 1e9);
        static const double after = debug::value(debug::FilmstripAfter, 0.0);
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

}  // namespace gpu::vk::detail
