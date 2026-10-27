#include "gpu/vk/pipeline_catalog.h"
#include "gpu/vk/catalog_game_sources.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
extern "C" void hw_log(const char*,...) {}
extern "C" void hw_fatal(const char*) {std::abort();}
static std::string test_root;
const std::string& hw_romfs_path(){return test_root;}
namespace hwfs::nsp {
bool active(){return false;}
uint64_t romfs_size(){return 0;}
bool romfs_read(uint64_t,uint8_t*,uint64_t){return false;}
}
using namespace gpu;
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#ifndef _WIN32
class ScopedWriteLimit {
    struct rlimit previous_limit{};
    struct sigaction previous_action{};
    bool limited=false, ignored=false;
public:
    bool enable() {
        if(getrlimit(RLIMIT_FSIZE,&previous_limit))return false;
        struct sigaction action{};action.sa_handler=SIG_IGN;sigemptyset(&action.sa_mask);
        if(sigaction(SIGXFSZ,&action,&previous_action))return false;
        ignored=true;
        auto limit=previous_limit;limit.rlim_cur=1;
        if(setrlimit(RLIMIT_FSIZE,&limit))return false;
        limited=true;return true;
    }
    bool restore() {
        if(limited) {if(setrlimit(RLIMIT_FSIZE,&previous_limit))return false;limited=false;}
        if(ignored) {if(sigaction(SIGXFSZ,&previous_action,nullptr))return false;ignored=false;}
        return true;
    }
    ~ScopedWriteLimit() {
        if(!restore()){std::fputs("FAIL: restore file-size limit/signal action\n",stderr);std::abort();}
    }
};
#endif
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
static FILE* create_resource(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(),L"wb");
#else
    return fopen(path.c_str(),"wb");
