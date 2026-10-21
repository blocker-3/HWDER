// Vulkan renderer: executes Maxwell 3D / compute / 2D work recorded by the GPU core.
//
// Design (simple and correct first):
//  * one command buffer is recorded at a time; flush_commands() submits it and signals a timeline
//    semaphore with its serial. A waiter thread runs deferred callbacks (syncpoint increments,
//    semaphore releases, resource destruction) once their serial completes.
//  * every image lives in VK_IMAGE_LAYOUT_GENERAL; synchronization uses full memory barriers between
//    rendering instances / transfers.
//  * uniform, vertex and index data are copied per draw into a host-visible ring (snapshot semantics
//    match the GPU's: constant buffer updates between draws are honored).
#include "vk_common.h"
#include "vk_renderer.h"

namespace gpu {
namespace vk {

// ------------------------------------------------------------------ basics
u32 find_memory_type(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    const auto& mp = display::device().mem;
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want &&
            !(mp.memoryTypes[i].propertyFlags & avoid))
            return i;
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

Buffer create_buffer(u64 size, VkBufferUsageFlags usage, bool host_visible, bool prefer_device_local, bool cached) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VK_CHECK(vkCreateBuffer(dev(), &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev(), b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    VkMemoryPropertyFlags want = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                              : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (host_visible && cached) {
        VkMemoryPropertyFlags want_c = want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, want_c, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((display::device().mem.memoryTypes[ai.memoryTypeIndex].propertyFlags & want_c) == want_c)
            r = vkAllocateMemory(dev(), &ai, nullptr, &b.mem);
    }
    if (r != VK_SUCCESS && host_visible && prefer_device_local) {
        ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (display::device().mem.memoryTypes[ai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            r = vkAllocateMemory(dev(), &ai, nullptr, &b.mem);
    }
    if (r != VK_SUCCESS) {
        ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, want,
                                              host_visible ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0);
        VK_CHECK(vkAllocateMemory(dev(), &ai, nullptr, &b.mem));
    }
    VK_CHECK(vkBindBufferMemory(dev(), b.buf, b.mem, 0));
    if (host_visible) VK_CHECK(vkMapMemory(dev(), b.mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.map));
    return b;
}

void destroy_buffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev(), b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev(), b.mem, nullptr);
    b = Buffer{};
}

void decode_astc(const u8* src, u32 width, u32 height, u32 depth, u32 bw, u32 bh, u8* dst);

}  // namespace vk
}  // namespace gpu

namespace gpu {
using namespace vk::detail;
Renderer* create_renderer(MemoryManager* mm) { return new VkRenderer(mm); }

}  // namespace gpu
