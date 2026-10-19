#include "gpu/vk/pipeline_warmup_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <windows.h>

using namespace gpu::warmup;
namespace {
constexpr u64 kTag = 0x1111222233334444ull;
unsigned checks = 0;
void check(bool good, const char* what) {
    ++checks;
    if (!good) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
std::vector<u8> read(const std::string& path) {
    std::ifstream f(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
void write(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream f(path,std::ios::binary | std::ios::trunc);
    f.write((const char*)bytes.data(),bytes.size());
    check(bool(f),"write corruption fixture");
}
void word(std::vector<u8>& b, size_t at, u32 v) {
    check(at+4<=b.size(),"fixture word bounds");
    for (int i=0;i<4;++i) b[at+i]=(u8)(v>>(i*8));
}
void wide(std::vector<u8>& b, size_t at, u64 v) { word(b,at,(u32)v); word(b,at+4,(u32)(v>>32)); }
void repair(std::vector<u8>& b) {
    wide(b,24,b.size()-40);
    u64 hash=14695981039346656037ull;
    for (size_t i=0;i<b.size();++i) if (i<32 || i>=40) hash=(hash^b[i])*1099511628211ull;
    wide(b,32,hash);
}
shader::Program program(shader::Stage stage) {
    shader::Program p{};
    p.stage=stage;
    u32 model=(u32)stage-1;
    p.spirv={0x07230203,0x00010300,123,16,0,
             0x00020011,1, 0x0003000e,0,1, 0x0005000f,model,1,0x6e69616d,0,
             0x00020013,2, 0x00030021,3,2, 0x00050036,2,1,0,3,
             0x000200f8,4, 0x000100fd,0x00010038};
    p.resources.const_buffers={{0,16},{17,65536}};
    p.resources.storage_buffers={{16,0x20,true}};
    p.resources.texel_buffers={{shader::TextureType::Buffer,false,false,2,0x24,false,0,0,4}};
    p.resources.textures={{shader::TextureType::CubeArray,true,true,3,0x30,true,4,0x40,3}};
    p.resources.images={{shader::TextureType::Tex3D,5,0x44,true,true,123}};
    p.attributes_read=0x80000005; p.outputs_written=0x52; p.writes_depth=true; p.uses_discard=true;
    p.local_size[0]=4; p.local_size[1]=2; p.local_size[2]=1;
    p.shared_memory_size=4096; p.local_memory_size=128;
    p.geometry_output_topology=28; p.geometry_max_vertices=48; p.geometry_invocations=2;
    return p;
}
Cache fixture() {
    Cache c;
    ProgramRecord p;
    p.key=0x1020304050607080ull;
    for (u32 i=0;i<5;++i) { p.stages[i].present=true; p.stages[i].program=program((shader::Stage)(i+1)); }
    p.fingerprint=fingerprint(p); c.programs.push_back(p);
    PipelineRecord r;
    std::memset(&r.key,0,sizeof(r.key));
    r.program_fingerprint=p.fingerprint;
    auto& k=r.key;
    k.program=p.key; k.depth_format=130; k.num_colors=8; k.topology=10; k.patch_points=7;
    for(u32 i=0;i<8;++i) {
        k.color_formats[i]=37+i; k.blend_enable[i]=i%2; k.color_mask[i]=i+1;
        for(u32 j=0;j<6;++j) k.blend[i][j]=(j==0 || j==3) ? i%5 : i+2;
    }
    for(u32 i=0;i<32;++i) { k.attrib_format[i]=37+i; k.attrib_offset[i]=i*4; k.attrib_binding[i]=i; k.binding_stride[i]=32+i; }
    k.cull_enable=1; k.cull_mode=3; k.front_face=1; k.polygon_mode=2;
    k.depth_bias=1; k.discard=1; k.depth_clamp=1;
    k.depth_test=1; k.depth_write=1; k.depth_func=7; k.depth_bounds=1;
    k.stencil_enable=1;
    for(u32 i=0;i<4;++i) { k.front[i]=i; k.back[i]=7-i; }
    k.logic_enable=1; k.logic=15; k.prim_restart=1;
    c.pipelines.push_back(r);
    r.key.topology=3; c.pipelines.push_back(r);
    return c;
}
void unchanged_on_failure(const std::string& path, u64 tag, Status expected) {
    Cache sentinel=fixture(); sentinel.programs[0].key=55;
    check(load(path.c_str(),tag,sentinel)==expected,"expected load rejection");
    check(sentinel.programs.size()==1 && sentinel.programs[0].key==55 && sentinel.pipelines.size()==2,"rejected load leaves caller unchanged");
}
}  // namespace

int main() {
    auto dir=std::filesystem::temp_directory_path()/("hwder-warmup-test-"+std::to_string(GetCurrentProcessId()));
    check(std::filesystem::create_directory(dir),"create private test directory");
    std::string path=(dir/"cache.bin").string(), raw=(dir/"raw.bin").string(), second=(dir/"second.bin").string();
    Cache original=fixture(), loaded;
    check(load(path.c_str(),kTag,loaded)==Status::NotFound,"missing cache");
    auto saved=save(path.c_str(),kTag,original);
    if(saved!=Status::Ok) std::fprintf(stderr,"nontrivial save status: %s (Win32 %lu)\n",name(saved),GetLastError());
    check(saved==Status::Ok,"nontrivial save");
    check(load(path.c_str(),kTag,loaded)==Status::Ok,"nontrivial load");
    check(loaded.programs.size()==1 && loaded.pipelines.size()==2,"record counts and shared program reference");
    check(equal(original.programs[0],loaded.programs[0]),"every Program and resource field roundtrips");
    for(size_t i=0;i<2;++i) {
        check(!std::memcmp(&original.pipelines[i].key,&loaded.pipelines[i].key,sizeof(PipelineKey)),"all key fields and zeroed padding roundtrip");
        check(loaded.pipelines[i].program_index==0 && loaded.pipelines[i].program_fingerprint==loaded.programs[0].fingerprint,"program references roundtrip");
    }
    check(save(second.c_str(),kTag,loaded)==Status::Ok && read(path)==read(second),"deterministic canonical encoding");
    auto good=read(path);
    unchanged_on_failure(path,kTag+1,Status::Incompatible);
    // Each changed semantic field must defeat exact equality, even a forged matching fingerprint.
    auto different=[&](auto change) {
        auto modified=original.programs[0]; change(modified.stages[0].program);
        check(!equal(original.programs[0].stages[0].program,modified.stages[0].program),"exact stage comparison rejects metadata change");
        check(fingerprint(modified)!=original.programs[0].fingerprint,"semantic change affects deterministic fingerprint");
        modified.fingerprint=original.programs[0].fingerprint;
        check(!equal(original.programs[0],modified),"matching stored fingerprint cannot replace exact comparison");
    };
#define CHANGED(field, value) different([](shader::Program& p){p.field=value;})
    CHANGED(stage,shader::Stage::Fragment); CHANGED(spirv[2],321);
    CHANGED(attributes_read,1); CHANGED(outputs_written,2); CHANGED(writes_depth,false); CHANGED(uses_discard,false);
    CHANGED(local_size[0],1); CHANGED(local_size[1],1); CHANGED(local_size[2],2);
    CHANGED(shared_memory_size,8); CHANGED(local_memory_size,16); CHANGED(geometry_output_topology,27);
    CHANGED(geometry_max_vertices,4); CHANGED(geometry_invocations,1);
    CHANGED(resources.const_buffers[0].index,1); CHANGED(resources.const_buffers[0].size,32);
    CHANGED(resources.storage_buffers[0].cbuf_index,1); CHANGED(resources.storage_buffers[0].cbuf_offset,0x28); CHANGED(resources.storage_buffers[0].written,false);
    CHANGED(resources.textures[0].type,shader::TextureType::Tex2D); CHANGED(resources.textures[0].is_depth,false);
    CHANGED(resources.textures[0].is_multisample,false); CHANGED(resources.textures[0].cbuf_index,1); CHANGED(resources.textures[0].cbuf_offset,0x38);
    CHANGED(resources.textures[0].has_secondary,false); CHANGED(resources.textures[0].secondary_cbuf_index,1); CHANGED(resources.textures[0].secondary_cbuf_offset,0x48); CHANGED(resources.textures[0].count,2);
    CHANGED(resources.texel_buffers[0].type,shader::TextureType::Tex1D); CHANGED(resources.texel_buffers[0].is_depth,true);
    CHANGED(resources.texel_buffers[0].is_multisample,true); CHANGED(resources.texel_buffers[0].cbuf_index,1); CHANGED(resources.texel_buffers[0].cbuf_offset,0x28);
    CHANGED(resources.texel_buffers[0].has_secondary,true); CHANGED(resources.texel_buffers[0].secondary_cbuf_index,1); CHANGED(resources.texel_buffers[0].secondary_cbuf_offset,0x48); CHANGED(resources.texel_buffers[0].count,2);
    CHANGED(resources.images[0].type,shader::TextureType::Tex1D); CHANGED(resources.images[0].cbuf_index,1); CHANGED(resources.images[0].cbuf_offset,0x48);
    CHANGED(resources.images[0].is_written,false); CHANGED(resources.images[0].is_read,false); CHANGED(resources.images[0].format_hint,124);
#undef CHANGED
    different([](auto& p){p.resources.const_buffers.pop_back();});
    different([](auto& p){p.resources.storage_buffers.clear();});
    different([](auto& p){p.resources.textures.clear();});
    different([](auto& p){p.resources.texel_buffers.clear();});
    different([](auto& p){p.resources.images.clear();});
    auto absent=original.programs[0]; absent.stages[4].present=false;
    check(!equal(absent,original.programs[0]),"stage presence participates in exact identity");
    auto no_payload=absent; no_payload.stages[4].program.spirv.clear();
    check(equal(absent,no_payload) && fingerprint(absent)==fingerprint(no_payload),"absent payload is irrelevant");
    auto same_key=original;
    auto alternate=original.programs[0]; alternate.stages[0].program.resources.textures[0].count=2;
    alternate.fingerprint=fingerprint(alternate); same_key.programs.push_back(alternate);
    same_key.pipelines[1].program_index=1; same_key.pipelines[1].program_fingerprint=alternate.fingerprint;
    Cache variants;
    check(save(second.c_str(),kTag,same_key)==Status::Ok && load(second.c_str(),kTag,variants)==Status::Ok,"same-key distinct program variants roundtrip by indexed identity");
    check(variants.programs.size()==2 && !equal(variants.programs[0],variants.programs[1]),"codec never merges distinct metadata by key");
    same_key.programs[1].fingerprint=same_key.programs[0].fingerprint;
    check(save(second.c_str(),kTag,same_key)==Status::Invalid,"forged stored fingerprint rejected on save");
    // Corrupt bytes with and without repaired integrity to exercise the parser, not only checksum handling.
    auto reject=[&](std::vector<u8> bytes,Status status=Status::Invalid,bool fix=true) {
        if(fix) repair(bytes); write(raw,bytes); unchanged_on_failure(raw,kTag,status);
    };
    auto corrupt=good; corrupt.back()^=1; reject(corrupt,Status::Invalid,false);
    corrupt=good; corrupt[16]^=1; reject(corrupt,Status::Invalid,false);
    corrupt=good; word(corrupt,8,99); reject(corrupt,Status::Incompatible);
    corrupt=good; word(corrupt,12,1); reject(corrupt);
    corrupt=good; word(corrupt,40,kMaxPrograms+1); reject(corrupt);
    corrupt=good; word(corrupt,44,kMaxPipelines+1); reject(corrupt);
    corrupt=good; word(corrupt,64,2); reject(corrupt); // noncanonical stage-present boolean
    corrupt=good; word(corrupt,72,kMaxSpirvWords+1); reject(corrupt);
    corrupt=good; corrupt[48]^=1; reject(corrupt); // stored program fingerprint
    // Last recipe: explicit on-disk key has 916 bytes, independent of its C++ tail padding.
    constexpr size_t key_bytes=916;
    size_t recipe=good.size()-(12+key_bytes), key=recipe+12;
    corrupt=good; word(corrupt,recipe,1); reject(corrupt); // invalid program index
    corrupt=good; corrupt[recipe+4]^=1; reject(corrupt); // recipe fingerprint
    corrupt=good; word(corrupt,key,99); reject(corrupt); // key.program differs from record
    corrupt=good; word(corrupt,key+44,9); reject(corrupt); // num_colors
    corrupt=good; word(corrupt,key+48,11); reject(corrupt); // topology enum
    corrupt=good; word(corrupt,key+312,32); reject(corrupt); // shift/index safety
    corrupt=good; word(corrupt,key+568,2); reject(corrupt); // normalized key boolean
    corrupt=good; corrupt.push_back(0); reject(corrupt); // trailing payload despite valid integrity
    corrupt=good; corrupt[0]^=1; reject(corrupt); // magic
    for(size_t cut:{size_t(0),size_t(7),size_t(39),size_t(40),size_t(47),size_t(75),good.size()-1}) {
        corrupt.assign(good.begin(),good.begin()+cut);
        reject(corrupt,Status::Invalid,cut>=40);
    }
    // Recompute fingerprints as well as integrity, to ensure malformed stage/SPIR-V rejection is independent.
    Cache minimal; minimal.programs.push_back(original.programs[0]);
    for(size_t i=1;i<5;++i) minimal.programs[0].stages[i].present=false;
    minimal.programs[0].fingerprint=fingerprint(minimal.programs[0]);
    check(save(second.c_str(),kTag,minimal)==Status::Ok,"minimal vertex-only cache");
    auto minimal_bytes=read(second);
    auto bad_stage=minimal.programs[0]; bad_stage.stages[0].program.stage=shader::Stage::Fragment;
    corrupt=minimal_bytes; word(corrupt,68,(u32)shader::Stage::Fragment); wide(corrupt,48,fingerprint(bad_stage)); reject(corrupt);
    auto bad_spirv=minimal.programs[0]; bad_spirv.stages[0].program.spirv[5]=17;
    corrupt=minimal_bytes; word(corrupt,96,17); wide(corrupt,48,fingerprint(bad_spirv)); reject(corrupt);
    bad_spirv=minimal.programs[0]; bad_spirv.stages[0].program.spirv[0]=0;
    corrupt=minimal_bytes; word(corrupt,76,0); wide(corrupt,48,fingerprint(bad_spirv)); reject(corrupt);
    size_t const_count=76+minimal.programs[0].stages[0].program.spirv.size()*4;
    size_t storage_count=const_count+4+2*8, texel_count=storage_count+4+12;
    size_t texture_count=texel_count+4+36;
    corrupt=minimal_bytes; word(corrupt,const_count,kMaxResources+1); reject(corrupt);
    auto bad_resource=minimal.programs[0]; bad_resource.stages[0].program.resources.textures[0].type=(shader::TextureType)99;
    corrupt=minimal_bytes; word(corrupt,texture_count+4,99); wide(corrupt,48,fingerprint(bad_resource)); reject(corrupt);
    bad_resource=minimal.programs[0]; bad_resource.stages[0].program.resources.const_buffers[0].index=18;
    corrupt=minimal_bytes; word(corrupt,const_count+4,18); wide(corrupt,48,fingerprint(bad_resource)); reject(corrupt);
    bad_resource=minimal.programs[0]; bad_resource.stages[0].program.resources.storage_buffers[0].cbuf_offset=0xfffffff0;
    corrupt=minimal_bytes; word(corrupt,storage_count+8,0xfffffff0); wide(corrupt,48,fingerprint(bad_resource)); reject(corrupt);
    bad_resource=minimal.programs[0]; bad_resource.stages[0].program.resources.textures[0].count=0xffffffff;
    corrupt=minimal_bytes; word(corrupt,texture_count+36,0xffffffff); wide(corrupt,48,fingerprint(bad_resource)); reject(corrupt);
    auto invalid=original; invalid.programs[0].stages[0].program.resources.textures[0].type=(shader::TextureType)99;
    invalid.programs[0].fingerprint=fingerprint(invalid.programs[0]);
    check(save(path.c_str(),kTag,invalid)==Status::Invalid && read(path)==good,"invalid enum save preserves old file");
    invalid=original; invalid.programs[0].stages[0].program.resources.textures[0].count=0xffffffff;
    check(save(path.c_str(),kTag,invalid)==Status::Invalid,"descriptor count overflow rejected");
    invalid=original; invalid.programs[0].stages[0].program.resources.storage_buffers[0].cbuf_offset=0xfffffff0;
    check(save(path.c_str(),kTag,invalid)==Status::Invalid,"resource offset overflow rejected");
    invalid=original; invalid.programs.resize(kMaxPrograms+1);
    check(save(path.c_str(),kTag,invalid)==Status::Invalid,"oversized in-memory program count rejected");
    {
        std::ofstream sparse(raw,std::ios::binary|std::ios::trunc); sparse.seekp(kMaxFileBytes); sparse.put(0);
    }
    unchanged_on_failure(raw,kTag,Status::Invalid);
    HANDLE locked=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(locked!=INVALID_HANDLE_VALUE,"lock old cache against replacement");
    check(save(path.c_str(),kTag,minimal)==Status::IoError,"atomic replacement failure reported");
    CloseHandle(locked);
    check(read(path)==good,"failed atomic replacement retains previous bytes");
    size_t files=0; for(const auto& ignored:std::filesystem::directory_iterator(dir)) ++files;
    check(files==3,"failed save leaves no temporary artifact");
    check(save(path.c_str(),kTag,minimal)==Status::Ok,"atomic replacement succeeds after releasing old file");
    Cache replaced; check(load(path.c_str(),kTag,replaced)==Status::Ok && replaced.pipelines.empty(),"replacement contains new cache");
    Cache empty; check(save(second.c_str(),kTag,empty)==Status::Ok && load(second.c_str(),kTag,replaced)==Status::Ok && replaced.programs.empty(),"empty cache roundtrips");
    std::filesystem::remove(path); std::filesystem::remove(raw); std::filesystem::remove(second); std::filesystem::remove(dir);
    std::printf("pipeline_warmup_cache_test: %u checks passed\n",checks);
}
