// Public synthetic compatibility probe. No game data, GPU, window, or codec use.
#include <windows.h>
#include <dbghelp.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <audioclient.h>
#include <malloc.h>
#include <cstdio>
#include <stdexcept>
#include <atomic>
#include "hwder/cpu.h"

static volatile LONG observed=0;
static LONG CALLBACK handler(EXCEPTION_POINTERS* p) {
    if(p->ExceptionRecord->ExceptionCode==0xE0424844) {
        InterlockedIncrement(&observed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
__declspec(noinline) static void tail(Ctx* c) {
    if(!c->x[0])return;
    --c->x[0];
    HW_MUSTTAIL return tail(c);
}
__declspec(noinline) static void throw_test(int& cleaned) {
    struct Guard {int& value;~Guard(){++value;}} guard{cleaned};
    throw std::runtime_error("synthetic private-toolchain exception");
}
int main(int argc,char**) {
    Ctx c{};
    c.x[0]=1000000;
    tail(&c);
    if(c.x[0])return 1;
    alignas(16) u64 pair[2]={5,7};
    u64 a=0,b=0;
    EXCL_LOADP64(&c,(u64)pair,&a,&b);
    if(a!=5||b!=7||!EXCL_STOREP64(&c,(u64)pair,11,13)||pair[0]!=11||pair[1]!=13)return 2;
    EXCL_LOADP64(&c,(u64)pair,&a,&b);
    pair[1]=17; // independent writer invalidates the exclusive expected value
    if(EXCL_STOREP64(&c,(u64)pair,19,23)||pair[0]!=11||pair[1]!=17)return 3;
    void* aligned=_aligned_malloc(sizeof(Ctx),64);
    if(!aligned||((uintptr_t)aligned&63))return 4;
    _aligned_free(aligned);
    int cleaned=0;
    try {throw_test(cleaned);} catch(const std::runtime_error&) {}
    if(cleaned!=1)return 5;
    auto token=AddVectoredExceptionHandler(1,handler);
    if(!token)return 6;
    RaiseException(0xE0424844,0,0,nullptr);
    RemoveVectoredExceptionHandler(token);
    if(observed!=1)return 7;
    CONTEXT context{};
    RtlCaptureContext(&context);
    DWORD64 base=0;
    // Exercise Windows unwind metadata discovery without rewriting the stack.
    if(!RtlLookupFunctionEntry(context.Rip,&base,nullptr))return 8;
    std::atomic<unsigned long long> count{0};
    count.fetch_add(1);
    if(count.load()!=1)return 9;
    // These non-executed paths require actual MF/COM/codec import libraries,
    // rather than accepting headers while leaving link compatibility untested.
    if(argc==12345) {
        MFStartup(MF_VERSION);
        IMFTransform* decoder=nullptr;
        CoCreateInstance(CLSID_CMSH264DecoderMFT,nullptr,CLSCTX_INPROC_SERVER,
                         IID_IMFTransform,(void**)&decoder);
        if(decoder)decoder->Release();
        MFShutdown();
    }
    std::puts("PASS: musttail/128-bit exclusives/aligned CRT/C++ unwind/VEH/Windows unwind metadata");
    return 0;
}
