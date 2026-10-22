// Presenting a frame of the Vulkan renderer: the picture post-process (FXAA / sharpening on the tonemapped
// picture) and present(), which picks the presented image, blacks out stale ultrawide side bands, runs the frame
// diagnostics and hands the image to the display. Moved text-for-text from vk_renderer.cpp.
#include "gpu/settings.h"
#include "gpu/diag.h"
#include "interp/interp.h"
#include "post_spv.h"
#include "vk_renderer.h"
#include "debug_flags.h"

namespace gpu::vk::detail {

// Draws in the last presented frame (interp/interp.h: read by the HWDER_AUTOPLAY=2 input script to tell battle from menus).
extern "C" std::atomic<u32> hw_last_frame_draws{0};

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
            static const bool ptrace = debug::flag(debug::PresentTrace);
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
            static const char* strip_dir = debug::str(debug::Filmstrip);
            if (strip_dir) filmstrip_capture(image, w, h, strip_dir);
        }
        // Fill ultrawide: the orthographic HUD (and movies, which the game draws as a 2D quad) stays in
        // the centred 16:9; when nothing full-width (a 3D frame's post passes, a full clear) wrote this
        // frame, the side bands hold stale memory. Clear them to black before presenting.
        // "Since it was last presented", not "during this present's frame number": with frames queued ahead
        // (the 120 fps mode's triple buffering) the GPU thread draws the next frame before this one is presented,
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
            static const bool env_scale = debug::flag(debug::ResScale) || debug::flag(debug::ResScaleX) || debug::flag(debug::ResScaleY);
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
        static const char* auto_env = debug::str(debug::VkFrameDumpMinDraws);
        static const u64 auto_min_draws = auto_env ? strtoull(auto_env, nullptr, 0) : 600;
        static const u64 auto_max_draws = auto_env && strchr(auto_env, '-') ? strtoull(strchr(auto_env, '-') + 1, nullptr, 0) : ~0ull;
        if (dump_auto_ && st_.draws >= auto_min_draws && st_.draws <= auto_max_draws &&
            GetTickCount64() - g_start_ms >= (u64)(dump_auto_after_ * 1000.0)) {
            dump_auto_ = false;
            // HWDER_VK_FRAME_DUMP_SKIP=n dumps the n-th present after the trigger instead (menu frames
            // straddle presents: a 44-draw "frame" is followed by a 7-draw one, then the next full one).
            static const u64 skip = debug::value(debug::VkFrameDumpSkip, (u64)0, 0);
            // HWDER_VK_FRAME_DUMP_DELAY=<secs>: arm the time trigger that long after the draw-count trigger
            // (e.g. the Select Mode menu is complete ~3 s after its first 44-draw parchment frame).
            static const double delay = debug::value(debug::VkFrameDumpDelay, 0.0);
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

}  // namespace gpu::vk::detail
