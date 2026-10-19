// Dear ImGui overlay drawn into the swapchain image: the F1 video-options menu and the FPS readout.
#pragma once
#include "gpu/vk_loader.h"

#include <cstdint>
#include <vector>

union SDL_Event;

namespace overlay {

// Window messages the overlay posts to the window thread (handled in display.cpp).
#ifdef _WIN32
constexpr UINT kMsgBorderless = WM_APP + 1;  // lParam: 1 = on, 0 = off
constexpr UINT kMsgClientSize = WM_APP + 2;  // lParam: (w << 16) | h
constexpr UINT kMsgDisplay60 = WM_APP + 3;   // lParam: 1 = switch the monitor to 60 Hz, 0 = restore

void init(HWND hwnd, VkInstance instance, VkPhysicalDevice phys, VkDevice dev, uint32_t queue_family, VkQueue queue,
          VkFormat swapchain_format);
bool wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp);  // true = consumed by the overlay
#else
void init(SDL_Window* window, VkInstance instance, VkPhysicalDevice phys, VkDevice dev, uint32_t queue_family, VkQueue queue,
          VkFormat swapchain_format);
bool process_event(const SDL_Event& event);
#endif
void toggle();
bool visible();
void on_swapchain(const std::vector<VkImage>& images, VkFormat fmt);  // called with the device idle
// Records the overlay into `image` (must be in COLOR_ATTACHMENT_OPTIMAL layout). Cheap when hidden.
void render(VkCommandBuffer cb, VkImage image, VkExtent2D extent);
bool swapchain_dirty();  // vsync setting changed: recreate the swapchain

// A full-window screen drawn instead of the menu (the prelaunch screen), called inside the ImGui frame.
// While one is set the overlay takes all window input and the menu cannot be opened. nullptr removes it.
using ScreenFn = void (*)(VkExtent2D extent);
void set_screen(ScreenFn fn);

}  // namespace overlay
