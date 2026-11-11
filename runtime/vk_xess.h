// Intel XeSS (vk_xess.cpp): temporal upscaling through libxess.dll, loaded at run time from the executable's folder.
#pragma once

#include <vulkan/vulkan.h>

int vk_xess_load(void);   // 1 when the library is there with its entry points (loaded once)
int vk_xess_loaded(void); // 1 once vk_xess_load found libxess.dll: XeSS can be asked for (the menu's list)
int vk_xess_present(void); // 1 when libxess.dll sits beside the executable, without loading it (the menu's list)
// 1 when the device was made with XeSS's extensions and features (vk_xess_device_features succeeded): only then can a
// context be created, and only a restart can turn it on otherwise.
int vk_xess_device_ready(void);
// XeSS asks for instance and device extensions and for device features at device creation, so these run from
// vk_device.c before vkCreateInstance/vkCreateDevice, and only when the ini asked for XeSS at start (as DLSS does).
// 1 when something was added.
int vk_xess_instance_exts(const char **ext, uint32_t *n, uint32_t max);
int vk_xess_device_exts(VkInstance inst, VkPhysicalDevice phys, const VkExtensionProperties *have, uint32_t nhave,
                        const char **ext, uint32_t *n, uint32_t max);
// Appends XeSS's feature chain to *tail, the pNext slot of the last structure in the chain (XeSS patches the chain it is
// given, so nothing of the runtime may follow it). 1 when the device has what XeSS asks for.
int vk_xess_device_features(VkInstance inst, VkPhysicalDevice phys, void **tail);
// Records XeSS into cb: color (the scene light, render size), depth (D32F, not inverted), mv (R16G16F, render pixels,
// current to previous), out (the output size, RGBA16F, storage). jx, jy: the jitter the scene took, render pixels.
// 0 on failure (the caller falls back as it does when FSR 3.1 fails).
int vk_xess_upscale(VkCommandBuffer cb, VkImage color, VkFormat color_format, VkImage depth, VkFormat depth_format,
                    VkImage mv, VkImage out, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, float jx, float jy,
                    int reset);
void vk_xess_release(void);   // before the device goes away
