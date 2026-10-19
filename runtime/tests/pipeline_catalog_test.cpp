#include "gpu/vk/pipeline_catalog.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <windows.h>
extern "C" void hw_log(const char*,...) {}
extern "C" void hw_fatal(const char*) {std::abort();}
using namespace gpu;
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
// Entirely synthetic minimal graphics programs, generated in memory. No game fixture.
static std::vector<u8> source(u32 stage) {
    std::vector<u8> b(0x60);u32 header=(stage<<10)|(3<<5)|(stage==5?2:1);
    std::memcpy(b.data(),&header,4);u64 end=0xe2400fffff87000full;
    std::memcpy(b.data()+0x58,&end,8);return b;
}
struct Env:shader::Environment {
    std::vector<u8> b;
    explicit Env(u32 stage):b(source(stage)){}
    u64 read_instruction(u32 a)override{u64 w=0;if(0x50ull+a+8<=b.size())std::memcpy(&w,b.data()+0x50+a,8);return w;}
    const u8* sph()override{return b.data();}
    u32 read_cbuf(u32,u32)override{return 0;}
    shader::TextureType texture_type(u32)override{return shader::TextureType::Tex2D;}
    u32 texture_bound_buffer()override{return 2;}
};
static void reseal(std::vector<u8>& bytes){auto h=catalog::digest(bytes.data(),bytes.size()-32);std::copy(h.begin(),h.end(),bytes.end()-32);}
int main() {
    catalog::Catalog cat;cat.build=37;cat.game_version=65536;cat.scope="synthetic-stage";
    catalog::Program program;program.key=0x123456789abcdef0ull;
    warmup::ProgramRecord translated;translated.key=program.key;
    for(u32 slot:{0u,4u}) {
        auto& s=program.stages[slot];s.present=true;s.source.resource="data/stage/common/StageShader.bin.gz";s.source.offset=128+slot*64;
        Env env(slot+1);CHECK(catalog::source_identity(env.b.data(),env.b.size(),slot+1,s.source.size,s.source.sha256));
        translated.stages[slot].present=true;translated.stages[slot].program=shader::translate(env,shader::Stage(slot+1),s.options);
    }
    program.expected_translation=warmup::fingerprint(translated);cat.programs.push_back(program);
    warmup::PipelineKey key{};key.program=program.key;cat.pipelines.push_back({0,key});
    std::vector<u8> bytes;CHECK(catalog::encode(cat,bytes)==catalog::Status::Ok);
    CHECK(bytes.size()<1600);CHECK(std::memcmp(bytes.data(),"HWDCAT01",8)==0);
    // The metadata format serializes only named settings; it has no opaque byte/blob field.
    for(u32 st:{1u,5u}) {auto b=source(st);CHECK(std::search(bytes.begin(),bytes.end(),b.begin(),b.end())==bytes.end());}
    const u8 spv_magic[]={3,2,35,7};CHECK(std::search(bytes.begin(),bytes.end(),std::begin(spv_magic),std::end(spv_magic))==bytes.end());
    catalog::Catalog round;CHECK(catalog::decode(bytes,cat.build,cat.game_version,round)==catalog::Status::Ok);
    std::vector<u8> again;CHECK(catalog::encode(round,again)==catalog::Status::Ok);CHECK(again==bytes);
    CHECK(catalog::decode(bytes,38,cat.game_version,round)==catalog::Status::Incompatible);
    CHECK(catalog::decode(bytes,37,1,round)==catalog::Status::Incompatible);
    for(size_t n=0;n<bytes.size();n+=11){auto b=bytes;b.resize(n);CHECK(catalog::decode(b,37,65536,round)==catalog::Status::Invalid);}
    for(size_t i=0;i<bytes.size();i+=13){auto b=bytes;b[i]^=1;CHECK(catalog::decode(b,37,65536,round)==catalog::Status::Invalid);}
    auto trailing=bytes;trailing.push_back(0);CHECK(catalog::decode(trailing,37,65536,round)==catalog::Status::Invalid);
    auto schema=bytes;schema[8]=2;reseal(schema);CHECK(catalog::decode(schema,37,65536,round)==catalog::Status::Incompatible);
    auto count=bytes;size_t count_offset=36+cat.scope.size();
    for(size_t i=0;i<4;i++)count[count_offset+i]=255;
    reseal(count);CHECK(catalog::decode(count,37,65536,round)==catalog::Status::Invalid);
    auto bad_key=bytes;size_t key_offset=bad_key.size()-32-916;
    bad_key[key_offset+78*4]=32;reseal(bad_key);
    CHECK(catalog::decode(bad_key,37,65536,round)==catalog::Status::Invalid);
    auto invalid=cat;invalid.programs[0].stages[0].source.resource="../outside";CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    for(const char* p:{"", "/absolute", "C:/file", "data/../file", "data//file", "data\\file", "data/./file"})CHECK(!catalog::safe_resource(p));
    CHECK(catalog::safe_resource("data/stage/common/StageShader.bin.gz"));
    invalid=cat;invalid.pipelines[0].program=1;CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    invalid=cat;invalid.pipelines[0].key.attrib_binding[0]=32;CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    invalid=cat;invalid.programs[0].stages[0].attributes=3;CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    invalid=cat;invalid.programs[0].stages[0].source.size=catalog::kMaxSourceBytes+8;CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    invalid=cat;invalid.programs[0].stages[0].source.offset=~0ull;CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    invalid=cat;invalid.title=1;CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    invalid=cat;invalid.programs.resize(warmup::kMaxPrograms+1);CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    invalid=cat;invalid.pipelines.resize(warmup::kMaxPipelines+1);CHECK(catalog::encode(invalid,again)==catalog::Status::Invalid);
    auto read=[](const catalog::Locator& l,std::vector<u8>& out){out=source(l.offset==128?1:5);return true;};
    catalog::Progress progress;
    auto prepared=catalog::prepare(cat,read,[&](const catalog::Progress& p){progress=p;});
    CHECK(prepared.pipelines.size()==1);CHECK(progress.done==1 && progress.resolved==1 && !progress.unresolved);
    CHECK(warmup::equal(prepared.programs[0],translated));
    auto unresolved=catalog::prepare(cat,[](const catalog::Locator&,std::vector<u8>&){return false;},[&](auto p){progress=p;});
    CHECK(unresolved.programs.empty() && unresolved.pipelines.empty());CHECK(progress.unresolved==1);
    auto changed=catalog::prepare(cat,[](const catalog::Locator& l,std::vector<u8>& b){b=source(l.offset==128?1:5);b[4]^=1;return true;},{});
    CHECK(changed.programs.empty());
    invalid=cat;invalid.programs[0].expected_translation^=1;CHECK(catalog::prepare(invalid,read,{}).pipelines.empty());
    auto short_source=source(1);short_source.resize(0x58);u32 size=0;catalog::Digest hash{};
    CHECK(!catalog::source_identity(short_source.data(),short_source.size(),1,size,hash));
    auto good=source(1);CHECK(!catalog::source_identity(good.data(),good.size(),5,size,hash));
    auto words=warmup::key_words(key);CHECK(words.size()==229);warmup::PipelineKey key2;CHECK(warmup::key_from_words(words,key2));CHECK(warmup::key_words(key2)==words);
    words.pop_back();CHECK(!warmup::key_from_words(words,key2));
    auto directory=std::filesystem::temp_directory_path()/("hwder-catalog-test-"+std::to_string(GetCurrentProcessId()));std::filesystem::create_directories(directory);
    auto path=(directory/"catalog.bin").string();CHECK(catalog::save(path.c_str(),cat)==catalog::Status::Ok);
    CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::Ok);
    HANDLE locked=CreateFileA(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(locked!=INVALID_HANDLE_VALUE);
    auto replacement=cat;replacement.scope="new-scope";
    CHECK(catalog::save(path.c_str(),replacement)==catalog::Status::IoError);CloseHandle(locked);
    CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::Ok && round.scope==cat.scope);
    invalid=cat;invalid.scope="";CHECK(catalog::save(path.c_str(),invalid)==catalog::Status::Invalid);CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::Ok);
    std::filesystem::remove(directory/"catalog.bin");std::filesystem::remove(directory);
    CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::NotFound);
    std::printf("pipeline catalog: %u checks passed; synthetic metadata only\n",checks);return 0;
}
