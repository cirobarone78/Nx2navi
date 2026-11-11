// AMD FidelityFX (vk_ffx.cpp): FSR 3.1 temporal upscaling through amd_fidelityfx_vk.dll, loaded at run time.
#pragma once
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;   // level 0, layer 0, in VK_IMAGE_LAYOUT_GENERAL
    VkFormat format;
    uint32_t width, height;
    int storage;     // created with VK_IMAGE_USAGE_STORAGE_BIT (FSR writes it)
} VkFfxImage;

int vk_ffx_load(void);   // 1 when the library is there with its entry points (loaded once)
// Whether FSR 3.1 upscaling can run in this build, for the settings menu: after vk_ffx_load ran, what it found; before,
// whether the library file is there, asked once without loading it. 0 where the build has no FSR 3.1 to load.
int vk_ffx_available(void);
// The same for frame generation (vk_fg.cpp), which runs through the library's own API calls (vk_ffx_create and the
// rest below), not only an upscaler.
int vk_ffx_framegen_available(void);
// Records FSR 3.1 into cb: color (the scene light, render size), depth (D32F), mv (R16G16F, render pixels, current to
// previous), out (the output size, RGBA16F, storage). jx, jy: the jitter the scene took, render pixels. fov_y in
// radians (0: unknown). 0 on failure (the caller falls back as it does when DLSS fails).
int vk_ffx_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                   const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, float fov_y);
void vk_ffx_release(void);   // before the device goes away
// The device's functions as the library is given them (ffxCreateBackendVKDesc.vkDeviceProcAddr): its pipeline creates
// go through the runtime's pipeline caches (vk_pipeline.c vk_pipeline_create_foreign; NX2NAVI_FFX_PIPE_CACHE=0: not).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_ffx_proc_addr(VkDevice dev, const char *name);

// AMD FSR 4 (vk_ffx4.cpp): the ML upscaler of AMD's FSR SDK 2.4, through amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll beside the executable. vk_ffx_upscale runs it in FSR 3.1's place when the setting
// asks (NX2NAVI_FSR4) and it runs here, and falls back to FSR 3.1 when it fails.
int vk_ffx4_available(void);      // the libraries are there and list a 4.x version for the device; asked once, with the
                                  // device, from any thread (0 before the device exists)
const char *vk_ffx4_name(void);   // "AMD FSR 4.1.2" once vk_ffx4_available found it
int vk_ffx4_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                    const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, float fov_y);   // as vk_ffx_upscale
void vk_ffx4_release(void);
int vk_ffx4_device_wanted(void);  // Linux: amd_fidelityfx_upscaler_vk.so is beside the executable, so the device is made
                                  // with what it needs (VK_EXT_descriptor_buffer; FP8 matrices where reported); else 0
int vk_ffx4_device_ready(void);   // the device was made with VK_EXT_descriptor_buffer (vk_device.c)
int vk_ffx_fsr4(void);            // the setting asks for FSR 4 and it runs here: what vk_ffx_upscale uses, for the menu

#if defined(__cplusplus) && !defined(VK_FFX_NO_API)
// The API's calls themselves, for vk_fg.cpp (0 = FFX_API_RETURN_OK): the library loaded first (vk_ffx_load).
#include "third_party/ffx_api/ffx_api.h"
extern "C" {
ffxReturnCode_t vk_ffx_create(ffxContext *ctx, ffxCreateContextDescHeader *desc);
ffxReturnCode_t vk_ffx_destroy(ffxContext *ctx);
ffxReturnCode_t vk_ffx_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc);
ffxReturnCode_t vk_ffx_query(ffxContext *ctx, ffxQueryDescHeader *desc);
ffxReturnCode_t vk_ffx_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc);
}
#endif
