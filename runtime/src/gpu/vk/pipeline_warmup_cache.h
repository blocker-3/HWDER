// Disposable startup pipeline recipes. No Vulkan objects or guest memory are persisted.
#pragma once

#include <array>
#include <cstddef>
#include <future>
#include <string>
#include <vector>

#include "shader/shader.h"

namespace gpu::warmup {

// Keep fields/order identical to the renderer's original key. Files encode fields,
// never this object's representation; decode zeroes padding before assigning fields.
struct PipelineKey {
    u64 program;
    u32 color_formats[8];
    u32 depth_format;
    u32 num_colors;
    u32 topology;
    u32 patch_points;
    u32 attrib_format[32];
    u32 attrib_offset[32];
    u32 attrib_binding[32];
    u32 binding_stride[32];
    u32 cull_enable, cull_mode, front_face, polygon_mode, depth_bias, discard, depth_clamp;
    u32 depth_test, depth_write, depth_func, depth_bounds;
    u32 stencil_enable, front[4], back[4];
    u32 blend_enable[8], blend[8][6], color_mask[8];
    u32 logic_enable, logic;
    u32 prim_restart;
};

inline constexpr u32 kMaxPrograms = 512;
inline constexpr u32 kMaxPipelines = 8192;
inline constexpr size_t kMaxFileBytes = 64 * 1024 * 1024;
inline constexpr u32 kMaxSpirvWords = 262144;
inline constexpr u32 kMaxResources = 256;
inline constexpr u32 kMaxTextureCount = 256;

struct StageRecord {
    bool present = false;
    shader::Program program{};
};
struct ProgramRecord {
    u64 key = 0;
    u64 fingerprint = 0;
    std::array<StageRecord, 5> stages{};
};
struct PipelineRecord {
    PipelineKey key{};
    u32 program_index = 0;
    u64 program_fingerprint = 0;
};
struct Cache {
    std::vector<ProgramRecord> programs;
    std::vector<PipelineRecord> pipelines;
};

enum class Status { Ok, NotFound, Incompatible, Invalid, IoError };
const char* name(Status status);

// compatibility_tag covers the translator/build, layouts and push-constant ABI.
// On failure load leaves out unchanged. Save validates before creating a unique
// sibling temporary file, flushes it, then atomically replaces path on Windows.
Status load(const char* path, u64 compatibility_tag, Cache& out);
struct SaveTimings { double validation_ms = 0, serialization_ms = 0, disk_ms = 0; };
Status save(const char* path, u64 compatibility_tag, const Cache& cache, SaveTimings* timings = nullptr);

struct SaveResult { Status status = Status::IoError; SaveTimings timings; size_t recipes = 0; };
// One immutable snapshot in flight. The caller retains live history and retries
// failures; a completed task must be polled before another snapshot can start.
// Destruction waits for the owned task; atomic replacement protects an older file
// if the process is terminated without running destructors.
class SaveTask {
public:
    bool pending() const { return result_.valid(); }
    bool start(std::string path, u64 compatibility_tag, Cache&& snapshot, bool profile = false);
    bool poll(SaveResult& result);
private:
    std::future<SaveResult> result_;
};

// Union compatible private history with newly prepared catalog recipes. Exact
// program/key duplicates are reused; distinct same-key shader variants survive.
// Invalid/oversized input or allocation failure leaves retained unchanged.
Status merge(Cache& retained, const Cache& additional);

// Fingerprints are only a lookup filter, not proof of equality. They include the
// key and all present stages' fields; stored fingerprint and absent payloads are ignored.
u64 fingerprint(const ProgramRecord& program);
bool equal(const shader::Program& a, const shader::Program& b);
bool equal(const ProgramRecord& a, const ProgramRecord& b);

// Metadata codecs shared with the distributable catalog. Never includes shader data.
std::vector<u32> key_words(const PipelineKey& key);
bool key_from_words(const std::vector<u32>& words, PipelineKey& key);
Status save_metadata_atomic(const char* path, const std::vector<u8>& bytes);

}  // namespace gpu::warmup
