#include "catalog_game_sources.h"
#include "fs/nsp.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <cstdio>

namespace gpu::catalog {
namespace {
// These exact local game files were verified to contain raw NVN programs inside
// G2S archive entries. Their .gz suffix does not imply a gzip stream. Compressed,
// relocated or other archive variants deliberately remain unresolved in v1.
constexpr const char* resources[] = {
    "data/action/shader/Normal.bin.gz", "data/deferred/DeferredShader.bin.gz",
    "data/shadow/ShadowShader.bin.gz", "data/stage/common/StageShader.bin.gz"
};
bool wanted(const std::string& name){for(auto r:resources)if(name==r)return true;return false;}
u32 word(const std::vector<u8>& b,u64 off){if(off>b.size() || b.size()-size_t(off)<4)throw std::runtime_error("RomFS word");u32 v;std::memcpy(&v,b.data()+off,4);return v;}
u64 wide(const std::vector<u8>& b,u64 off){return u64(word(b,off))|(u64(word(b,off+4))<<32);}
std::string name(const std::vector<u8>& b,u32 off,u32 header,u32 len){
    if(len>256 || u64(off)+header+len>b.size())throw std::runtime_error("RomFS name");
    return std::string((const char*)b.data()+off+header,len);
}
bool host_path(const std::string& root,const std::string& name,std::filesystem::path& out) {
    if(!safe_resource(name) || !wanted(name))return false;
    std::error_code error;auto base=std::filesystem::canonical(std::filesystem::u8path(root),error);if(error)return false;
    auto resolved=std::filesystem::canonical(base/std::filesystem::u8path(name),error);if(error)return false;
    auto rel=resolved.lexically_relative(base);if(rel.empty() || *rel.begin()=="..")return false;
    out=resolved;return true;
}
} // namespace
bool GameSources::resource(const Resource& file,std::vector<u8>& out) const {
    if(file.size>64*1024*1024)return false;
    std::vector<u8> bytes(size_t(file.size));
    if(nsp_) {if(!hwfs::nsp::active() || !hwfs::nsp::romfs_read(file.offset,bytes.data(),file.size))return false;}
    else {
        std::filesystem::path path;if(!host_path(root_,file.name,path))return false;
        FILE* f=_wfopen(path.c_str(),L"rb");if(!f)return false;
        bool ok=fread(bytes.data(),1,bytes.size(),f)==bytes.size() && fgetc(f)==EOF;fclose(f);if(!ok)return false;
    }
    out=std::move(bytes);return true;
}
bool GameSources::index() {
    files_.clear();shaders_.clear();nsp_=hwfs::nsp::active();root_=hw_romfs_path();
    try {
        if(nsp_) {
            const u64 size=hwfs::nsp::romfs_size();std::vector<u8> h(80);
            if(!hwfs::nsp::romfs_read(0,h.data(),80) || wide(h,0)!=80)return false;
            auto table=[&](u64 off,u64 count){
                if(count>16*1024*1024 || off>size || count>size-off)throw std::runtime_error("RomFS table");
                std::vector<u8> b(static_cast<size_t>(count));if(!hwfs::nsp::romfs_read(off,b.data(),count))throw std::runtime_error("RomFS read");return b;
            };
            auto dirs=table(wide(h,24),wide(h,32)), files=table(wide(h,56),wide(h,64));const u64 data=wide(h,72);
            std::set<u32> visited_dirs,visited_files;
            std::function<void(u32,std::string,u32)> walk=[&](u32 d,std::string path,u32 depth){
                if(depth>64 || visited_dirs.size()>100000 || !visited_dirs.insert(d).second)throw std::runtime_error("RomFS cycle");
                const auto nm=name(dirs,d,24,word(dirs,u64(d)+20));if(!nm.empty()){path+=nm;path+='/';}
                u32 f=word(dirs,u64(d)+12);
                while(f!=~0u) {
                    if(visited_files.size()>100000 || !visited_files.insert(f).second)throw std::runtime_error("RomFS file cycle");
                    auto path_name=path+name(files,f,32,word(files,u64(f)+28));
                    u64 off=wide(files,u64(f)+8), count=wide(files,u64(f)+16);
                    if(data>size || off>size-data || count>size-data-off)throw std::runtime_error("RomFS file bounds");
                    if(wanted(path_name))files_.push_back({path_name,data+off,count});
                    f=word(files,u64(f)+4);
                }
                u32 child=word(dirs,u64(d)+8);
                while(child!=~0u){walk(child,path,depth+1);child=word(dirs,u64(child)+4);}
            };
            walk(0,"",0);
        } else {
            for(const char* r:resources) {
                std::filesystem::path path;if(!host_path(root_,r,path))continue;
                std::error_code error;u64 size=std::filesystem::file_size(path,error);if(!error)files_.push_back({r,0,size});
            }
        }
        for(const auto& file:files_) {
            std::vector<u8> bytes;if(!resource(file,bytes))continue;
            size_t found=0;
            for(size_t off=0;off+0x90<=bytes.size();off+=4) {
                if(word(bytes,off)!=0x12345678)continue;
                const u32 stage=(word(bytes,off+0x30)>>10)&15;u32 length=0;Digest hash{};
                if(!source_identity(bytes.data()+off+0x30,bytes.size()-off-0x30,stage,length,hash))continue;
                if(shaders_.size()>=16384)throw std::runtime_error("source count");
                shaders_.push_back({file.name,u64(off+0x30),length,hash});++found;
            }
            hw_log("catalog: local resource %s: %zu raw NVN programs (no code exported)",file.name.c_str(),found);
        }
        hw_log("catalog: indexed %zu local shader identities from %zu selected game resources",shaders_.size(),files_.size());
        return !shaders_.empty();
    }catch(const std::exception& e){files_.clear();shaders_.clear();hw_log("catalog: source index unavailable: %s",e.what());return false;}
}
bool GameSources::locate(const u8* bytes,size_t available,u32 stage,Locator& out)const {
    u32 size=0;Digest hash{};if(!source_identity(bytes,available,stage,size,hash))return false;
    for(const auto& locator:shaders_)if(locator.size==size && locator.sha256==hash){out=locator;return true;}
    return false;
}
bool GameSources::read(const Locator& locator,std::vector<u8>& out)const {
    if(!safe_resource(locator.resource) || !wanted(locator.resource) || locator.size>kMaxSourceBytes)return false;
    for(const auto& file:files_)if(file.name==locator.resource) {
        if(locator.offset>file.size || locator.size>file.size-locator.offset)return false;
        std::vector<u8> bytes(locator.size);
        // Fresh read on every resolution, never the indexing buffer or a shader cache.
        if(nsp_) {if(!hwfs::nsp::romfs_read(file.offset+locator.offset,bytes.data(),bytes.size()))return false;}
        else {
            std::filesystem::path path;if(!host_path(root_,file.name,path))return false;
            FILE* f=_wfopen(path.c_str(),L"rb");if(!f)return false;
            bool ok=!_fseeki64(f,locator.offset,SEEK_SET) && fread(bytes.data(),1,bytes.size(),f)==bytes.size();fclose(f);if(!ok)return false;
        }
        out=std::move(bytes);return true;
    }
    return false;
}
bool GameSources::verify()const {
    for(const auto& l:shaders_) {
        std::vector<u8> bytes;u32 size=0;Digest hash{};
        if(!read(l,bytes) || bytes.size()<4)return false;
        u32 header;std::memcpy(&header,bytes.data(),4);
        if(!source_identity(bytes.data(),bytes.size(),(header>>10)&15,size,hash) || size!=l.size || hash!=l.sha256)return false;
    }
    return !shaders_.empty();
}
} // namespace gpu::catalog
