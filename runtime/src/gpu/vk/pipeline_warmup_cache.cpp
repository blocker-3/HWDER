#include "pipeline_warmup_cache.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <tuple>
#include <windows.h>

namespace gpu::warmup {
namespace {
constexpr u64 kHashSeed = 14695981039346656037ull;
constexpr u64 kHashPrime = 1099511628211ull;
constexpr u32 kVersion = 1;
constexpr size_t kHeaderBytes = 40;
constexpr u8 kMagic[8] = {'H', 'W', 'D', 'P', 'W', 'M', '0', '1'};

struct Encoder {
    std::vector<u8>* bytes = nullptr;
    u64 hash = kHashSeed;
    size_t size = 0;
    bool ok = true;
    void byte(u8 v) {
        if (size >= kMaxFileBytes) { ok = false; return; }
        ++size;
        hash = (hash ^ v) * kHashPrime;
        if (bytes) bytes->push_back(v);
    }
    void word(u32 v) { for (int i = 0; i < 4; ++i) byte((u8)(v >> (8 * i))); }
    void wide(u64 v) { word((u32)v); word((u32)(v >> 32)); }
    void flag(bool v) { word(v ? 1 : 0); }
};

struct Decoder {
    const std::vector<u8>& bytes;
    size_t pos = 0;
    bool ok = true;
    size_t left() const { return bytes.size() - pos; }
    u32 word() {
        if (left() < 4) { ok = false; return 0; }
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= u32(bytes[pos++]) << (8 * i);
        return v;
    }
    u64 wide() { u64 lo = word(); return lo | (u64(word()) << 32); }
    bool flag() { u32 v = word(); if (v > 1) ok = false; return v != 0; }
    u32 count(u32 maximum, size_t minimum_bytes = 4) {
        u32 n = word();
        if (n > maximum || n > left() / minimum_bytes) { ok = false; return 0; }
        return n;
    }
};

void write_texture(Encoder& e, const shader::TextureUse& t) {
    e.word((u32)t.type); e.flag(t.is_depth); e.flag(t.is_multisample);
    e.word(t.cbuf_index); e.word(t.cbuf_offset); e.flag(t.has_secondary);
    e.word(t.secondary_cbuf_index); e.word(t.secondary_cbuf_offset); e.word(t.count);
}
shader::TextureUse read_texture(Decoder& d) {
    shader::TextureUse t{};
    t.type = (shader::TextureType)d.word(); t.is_depth = d.flag(); t.is_multisample = d.flag();
    t.cbuf_index = d.word(); t.cbuf_offset = d.word(); t.has_secondary = d.flag();
    t.secondary_cbuf_index = d.word(); t.secondary_cbuf_offset = d.word(); t.count = d.word();
    return t;
}

void write_program(Encoder& e, const shader::Program& p) {
    e.word((u32)p.stage);
    e.word((u32)p.spirv.size());
    for (u32 word : p.spirv) e.word(word);
    const auto& r = p.resources;
    e.word((u32)r.const_buffers.size());
    for (const auto& b : r.const_buffers) { e.word(b.index); e.word(b.size); }
    e.word((u32)r.storage_buffers.size());
    for (const auto& b : r.storage_buffers) { e.word(b.cbuf_index); e.word(b.cbuf_offset); e.flag(b.written); }
    e.word((u32)r.texel_buffers.size()); for (const auto& t : r.texel_buffers) write_texture(e, t);
    e.word((u32)r.textures.size()); for (const auto& t : r.textures) write_texture(e, t);
    e.word((u32)r.images.size());
    for (const auto& i : r.images) {
        e.word((u32)i.type); e.word(i.cbuf_index); e.word(i.cbuf_offset);
        e.flag(i.is_written); e.flag(i.is_read); e.word(i.format_hint);
    }
    e.word(p.attributes_read); e.word(p.outputs_written); e.flag(p.writes_depth); e.flag(p.uses_discard);
    for (u32 n : p.local_size) e.word(n);
    e.word(p.shared_memory_size); e.word(p.local_memory_size);
    e.word(p.geometry_output_topology); e.word(p.geometry_max_vertices); e.word(p.geometry_invocations);
}

shader::Program read_program(Decoder& d) {
    shader::Program p{};
    p.stage = (shader::Stage)d.word();
    p.spirv.resize(d.count(kMaxSpirvWords)); for (u32& v : p.spirv) v = d.word();
    auto& r = p.resources;
    r.const_buffers.resize(d.count(kMaxResources, 8));
    for (auto& b : r.const_buffers) { b.index = d.word(); b.size = d.word(); }
    r.storage_buffers.resize(d.count(kMaxResources, 12));
    for (auto& b : r.storage_buffers) { b.cbuf_index = d.word(); b.cbuf_offset = d.word(); b.written = d.flag(); }
    r.texel_buffers.resize(d.count(kMaxResources, 36)); for (auto& t : r.texel_buffers) t = read_texture(d);
    r.textures.resize(d.count(kMaxResources, 36)); for (auto& t : r.textures) t = read_texture(d);
    r.images.resize(d.count(kMaxResources, 24));
    for (auto& i : r.images) {
        i.type = (shader::TextureType)d.word(); i.cbuf_index = d.word(); i.cbuf_offset = d.word();
        i.is_written = d.flag(); i.is_read = d.flag(); i.format_hint = d.word();
    }
    p.attributes_read = d.word(); p.outputs_written = d.word(); p.writes_depth = d.flag(); p.uses_discard = d.flag();
    for (u32& n : p.local_size) n = d.word();
    p.shared_memory_size = d.word(); p.local_memory_size = d.word();
    p.geometry_output_topology = d.word(); p.geometry_max_vertices = d.word(); p.geometry_invocations = d.word();
    return p;
}

// One explicit field list serves key encoding/decoding without copying padding.
template<class F> void key_fields(PipelineKey& k, F field) {
    for (u32& v : k.color_formats) field(v);
    field(k.depth_format); field(k.num_colors); field(k.topology); field(k.patch_points);
    for (u32& v : k.attrib_format) field(v);
    for (u32& v : k.attrib_offset) field(v);
    for (u32& v : k.attrib_binding) field(v);
    for (u32& v : k.binding_stride) field(v);
    field(k.cull_enable); field(k.cull_mode); field(k.front_face); field(k.polygon_mode);
    field(k.depth_bias); field(k.discard); field(k.depth_clamp);
    field(k.depth_test); field(k.depth_write); field(k.depth_func); field(k.depth_bounds);
    field(k.stencil_enable);
    for (u32& v : k.front) field(v); for (u32& v : k.back) field(v);
    for (u32& v : k.blend_enable) field(v);
    for (auto& row : k.blend) for (u32& v : row) field(v);
    for (u32& v : k.color_mask) field(v);
    field(k.logic_enable); field(k.logic); field(k.prim_restart);
}
void write_key(Encoder& e, const PipelineKey& original) {
    PipelineKey k = original;
    e.wide(k.program);
    key_fields(k, [&](u32 v) { e.word(v); });
}
void read_key(Decoder& d, PipelineKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.program = d.wide();
    key_fields(k, [&](u32& v) { v = d.word(); });
}
void write_record_content(Encoder& e, const ProgramRecord& p) {
    e.wide(p.key);
    for (const auto& s : p.stages) { e.flag(s.present); if (s.present) write_program(e, s.program); }
}

bool valid_spirv(const shader::Program& p) {
    const auto& w = p.spirv;
    if (w.size() < 5 || w.size() > kMaxSpirvWords || w[0] != 0x07230203 ||
        w[1] < 0x00010000 || w[1] > 0x00010600 || (w[1] & 0xff) ||
        !w[3] || w[3] > 0x01000000 || w[4]) return false;
    static constexpr u32 models[] = {0, 0, 1, 2, 3, 4, 5};
    bool main_entry = false, memory_model = false;
    for (size_t pos = 5; pos < w.size();) {
        u32 count = w[pos] >> 16, op = w[pos] & 0xffff;
        if (!count || count > w.size() - pos) return false;
        if (op == 14) { if (count != 3) return false; memory_model = true; }
        if (op == 15) {
            if (count < 5 || w[pos + 1] != models[(u32)p.stage] || !w[pos + 2] || w[pos + 2] >= w[3]) return false;
            // All renderer pipelines use pName="main"; its zero terminator occupies a second word.
            if (w[pos + 3] != 0x6e69616d || w[pos + 4] != 0) return false;
            main_entry = true;
        }
        pos += count;
    }
    return main_entry && memory_model;
}
bool cbuf(u32 index, u32 offset) { return index < 18 && offset <= 0xfffc && !(offset & 3); }
bool valid_texture(const shader::TextureUse& t) {
    return (u32)t.type <= (u32)shader::TextureType::Buffer && cbuf(t.cbuf_index, t.cbuf_offset) &&
           cbuf(t.secondary_cbuf_index, t.secondary_cbuf_offset) && t.count && t.count <= kMaxTextureCount;
}
bool valid_program(const shader::Program& p) {
    if ((u32)p.stage > (u32)shader::Stage::Compute || !valid_spirv(p)) return false;
    const auto& r = p.resources;
    if (r.const_buffers.size() > kMaxResources || r.storage_buffers.size() > kMaxResources ||
        r.texel_buffers.size() > kMaxResources || r.textures.size() > kMaxResources || r.images.size() > kMaxResources) return false;
    u32 total = (u32)(r.const_buffers.size() + r.storage_buffers.size() + r.images.size());
    for (const auto& b : r.const_buffers) if (b.index >= 18 || !b.size || b.size > 0x10000) return false;
    for (const auto& b : r.storage_buffers) if (!cbuf(b.cbuf_index, b.cbuf_offset) || b.cbuf_offset > 0xfff0) return false;
    for (const auto* list : {&r.texel_buffers, &r.textures}) for (const auto& t : *list) {
        if (!valid_texture(t)) return false;
        total += t.count;
    }
    for (const auto& i : r.images)
        if ((u32)i.type > (u32)shader::TextureType::Buffer || !cbuf(i.cbuf_index, i.cbuf_offset) || i.format_hint > 0xffff) return false;
    for (u32 n : p.local_size) if (!n || n > 1024) return false;
    if ((u64)p.local_size[0] * p.local_size[1] * p.local_size[2] > 1024 || total > 4096) return false;
    return p.shared_memory_size <= 0x100000 && p.local_memory_size <= 0x1000000 &&
           (p.geometry_output_topology == 0 || (p.geometry_output_topology >= 27 && p.geometry_output_topology <= 29)) &&
           p.geometry_max_vertices <= 1024 && p.geometry_invocations >= 1 && p.geometry_invocations <= 32;
}
bool format(u32 f) { return f <= 184 || f == 1000340000 || f == 1000340001; }
bool valid_key(const PipelineKey& k) {
    if (k.num_colors > 8 || k.topology > 10 || k.patch_points > 32 ||
        (k.depth_format && (k.depth_format < 124 || k.depth_format > 130))) return false;
    for (u32 v : k.color_formats) if (!format(v)) return false;
    for (u32 v : k.attrib_format) if (!format(v)) return false;
    for (u32 v : k.attrib_offset) if (v > 0xffff) return false;
    for (u32 v : k.attrib_binding) if (v >= 32) return false;
    for (u32 v : k.binding_stride) if (v > 0x100000) return false;
    for (u32 v : {k.cull_enable, k.depth_bias, k.discard, k.depth_clamp, k.depth_test,
                  k.depth_write, k.depth_bounds, k.stencil_enable, k.logic_enable, k.prim_restart}) if (v > 1) return false;
    if (k.cull_mode > 3 || k.front_face > 1 || k.polygon_mode > 2 || k.depth_func > 7 || k.logic > 15) return false;
    for (u32 v : k.front) if (v > 7) return false;
    for (u32 v : k.back) if (v > 7) return false;
    for (u32 i = 0; i < 8; ++i) {
        if (k.blend_enable[i] > 1 || k.color_mask[i] > 15 || k.blend[i][0] > 4 || k.blend[i][3] > 4) return false;
        for (u32 j : {1u, 2u, 4u, 5u}) if (k.blend[i][j] > 18) return false;
    }
    return true;
}
bool valid_cache(const Cache& cache) {
    if (cache.programs.size() > kMaxPrograms || cache.pipelines.size() > kMaxPipelines) return false;
    for (const auto& p : cache.programs) {
        if (!p.stages[0].present) return false;
        for (u32 i = 0; i < 5; ++i)
            if (p.stages[i].present && (p.stages[i].program.stage != (shader::Stage)(i + 1) || !valid_program(p.stages[i].program))) return false;
        if (p.fingerprint != fingerprint(p)) return false;
    }
    for (const auto& p : cache.pipelines) {
        if (p.program_index >= cache.programs.size() || !valid_key(p.key)) return false;
        const auto& program = cache.programs[p.program_index];
        if (p.key.program != program.key || p.program_fingerprint != program.fingerprint) return false;
    }
    return true;
}
template<class T, class Pred> bool same(const std::vector<T>& a, const std::vector<T>& b, Pred pred) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), pred);
}
bool same_texture(const shader::TextureUse& a, const shader::TextureUse& b) {
    return std::tie(a.type,a.is_depth,a.is_multisample,a.cbuf_index,a.cbuf_offset,a.has_secondary,a.secondary_cbuf_index,a.secondary_cbuf_offset,a.count) ==
           std::tie(b.type,b.is_depth,b.is_multisample,b.cbuf_index,b.cbuf_offset,b.has_secondary,b.secondary_cbuf_index,b.secondary_cbuf_offset,b.count);
}

