// Distributable pipeline SETTINGS only. Keep this model free of shader::Program,
// SPIR-V, instruction/header bytes, resource chunks and native cache objects.
#pragma once
#include <array>
#include <functional>
#include <string>
#include "gpu/vk/pipeline_warmup_cache.h"

namespace gpu::catalog {
using Digest = std::array<u8, 32>;
constexpr size_t kMaxBytes = 16 * 1024 * 1024;
constexpr u32 kMaxSourceBytes = 0x100050;
constexpr u64 kTitleId = 0x0100AE00096EA000ull;
struct Locator {
    std::string resource;  // relative RomFS filename; never a host path
    u64 offset = 0;        // SPH offset inside the uncompressed NVN resource
    u32 size = 0;          // SPH through terminating BRA, inclusive
    Digest sha256{};
};
struct Stage {
    bool present = false;
    Locator source;
    u64 attributes = 0;
    u32 render_targets = 0, bound_buffer = 2;
    shader::Options options{};
};
struct Program {
    u64 key = 0, expected_translation = 0;
    std::array<Stage, 5> stages{};
};
struct Pipeline { u32 program = 0; warmup::PipelineKey key{}; };
struct Catalog {
    u64 build = 0, title = kTitleId;
    u32 game_version = 0;
    std::string scope;  // collector-supplied stage/route; not a coverage guarantee
    std::vector<Program> programs;
    std::vector<Pipeline> pipelines;
};
using Status = warmup::Status;
bool safe_resource(const std::string& path);
Digest digest(const void* bytes, size_t size);
bool source_identity(const u8* bytes, size_t available, u32 stage, u32& size, Digest& sha);
Status encode(const Catalog& value, std::vector<u8>& out);
Status decode(const std::vector<u8>& bytes, u64 build, u32 game_version, Catalog& out);
Status load(const char* path, u64 build, u32 game_version, Catalog& out);
Status save(const char* path, const Catalog& value);

// The reader must freshly read the selected local game resource. Resolution is
// checked again here before translation. No private warmup-cache input exists.
using ReadSource = std::function<bool(const Locator&, std::vector<u8>&)>;
struct Progress { u32 done = 0, total = 0, resolved = 0, unresolved = 0; };
warmup::Cache prepare(const Catalog&, const ReadSource&, const std::function<void(const Progress&)>& progress);
}  // namespace gpu::catalog
