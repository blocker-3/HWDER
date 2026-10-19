#include "pipeline_catalog.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
// OpenSSL 3.x is provided by the Linux host (Apache-2.0); no sources are vendored.
#include <openssl/evp.h>
#endif

namespace gpu::catalog {
namespace {
constexpr u8 magic[8] = {'H','W','D','C','A','T','0','1'};
struct Codec {
    std::vector<u8> data;
    size_t pos = 0;
    bool reading = false;
    void byte(u8& v) {
        if (reading) { if (pos >= data.size()) throw std::runtime_error("truncated"); v = data[pos++]; }
        else { if (data.size() >= kMaxBytes) throw std::runtime_error("limit"); data.push_back(v); }
    }
    void word(u32& v) { u32 n = 0; for (int i=0;i<4;i++) { u8 b=u8(v>>(8*i)); byte(b); n|=u32(b)<<(8*i); } if(reading)v=n; }
    void wide(u64& v) { u32 lo=u32(v), hi=u32(v>>32); word(lo);word(hi);if(reading)v=u64(lo)|(u64(hi)<<32); }
    void flag(bool& v) { u32 n=v;word(n);if(n>1)throw std::runtime_error("flag");v=n!=0; }
    void string(std::string& s, u32 limit) {
        u32 n=u32(s.size());word(n);if(n>limit || (reading && n>data.size()-pos))throw std::runtime_error("string");
        if(reading)s.resize(n);for(char& ch:s){u8 b=u8(ch);byte(b);ch=char(b);}
    }
};
void stage_fields(Codec& c, Stage& s) {
    c.flag(s.present); if(!s.present)return;
    c.string(s.source.resource,256);c.wide(s.source.offset);c.word(s.source.size);
    for(u8& b:s.source.sha256)c.byte(b);
    c.wide(s.attributes);c.word(s.render_targets);c.word(s.bound_buffer);
    c.flag(s.options.flip_y);c.flag(s.options.convert_depth_mode);c.flag(s.options.picture_controls);
    c.word(s.options.bloom_cbuf);c.word(s.options.bloom_offset);
}
bool valid(const Catalog& cat) {
    if(cat.title!=kTitleId || cat.scope.empty() || cat.scope.size()>128 ||
       cat.programs.size()>warmup::kMaxPrograms || cat.pipelines.size()>warmup::kMaxPipelines)return false;
    for(unsigned char ch:cat.scope)if(ch<32 || ch==127)return false;
    for(const auto& p:cat.programs) {
        if(!p.stages[0].present)return false;
        for(const auto& s:p.stages)if(s.present) {
            const auto& l=s.source;
            if(!safe_resource(l.resource) || l.size<0x60 || l.size>kMaxSourceBytes || (l.size&7) ||
               l.offset>0x40000000ull || l.sha256==Digest{} || s.bound_buffer>=18 ||
               s.options.bloom_cbuf>=18 || s.options.bloom_offset>0xfffc || (s.options.bloom_offset&3))return false;
            for(u32 i=0;i<32;i++)if(((s.attributes>>(2*i))&3)>2)return false;
            if(s.render_targets>>16)return false;
            for(u32 i=0;i<8;i++)if(((s.render_targets>>(2*i))&3)>2)return false;
        }
    }
    for(const auto& p:cat.pipelines) {
        warmup::PipelineKey key;
        if(p.program>=cat.programs.size() || p.key.program!=cat.programs[p.program].key ||
           !warmup::key_from_words(warmup::key_words(p.key),key))return false;
    }
    return true;
}
void contents(Codec& c, Catalog& cat) {
    c.wide(cat.build);c.wide(cat.title);c.word(cat.game_version);c.string(cat.scope,128);
    u32 np=u32(cat.programs.size()), nk=u32(cat.pipelines.size());c.word(np);c.word(nk);
    if(np>warmup::kMaxPrograms || nk>warmup::kMaxPipelines)throw std::runtime_error("count");
    if(c.reading) {
        if(u64(np)*36+u64(nk)*920>c.data.size()-c.pos)throw std::runtime_error("count bytes");
        cat.programs.resize(np);cat.pipelines.resize(nk);
    }
    for(auto& p:cat.programs){c.wide(p.key);c.wide(p.expected_translation);for(auto& s:p.stages)stage_fields(c,s);}
    for(auto& p:cat.pipelines) {
        c.word(p.program);auto words=warmup::key_words(p.key);
        for(auto& w:words)c.word(w);
        if(c.reading && !warmup::key_from_words(words,p.key))throw std::runtime_error("key");
    }
}
class LocalEnv : public shader::Environment {
public:
    const Stage& stage;
    const std::vector<u8>& bytes;
    bool safe = true;
    LocalEnv(const Stage& s,const std::vector<u8>& b):stage(s),bytes(b){}
    u64 read_instruction(u32 address) override {
        const u64 off=0x50ull+address;
        if(off>bytes.size() || bytes.size()-size_t(off)<8){safe=false;return 0;}
        u64 word;std::memcpy(&word,bytes.data()+off,8);return word;
    }
    const u8* sph() override{return bytes.data();}
    // Current graphics translator does not query these. Refuse future functional
    // context we have not captured rather than guessing handles or texture types.
    u32 read_cbuf(u32,u32) override{safe=false;return 0;}
    shader::TextureType texture_type(u32) override{safe=false;return shader::TextureType::Tex2D;}
    u32 texture_bound_buffer() override{return stage.bound_buffer;}
    shader::AttributeType attribute_type(u32 i) override{return i<32?shader::AttributeType((stage.attributes>>(2*i))&3):shader::AttributeType::Float;}
    shader::AttributeType render_target_type(u32 i) override{return i<8?shader::AttributeType((stage.render_targets>>(2*i))&3):shader::AttributeType::Float;}
};
} // namespace

Digest digest(const void* bytes,size_t size) {
    Digest result{};
    if(size>0xffffffffull)throw std::runtime_error("SHA256 length");
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider");
    auto status=BCryptHash(alg,nullptr,0,(PUCHAR)bytes,ULONG(size),result.data(),ULONG(result.size()));
    BCryptCloseAlgorithmProvider(alg,0);
    if(status<0)throw std::runtime_error("SHA256");
#else
    unsigned int length=0;
    if(EVP_Digest(bytes,size,result.data(),&length,EVP_sha256(),nullptr)!=1 || length!=result.size())
        throw std::runtime_error("SHA256");
#endif
    return result;
}
bool safe_resource(const std::string& p) {
    if(p.empty() || p.size()>256 || p.front()=='/' || p.back()=='/')return false;
    size_t start=0;
    for(size_t i=0;i<=p.size();i++) {
        if(i<p.size() && (u8(p[i])<32 || p[i]=='\\' || p[i]==':' || u8(p[i])==127))return false;
        if(i==p.size() || p[i]=='/'){auto part=p.substr(start,i-start);if(part.empty() || part=="." || part=="..")return false;start=i+1;}
    }
    return true;
}
bool source_identity(const u8* p,size_t avail,u32 stage,u32& size,Digest& sha) {
    if(!p || avail<0x60 || stage<1 || stage>5)return false;
    u32 w;std::memcpy(&w,p,4);
    if(((w>>5)&31)!=3 || ((w>>10)&15)!=stage || (w&31)!=(stage==5?2u:1u))return false;
    const size_t limit=std::min(avail,size_t(kMaxSourceBytes));
    for(u32 a=0;0x50ull+a+8<=limit;a+=8) {
        u64 ins;std::memcpy(&ins,p+0x50+a,8);
        if((a/8)%4 && (ins>>52)==0xe24 && ((ins>>20)&0xffffff)>=0xffffe0 && (ins&31)==15) {
            size=0x50+a+8;sha=digest(p,size);return true;
        }
    }
    return false;
}
Status encode(const Catalog& value,std::vector<u8>& out) {
    try {
        if(!valid(value))return Status::Invalid;
        Catalog copy=value;Codec c;for(u8 b:magic)c.byte(b);u32 version=1;c.word(version);contents(c,copy);
        auto hash=digest(c.data.data(),c.data.size());for(auto b:hash)c.byte(b);out=std::move(c.data);return Status::Ok;
    }catch(const std::exception&){return Status::Invalid;}
}
Status decode(const std::vector<u8>& bytes,u64 build,u32 version,Catalog& out) {
    try {
        if(bytes.size()<76 || bytes.size()>kMaxBytes || std::memcmp(bytes.data(),magic,8))return Status::Invalid;
        auto hash=digest(bytes.data(),bytes.size()-32);
        if(std::memcmp(hash.data(),bytes.data()+bytes.size()-32,32))return Status::Invalid;
        Codec c;c.reading=true;c.data.assign(bytes.begin(),bytes.end()-32);c.pos=8;
        u32 schema=0;c.word(schema);if(schema!=1)return Status::Incompatible;
        Catalog result;contents(c,result);
        if(c.pos!=c.data.size() || !valid(result))return Status::Invalid;
        if(result.build!=build || result.game_version!=version)return Status::Incompatible;
        out=std::move(result);return Status::Ok;
    }catch(const std::exception&){return Status::Invalid;}
}
Status load(const char* path,u64 build,u32 version,Catalog& out) {
    if(!path || !*path)return Status::IoError;
    FILE* f=nullptr;
#ifdef _WIN32
    auto error=fopen_s(&f,path,"rb");
#else
    f=fopen(path,"rb");const int error=errno;
#endif
    if(!f)return error==ENOENT?Status::NotFound:Status::IoError;
    std::unique_ptr<FILE,decltype(&fclose)> owned(f,fclose);
    if(fseek(f,0,SEEK_END))return Status::IoError;long n=ftell(f);
    if(n<0 || size_t(n)>kMaxBytes)return Status::Invalid;
    rewind(f);std::vector<u8> data(n);if(fread(data.data(),1,data.size(),f)!=data.size())return Status::IoError;
    return decode(data,build,version,out);
}
Status save(const char* path,const Catalog& value){std::vector<u8> bytes;auto status=encode(value,bytes);return status==Status::Ok?warmup::save_metadata_atomic(path,bytes):status;}
warmup::Cache prepare(const Catalog& cat,const ReadSource& read,const std::function<void(const Progress&)>& update) {
    warmup::Cache cache;Progress progress;progress.total=u32(cat.programs.size());
    if(!valid(cat))return cache;
    std::vector<u32> remap(cat.programs.size(),~0u);
    for(u32 i=0;i<cat.programs.size();i++) {
        const auto& p=cat.programs[i];warmup::ProgramRecord record;record.key=p.key;bool ok=true;
        try {
        for(u32 j=0;j<5 && ok;j++)if(p.stages[j].present) {
            const auto& s=p.stages[j];std::vector<u8> bytes;
            u32 length=0;Digest hash{};
            if(!read(s.source,bytes) || bytes.size()!=s.source.size || !source_identity(bytes.data(),bytes.size(),j+1,length,hash) ||
               length!=s.source.size || hash!=s.source.sha256){ok=false;break;}
            LocalEnv env(s,bytes);record.stages[j].present=true;
            record.stages[j].program=shader::translate(env,shader::Stage(j+1),s.options);
            if(!env.safe || record.stages[j].program.spirv.empty())ok=false;
        }
        } catch(const std::exception&) { ok=false; }
        record.fingerprint=warmup::fingerprint(record);
        if(ok && record.fingerprint==p.expected_translation){remap[i]=u32(cache.programs.size());cache.programs.push_back(std::move(record));++progress.resolved;}
        else ++progress.unresolved;
        ++progress.done;if(update)update(progress);
    }
    for(const auto& p:cat.pipelines)if(remap[p.program]!=~0u) {
        warmup::PipelineRecord record;record.key=p.key;record.program_index=remap[p.program];
        record.program_fingerprint=cache.programs[record.program_index].fingerprint;cache.pipelines.push_back(record);
    }
    return cache;
}
} // namespace gpu::catalog
