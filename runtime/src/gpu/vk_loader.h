// Minimal dynamic Vulkan loader (no SDK needed: vulkan-1.dll ships with the GPU driver).
#pragma once
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#define HW_VK_PLATFORM_INSTANCE_FUNCS(X) X(vkCreateWin32SurfaceKHR)
#else
#define HW_VK_PLATFORM_INSTANCE_FUNCS(X)
struct SDL_Window;
#endif
#include <vulkan/vulkan.h>

#define HW_VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)

#define HW_VK_INSTANCE_FUNCS(X)                                                                                  \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                          \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice)              \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) HW_VK_PLATFORM_INSTANCE_FUNCS(X)                    \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)  \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)                         \
    X(vkCreateDebugUtilsMessengerEXT)

#define HW_VK_DEVICE_FUNCS(X)                                                                                    \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle)               \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)         \
    X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool)                    \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)            \
    X(vkResetCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)               \
    X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkAllocateMemory) X(vkFreeMemory)           \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)         \
    X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)                            \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)        \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateGraphicsPipelines)                  \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkGetPipelineCacheData)          \
    X(vkGetMemoryHostPointerPropertiesEXT) X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2)                      \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkCmdClearColorImage)                 \
    X(vkCmdClearDepthStencilImage) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)        \
    X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdBindPipeline)             \
    X(vkCmdBindVertexBuffers) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw)                    \
    X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch)                        \
    X(vkCmdDispatchIndirect) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants)                    \
    X(vkCmdSetDepthBias) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference)  \
    X(vkCmdSetLineWidth) X(vkCmdSetDepthBounds) X(vkCmdSetCullMode) X(vkCmdSetFrontFace)                        \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable)                         \
    X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetPrimitiveRestartEnable) \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetDepthBiasEnable) X(vkCmdPushDescriptorSetKHR)                   \
    X(vkCmdPushConstants) X(vkCmdResolveImage)                                                                    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets)       X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets) X(vkCreateBufferView) X(vkDestroyBufferView)               X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) X(vkDestroyPipelineCache) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

// The pointers carry Vulkan's own function names. On Linux the game is linked with -rdynamic (named backtraces), so
// they must stay out of its dynamic symbol table: otherwise Vulkan layers loaded into the game (gamescope's WSI
// layer, MangoHud, the Steam overlay) bind their calls of e.g. vkEnumerateInstanceExtensionProperties to these
// variables instead of the loader's functions and jump into data.
#ifdef _WIN32
#define HW_VK_HIDDEN
#else
#define HW_VK_HIDDEN __attribute__((visibility("hidden")))
#endif
#define HW_VK_DECLARE(fn) extern HW_VK_HIDDEN PFN_##fn fn;
HW_VK_GLOBAL_FUNCS(HW_VK_DECLARE)
HW_VK_INSTANCE_FUNCS(HW_VK_DECLARE)
HW_VK_DEVICE_FUNCS(HW_VK_DECLARE)
extern HW_VK_HIDDEN PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

bool hw_vk_load_global();
void hw_vk_load_instance(VkInstance inst);
void hw_vk_load_device(VkDevice dev);