Status write_atomic(const char* path, const std::vector<u8>& bytes) {
    static std::atomic<u64> serial{0};
    std::string temporary = std::string(path) + ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(++serial);
    HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Status::IoError;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    if (ok) ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = false;
    if (ok) ok = MoveFileExA(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileA(temporary.c_str());
    return ok ? Status::Ok : Status::IoError;
}
}  // namespace

std::vector<u32> key_words(const PipelineKey& original) {
    PipelineKey key = original;
    std::vector<u32> words{u32(key.program), u32(key.program >> 32)};
    key_fields(key, [&](u32 v) { words.push_back(v); });
    return words;
}
bool key_from_words(const std::vector<u32>& words, PipelineKey& key) {
    if (words.size() != 229) return false;
    std::memset(&key, 0, sizeof(key));
    key.program = u64(words[0]) | (u64(words[1]) << 32);
    size_t i = 2;
    key_fields(key, [&](u32& v) { v = words[i++]; });
    return valid_key(key);
}
Status save_metadata_atomic(const char* path, const std::vector<u8>& bytes) {
    if (!path || !*path || bytes.size() > kMaxFileBytes) return Status::Invalid;
    return write_atomic(path, bytes);
}

const char* name(Status status) {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::NotFound: return "not found";
    case Status::Incompatible: return "incompatible";
    case Status::Invalid: return "invalid";
    case Status::IoError: return "I/O error";
    }
    return "unknown";
}

