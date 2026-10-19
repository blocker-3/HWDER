// Read-only local resource proof and metadata-catalog verification. Never dumps code.
#include "gpu/vk/catalog_game_sources.h"
#include "fs/nsp.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <windows.h>
namespace hwfs {
std::wstring utf8_to_wide(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring out(n,L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),out.data(),n);return out;
}
std::string wide_to_utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string out(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;
}
}
extern "C" void hw_log(const char* fmt,...) {va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fputc('\n',stderr);}
extern "C" void hw_fatal(const char*) {std::abort();}
int main(int argc,char** argv) {
    if(argc!=4 && argc!=7){fprintf(stderr,"usage: pipeline_catalog_tool game.nsp prod.keys exe-directory [catalog.bin build-tag game-version]\n");return 2;}
    hw_config_load(argv[3]);hwfs::nsp::set_paths(argv[1],argv[2]);std::string error;
    if(!hwfs::nsp::init(argv[3],error)){fprintf(stderr,"%s\n",error.c_str());return 1;}
    gpu::catalog::GameSources sources;if(!sources.index())return 1;
    printf("Verified %zu local raw-NVN identities; no game or shader bytes written.\n",sources.count());
    if(argc==7) {
        gpu::catalog::Catalog cat;
        auto status=gpu::catalog::load(argv[4],strtoull(argv[5],nullptr,0),strtoul(argv[6],nullptr,0),cat);
        if(status!=gpu::catalog::Status::Ok){fprintf(stderr,"catalog: %s\n",gpu::warmup::name(status));return 1;}
        auto cache=gpu::catalog::prepare(cat,[&](const auto& l,auto& b){return sources.read(l,b);},[](const auto& p){if(p.done==p.total)printf("Resolved %u/%u programs, unresolved %u\n",p.resolved,p.total,p.unresolved);});
        printf("Locally translated %zu programs / %zu recipes; no compiled output persisted.\n",cache.programs.size(),cache.pipelines.size());
        return cache.pipelines.size()==cat.pipelines.size()?0:1;
    }
    return 0;
}
