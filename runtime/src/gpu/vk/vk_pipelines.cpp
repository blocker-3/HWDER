// Shader and pipeline management of the Vulkan renderer: descriptor-set layouts, shader translation, graphics
// programs, the pipeline cache and its worker thread, the prewarm/warmup records and the pipeline catalog, with
// the start_pipeline_prewarm / prepare_pipeline_catalog entry points. Moved text-for-text from vk_renderer.cpp.
#include "gpu/vk/catalog_game_sources.h"
#include "gpu/prelaunch.h"
#include "fs/nsp.h"
#include "hwder_version.h"
#include "vk_renderer.h"
#include "debug_flags.h"

namespace gpu::vk::detail {
using namespace m3d;

const char* kPipelineCacheFile = "pipeline_cache.bin";
static const char* kWarmupFile = "pipeline_warmup.bin";
static const char* kCatalogFile = "pipeline_catalog.bin";
static catalog::GameSources catalog_sources;
static bool catalog_recording = false, catalog_deferred = false;
static bool catalog_enabled() {
    const char* value = debug::str(debug::PipelineCatalog);
    return !value || strcmp(value,"0") != 0;
}
static u64 catalog_build() {
    constexpr char epoch[] = "HWDER metadata pipeline catalog ABI 1";
    u64 tag = hash_bytes(epoch,sizeof(epoch));
    return hash_bytes(HWDER_COMMIT,sizeof(HWDER_COMMIT),tag);
}
static u32 catalog_game_version() { return hwfs::nsp::active() && hwfs::nsp::info().has_version ? hwfs::nsp::info().version : 0; }
static bool catalog_debug_translation() { return debug::flag(debug::ShaderNoKil) || debug::flag(debug::ShaderFallback); }

static bool prewarm_enabled() {
    const char* value = debug::str(debug::PipelinePrewarm);
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
    if (debug::flag(debug::DumpShaders)) {
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
    const char* scope=debug::str(debug::PipelineCatalogRecord);
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
    warmup::SaveResult completed;
    if (warmup_save_.poll(completed)) {
        // New records/mismatches since submission already set dirty again. A
        // successful old snapshot must never clear those later changes.
        if (completed.status != warmup::Status::Ok) warmup_dirty_ = true;
        hw_log("vk: prewarm background save: %s, %zu recipes retained", warmup::name(completed.status), completed.recipes);
        if (debug::flag(debug::CacheProfile)) hw_log("cache-save: background validation %.3f ms, serialization %.3f ms, disk %.3f ms",
            completed.timings.validation_ms, completed.timings.serialization_ms, completed.timings.disk_ms);
    }
    if (warmup_save_.pending()) return;
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
    static const bool synchronous = debug::flag(debug::CacheSaveSync) && strcmp(debug::str(debug::CacheSaveSync), "1") == 0;
    if (!synchronous) {
        if (warmup_save_.start(kWarmupFile, warmup_compatibility(), std::move(snapshot), debug::flag(debug::CacheProfile))) {
            warmup_dirty_ = false;
            if (debug::flag(debug::CacheProfile)) hw_log("cache-save: background snapshot %.3f ms, submit %.3f ms",
                std::chrono::duration<double, std::milli>(copied-begin).count(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-copied).count());
        } else hw_log("vk: prewarm background save unavailable; retaining dirty history for retry");
        return;
    }
    warmup::SaveTimings timings;
    const auto status = warmup::save(kWarmupFile, warmup_compatibility(), snapshot, debug::flag(debug::CacheProfile) ? &timings : nullptr);
    if (status == warmup::Status::Ok) warmup_dirty_ = false;
    hw_log("vk: prewarm save: %s, %zu recipes retained, %llu newly recorded this run",
           warmup::name(status), snapshot.pipelines.size(), (unsigned long long)warmup_recorded_);
    if (debug::flag(debug::CacheProfile)) hw_log("cache-save: snapshot %.3f ms, validation %.3f ms, serialization %.3f ms, disk %.3f ms",
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
    static const bool sync_pipes = debug::flag(debug::SyncPipelines);
    // Light frames (menus, the first frames of a scene) compile synchronously: a 100 ms hitch behind a
    // fade is invisible, while skipping their draws shows a half-drawn scene. Heavy battle frames go
    // async so a new effect's pipeline does not stall the whole frame.
    // Draws that sample a render target (screen copies, effect composites, post-processing) also
    // compile synchronously: skipping one leaves its output, or the copy a later pass reads, empty,
    // which showed as a one-frame black box the first time an attack effect was used.
    // HWDER_ASYNC_RT_PIPELINES=1 restores async compiles for them (for comparison).
    static const bool async_rt = debug::flag(debug::AsyncRtPipelines);
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

}  // namespace gpu::vk::detail

namespace gpu {
using namespace vk::detail;

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
    const char* record=debug::str(debug::PipelineCatalogRecord);
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