#endif
}
int main() {
    // Public known-answer vectors ensure both host providers implement SHA-256.
    const catalog::Digest abc={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    const catalog::Digest empty={0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    CHECK(catalog::digest("abc",3)==abc);CHECK(catalog::digest("",0)==empty);
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
    CHECK(catalog::prepare(cat,[](const catalog::Locator&,std::vector<u8>&)->bool {throw std::runtime_error("unavailable source");},{}).pipelines.empty());
    auto changed=catalog::prepare(cat,[](const catalog::Locator& l,std::vector<u8>& b){b=source(l.offset==128?1:5);b[4]^=1;return true;},{});
    CHECK(changed.programs.empty());
    invalid=cat;invalid.programs[0].expected_translation^=1;CHECK(catalog::prepare(invalid,read,{}).pipelines.empty());
    auto short_source=source(1);short_source.resize(0x58);u32 size=0;catalog::Digest hash{};
    CHECK(!catalog::source_identity(short_source.data(),short_source.size(),1,size,hash));
    auto good=source(1);CHECK(!catalog::source_identity(good.data(),good.size(),5,size,hash));
    auto words=warmup::key_words(key);CHECK(words.size()==229);warmup::PipelineKey key2;CHECK(warmup::key_from_words(words,key2));CHECK(warmup::key_words(key2)==words);
    words.pop_back();CHECK(!warmup::key_from_words(words,key2));
#ifdef _WIN32
    const auto pid=GetCurrentProcessId();
#else
    const auto pid=getpid();
#endif
    auto directory=std::filesystem::temp_directory_path()/("hwder-catalog-test-"+std::to_string(pid));CHECK(std::filesystem::create_directory(directory));
    auto path=(directory/"catalog.bin").string();CHECK(catalog::save(path.c_str(),cat)==catalog::Status::Ok);
    CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::Ok);
    auto replacement=cat;replacement.scope="new-scope";
#ifdef _WIN32
    HANDLE locked=CreateFileA(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(locked!=INVALID_HANDLE_VALUE);
    CHECK(catalog::save(path.c_str(),replacement)==catalog::Status::IoError);CloseHandle(locked);
#else
    // POSIX permits replacement while readers retain the old inode.
    FILE* old=fopen(path.c_str(),"rb");CHECK(old!=nullptr);
    CHECK(catalog::save(path.c_str(),replacement)==catalog::Status::Ok);
    std::vector<u8> old_bytes(bytes.size());CHECK(fread(old_bytes.data(),1,old_bytes.size(),old)==old_bytes.size());fclose(old);
    CHECK(old_bytes==bytes);CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::Ok && round.scope==replacement.scope);
    CHECK(catalog::save(path.c_str(),cat)==catalog::Status::Ok);
    {
        // A partial sibling-temp write must preserve this catalog's old bytes.
        ScopedWriteLimit write_limit;CHECK(write_limit.enable());
        const auto failed=catalog::save(path.c_str(),replacement);
        CHECK(write_limit.restore());CHECK(failed==catalog::Status::IoError);
    }
    FILE* retained=fopen(path.c_str(),"rb");CHECK(retained!=nullptr);
    std::vector<u8> retained_bytes(bytes.size());
    const bool retained_ok=fread(retained_bytes.data(),1,retained_bytes.size(),retained)==retained_bytes.size() && fgetc(retained)==EOF;
    fclose(retained);CHECK(retained_ok && retained_bytes==bytes);
    CHECK(std::distance(std::filesystem::directory_iterator(directory),std::filesystem::directory_iterator{})==1);
#endif
    CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::Ok && round.scope==cat.scope);
    invalid=cat;invalid.scope="";CHECK(catalog::save(path.c_str(),invalid)==catalog::Status::Invalid);CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::Ok);
    std::filesystem::remove(directory/"catalog.bin");
    CHECK(catalog::load(path.c_str(),37,65536,round)==catalog::Status::NotFound);
    // Extracted-resource adapter uses the same raw NVN locator path as the NSP.
    test_root=directory.string();auto resource=directory/"data/stage/common/StageShader.bin.gz";
    std::filesystem::create_directories(resource.parent_path());
    // Deliberately byte-unaligned NVN header/SPH, as in the actual G2S archives.
    std::vector<u8> container(0x31);u32 nvn=0x12345678;std::memcpy(container.data()+1,&nvn,4);
    container.insert(container.end(),good.begin(),good.end());
    FILE* f=create_resource(resource);CHECK(f!=nullptr);CHECK(fwrite(container.data(),1,container.size(),f)==container.size());fclose(f);
    catalog::GameSources sources;CHECK(sources.index());CHECK(sources.count()==1);CHECK(sources.verify());
    catalog::Locator locator;CHECK(sources.locate(good.data(),good.size(),1,locator));CHECK(locator.offset==0x31);
    std::vector<u8> reread;CHECK(sources.read(locator,reread));CHECK(reread==good);
    auto escape=locator;escape.resource="../outside";CHECK(!sources.read(escape,reread));
    auto oversized=locator;oversized.offset=~0ull;CHECK(!sources.read(oversized,reread));
    f=create_resource(resource);CHECK(f!=nullptr);fputc(0,f);fclose(f);CHECK(!sources.read(locator,reread));
    std::filesystem::remove(resource);
#ifndef _WIN32
    // An allowed resource name must still reject a symlink outside the RomFS root.
    const auto outside=directory.parent_path()/(directory.filename().string()+"-outside");
    CHECK(std::filesystem::create_directory(outside));
    const auto external=outside/"synthetic.bin";
    f=create_resource(external);CHECK(f!=nullptr);CHECK(fwrite(container.data(),1,container.size(),f)==container.size());fclose(f);
    std::filesystem::create_symlink(external,resource);
    CHECK(!sources.read(locator,reread));CHECK(!sources.index());
    CHECK(std::filesystem::remove(resource));CHECK(std::filesystem::remove(external));CHECK(std::filesystem::remove(outside));
#endif
    std::filesystem::remove(directory/"data/stage/common");std::filesystem::remove(directory/"data/stage");std::filesystem::remove(directory/"data");std::filesystem::remove(directory);
    std::printf("pipeline catalog: %u checks passed; synthetic metadata only\n",checks);return 0;
}
