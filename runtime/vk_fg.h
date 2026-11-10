// Frame generation with AMD FSR 3.1 (vk_fg.cpp): AMD's frame generation swapchain in gfx_present.c's place.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

int vk_fg_wanted(void);                      // NX2NAVI_FRAMEGEN = fsr3 with DLSS or FSR upscaling on (live), where an FSR library loads
uint32_t vk_fg_queue_count(void);            // graphics-family queues the device creates
void vk_fg_queues(VkQueue present, VkQueue acquire);   // vk_device.c, after the device exists
int vk_fg_active(void);                      // the frame generation swapchain exists
int vk_fg_generating(void);                  // ... and the last frame had generation on (not paused for the menu)
// gfx_present.c's swapchain calls while vk_fg_wanted: the first create makes AMD's swapchain context, later ones
// (window resizes) its swapchain again. Non-success from the first: the plain swapchain path stays. With the FSR SDK 2
// loader (vk_ffx_sdk2) the handle is AMD's swapchain object: vk_fg_destroy_swapchain only notes it, the next create
// recreates it in place and vk_fg_release destroys it. Another format or colour space, an acquired image never
// presented or a failed recreation starts over as a first create, whose failure leaves the plain swapchain with frame
// generation off.
VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);
void vk_fg_destroy_swapchain(VkSwapchainKHR sc);
VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img);
VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx);
VkResult vk_fg_present(const VkPresentInfoKHR *pi);   // on the game's queue, its lock held by the caller
// Each present, before its command buffer is submitted: configures generation for this frame and records its
// preparation (the frame's depth and motion vectors, vk_dlaa_fg_inputs) into cb.
// hudless: this frame's picture without the HUD at the swapchain's size and format, in GENERAL (VK_NULL_HANDLE: none),
// from which generation tells the HUD apart and keeps it out of the generated frames.
// pic: the picture's rectangle in the swapchain {x, y, w, h} (black bars around it when the window has another shape);
// frames are generated inside it only.
void vk_fg_frame(VkCommandBuffer cb, VkSwapchainKHR sc, VkFormat fmt, VkExtent2D ext, VkImage hudless, VkImageView hudless_view,
                 const int pic[4]);
uint64_t vk_fg_presents(VkSwapchainKHR sc);   // frames the swapchain presented, generated ones included (0: off)
PFN_vkSetHdrMetadataEXT vk_fg_hdr_metadata_fn(void);   // AMD's replacement for vkSetHdrMetadataEXT while active, else NULL
void vk_fg_release(void);
