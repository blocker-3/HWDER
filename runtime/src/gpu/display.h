// Window, Vulkan device and presentation.
#pragma once
#include <atomic>
#include <mutex>
#include <string>

#include "hwder/runtime.h"
#include "vk_loader.h"

namespace display {

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;          // renderer queue
    VkQueue present_queue = VK_NULL_HANDLE;  // presentation (same as queue when the family has one queue)
    u32 queue_family = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    std::mutex queue_lock;
};
Device& device();

// Opens the window and initializes Vulkan.
void init();
// Starts the presenter for the game: the boot screen until frames arrive (and the 60 Hz monitor switch).
void start();
// One frame of just the overlay (the prelaunch screen) on a dark backdrop. Not paced: the caller sleeps.
void present_overlay_frame();
HWND window();

// Present a guest frame (an image in GENERAL layout). Paced to the guest swap interval.
// wait_sem/wait_value: timeline semaphore the presentation blit waits on (the renderer's last submit).
void present_image(VkImage image, u32 width, u32 height, u32 swap_interval, VkSemaphore wait_sem = VK_NULL_HANDLE,
                   u64 wait_value = 0);

// Completion ticket of the last present_image's blit (0 if it submitted none). wait_present_done blocks until
// that blit has finished reading its source image (false on timeout).
u64 last_present_ticket();
bool wait_present_done(u64 ticket, u64 timeout_ns);

// Status shown in the title bar while booting.
void set_status(const std::string& s);
// Frame interpolation: pace presents at `hz` (0 = the 60 Hz game grid with swap-interval slots).
void set_output_hz(double hz);
void client_size(unsigned* w, unsigned* h);  // current swapchain / window client size (0 if none yet)
double monitor_hz();

}  // namespace display