u64 fingerprint(const ProgramRecord& p) {
    Encoder e;
    write_record_content(e, p);
    return e.ok ? e.hash : 0;
}

bool equal(const shader::Program& a, const shader::Program& b) {
    if (std::tie(a.stage,a.attributes_read,a.outputs_written,a.writes_depth,a.uses_discard,a.shared_memory_size,a.local_memory_size,
                 a.geometry_output_topology,a.geometry_max_vertices,a.geometry_invocations) !=
        std::tie(b.stage,b.attributes_read,b.outputs_written,b.writes_depth,b.uses_discard,b.shared_memory_size,b.local_memory_size,
                 b.geometry_output_topology,b.geometry_max_vertices,b.geometry_invocations) ||
        !std::equal(std::begin(a.local_size),std::end(a.local_size),std::begin(b.local_size)) || a.spirv != b.spirv) return false;
    const auto& x = a.resources; const auto& y = b.resources;
    return same(x.const_buffers,y.const_buffers,[](const auto& x,const auto& y){return std::tie(x.index,x.size)==std::tie(y.index,y.size);}) &&
           same(x.storage_buffers,y.storage_buffers,[](const auto& x,const auto& y){return std::tie(x.cbuf_index,x.cbuf_offset,x.written)==std::tie(y.cbuf_index,y.cbuf_offset,y.written);}) &&
           same(x.texel_buffers,y.texel_buffers,same_texture) && same(x.textures,y.textures,same_texture) &&
           same(x.images,y.images,[](const auto& x,const auto& y){return std::tie(x.type,x.cbuf_index,x.cbuf_offset,x.is_written,x.is_read,x.format_hint)==std::tie(y.type,y.cbuf_index,y.cbuf_offset,y.is_written,y.is_read,y.format_hint);});
}
bool equal(const ProgramRecord& a, const ProgramRecord& b) {
    if (a.key != b.key) return false;
    for (u32 i = 0; i < 5; ++i)
        if (a.stages[i].present != b.stages[i].present || (a.stages[i].present && !equal(a.stages[i].program,b.stages[i].program))) return false;
    return true;
}

