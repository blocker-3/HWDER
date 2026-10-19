// Local private-input timing probe. Never copies input into tracked fixtures.
#include "gpu/vk/pipeline_warmup_cache.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

using Clock = std::chrono::steady_clock;
using namespace gpu::warmup;
static double ms(Clock::time_point begin) { return std::chrono::duration<double, std::milli>(Clock::now()-begin).count(); }
static void require(bool ok, const char* what) { if (!ok) { std::fprintf(stderr,"failed: %s\n",what); std::exit(1); } }
static std::vector<u8> read(const char* path) {
    std::ifstream f(path,std::ios::binary);
    require(bool(f),"open input"); return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    if (argc!=4) { std::fprintf(stderr,"usage: pipeline_cache_persistence_bench private-recipes private-native output-prefix\n"); return 2; }
    auto bytes=read(argv[1]); require(bytes.size()>=40,"recipe header");
    u64 tag=0; for (int i=0;i<8;++i) tag|=u64(bytes[16+i])<<(8*i);
    Cache cache; require(load(argv[1],tag,cache)==Status::Ok,"load recipes");
    auto native=read(argv[2]); require(native.size()>=32,"native header");
    HMODULE loader=LoadLibraryA("vulkan-1.dll"); require(loader!=nullptr,"Vulkan loader");
    auto get=(PFN_vkGetInstanceProcAddr)GetProcAddress(loader,"vkGetInstanceProcAddr"); require(get!=nullptr,"instance dispatch");
    auto create=(PFN_vkCreateInstance)get(nullptr,"vkCreateInstance");
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; VkInstance instance;
    require(create(&info,nullptr,&instance)==VK_SUCCESS,"instance");
#define INSTANCE_FN(name) auto name=(PFN_##name)get(instance,#name); require(name!=nullptr,#name)
    INSTANCE_FN(vkEnumeratePhysicalDevices); INSTANCE_FN(vkGetPhysicalDeviceProperties);
    INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties); INSTANCE_FN(vkCreateDevice); INSTANCE_FN(vkDestroyInstance);
    u32 count=0; require(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS && count,"physical devices");
    std::vector<VkPhysicalDevice> devices(count); require(vkEnumeratePhysicalDevices(instance,&count,devices.data())==VK_SUCCESS,"physical device list");
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    for (auto device:devices) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(device,&p);
        u32 vendor=0,id=0; std::memcpy(&vendor,native.data()+8,4); std::memcpy(&id,native.data()+12,4);
        if (p.vendorID==vendor && p.deviceID==id) { physical=device; std::printf("device: %s, driver %u\n",p.deviceName,p.driverVersion); break; }
    }
    require(physical!=VK_NULL_HANDLE,"matching native cache GPU");
    count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr); std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data()); u32 family=0;
    while(family<count && !(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
    require(family<count,"graphics queue"); float priority=1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; VkDevice device;
    require(vkCreateDevice(physical,&di,nullptr,&device)==VK_SUCCESS,"device");
#define DEVICE_FN(name) auto name=(PFN_##name)get(instance,#name); require(name!=nullptr,#name)
    DEVICE_FN(vkCreatePipelineCache); DEVICE_FN(vkGetPipelineCacheData); DEVICE_FN(vkDestroyPipelineCache); DEVICE_FN(vkDestroyDevice);
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; ci.initialDataSize=native.size(); ci.pInitialData=native.data(); VkPipelineCache driver;
    require(vkCreatePipelineCache(device,&ci,nullptr,&driver)==VK_SUCCESS,"native cache");
    std::string recipe_out=std::string(argv[3])+".recipes", native_out=std::string(argv[3])+".native";
    std::printf("programs=%zu recipes=%zu input_bytes=%zu native_bytes=%zu\n",cache.programs.size(),cache.pipelines.size(),bytes.size(),native.size());
    for (int i=0;i<7;++i) {
        auto begin=Clock::now(); Cache snapshot=cache; double snapshot_ms=ms(begin); SaveTimings t;
        require(save(recipe_out.c_str(),tag,snapshot,&t)==Status::Ok,"save recipes");
        begin=Clock::now(); size_t n=0;
        require(vkGetPipelineCacheData(device,driver,&n,nullptr)==VK_SUCCESS && n,"native size"); std::vector<u8> blob(n);
        require(vkGetPipelineCacheData(device,driver,&n,blob.data())==VK_SUCCESS,"native bytes"); double native_ms=ms(begin);
        begin=Clock::now(); FILE* f=std::fopen(native_out.c_str(),"wb"); require(f!=nullptr,"native open");
        bool wrote=std::fwrite(blob.data(),1,n,f)==n; int closed=std::fclose(f); require(wrote && !closed,"native write");
        std::printf("sample=%d snapshot=%.3f validation=%.3f serialization=%.3f recipe_disk=%.3f native_retrieval=%.3f native_disk=%.3f ms\n",i,snapshot_ms,t.validation_ms,t.serialization_ms,t.disk_ms,native_ms,ms(begin));
    }
    require(read(recipe_out.c_str())==bytes,"identical recipe encoding");
    vkDestroyPipelineCache(device,driver,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr); FreeLibrary(loader);
}