Status save(const char* path, u64 compatibility_tag, const Cache& cache) {
    if (!path || !*path) return Status::IoError;
    try {
        if (!valid_cache(cache)) return Status::Invalid;
        std::vector<u8> payload;
        Encoder body{&payload};
        body.word((u32)cache.programs.size()); body.word((u32)cache.pipelines.size());
        for (const auto& p : cache.programs) {
            body.wide(p.fingerprint); write_record_content(body,p);
            if (!body.ok) return Status::Invalid;
        }
        for (const auto& p : cache.pipelines) { body.word(p.program_index); body.wide(p.program_fingerprint); write_key(body,p.key); }
        if (!body.ok || payload.size() > kMaxFileBytes - kHeaderBytes) return Status::Invalid;
        std::vector<u8> file;
        Encoder header{&file};
        for (u8 v : kMagic) header.byte(v);
        header.word(kVersion); header.word(0); header.wide(compatibility_tag); header.wide(payload.size());
        // Cover the header as well as the payload, excluding only the checksum itself.
        Encoder checksum;
        for (u8 v : file) checksum.byte(v);
        for (u8 v : payload) checksum.byte(v);
        header.wide(checksum.hash);
        file.insert(file.end(),payload.begin(),payload.end());
        return write_atomic(path,file);
    } catch (const std::exception&) { return Status::Invalid; }
}

Status load(const char* path, u64 compatibility_tag, Cache& out) {
    if (!path || !*path) return Status::IoError;
    try {
        FILE* opened = nullptr;
        errno_t error = fopen_s(&opened,path,"rb");
        std::unique_ptr<FILE,decltype(&fclose)> f(opened,fclose);
        if (!f) return error == ENOENT ? Status::NotFound : Status::IoError;
        if (fseek(f.get(),0,SEEK_END)) return Status::IoError;
        long length = ftell(f.get());
        if (length < 0) return Status::IoError;
        if ((size_t)length < kHeaderBytes || (size_t)length > kMaxFileBytes) return Status::Invalid;
        if (fseek(f.get(),0,SEEK_SET)) return Status::IoError;
        std::vector<u8> bytes((size_t)length);
        if (fread(bytes.data(),1,bytes.size(),f.get()) != bytes.size() || fgetc(f.get()) != EOF) return Status::IoError;
        if (std::memcmp(bytes.data(),kMagic,sizeof(kMagic))) return Status::Invalid;
        Decoder d{bytes,8};
        u32 version = d.word(), reserved = d.word();
        u64 compatibility = d.wide();
        u64 payload_size = d.wide(), checksum = d.wide();
        if (payload_size != bytes.size() - kHeaderBytes) return Status::Invalid;
        Encoder hash;
        for (size_t i = 0; i < 32; ++i) hash.byte(bytes[i]);
        for (size_t i = kHeaderBytes; i < bytes.size(); ++i) hash.byte(bytes[i]);
        if (hash.hash != checksum) return Status::Invalid;
        if (version != kVersion || compatibility != compatibility_tag) return Status::Incompatible;
        if (reserved) return Status::Invalid;
        Cache result;
        u32 programs = d.count(kMaxPrograms,36), pipelines = d.count(kMaxPipelines,12);
        result.programs.resize(programs);
        for (auto& p : result.programs) {
            p.fingerprint = d.wide(); p.key = d.wide();
            for (auto& s : p.stages) { s.present = d.flag(); if (s.present) s.program = read_program(d); }
            if (!d.ok) return Status::Invalid;
        }
        result.pipelines.resize(pipelines);
        for (auto& p : result.pipelines) { p.program_index = d.word(); p.program_fingerprint = d.wide(); read_key(d,p.key); }
        if (!d.ok || d.left() || !valid_cache(result)) return Status::Invalid;
        out = std::move(result);
        return Status::Ok;
    } catch (const std::exception&) { return Status::Invalid; }
}
}  // namespace gpu::warmup
