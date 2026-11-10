// AMD FidelityFX / FSR (vk_ffx.cpp): temporal upscaling and frame generation through the AMD FSR API, loaded at run
// time. With the AMD FSR SDK 2 loader and both its effect libraries next to the executable (amd_fidelityfx_loader_vk.dll,
// amd_fidelityfx_upscaler_vk.dll and amd_fidelityfx_framegeneration_vk.dll) the API offers each effect in the versions
// the GPU runs (FSR 4 upscaling on Radeon RX 9000 and RX 7000 cards, FSR Frame Generation 4 on RX 9000 cards, FSR 3.1.x
// elsewhere); without them, the FSR 3.1 library amd_fidelityfx_vk.dll as before. nx2navi.ini ffx_sdk2 = 0 keeps the FSR
// 3.1 library.
#pragma once
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;   // level 0, layer 0, in VK_IMAGE_LAYOUT_GENERAL
    VkFormat format;
    uint32_t width, height;
    int storage;     // created with VK_IMAGE_USAGE_STORAGE_BIT (FSR writes it)
} VkFfxImage;

int vk_ffx_load(void);   // 1 when the library is there with its entry points (loaded once)
// Whether FSR upscaling can run in this build, for the settings menu: after vk_ffx_load ran, what it found; before,
// whether a library it takes is there (the FSR 3.1 library, or the FSR SDK 2 loader with both effect libraries),
// asked once without loading it. 0 where the build has no FSR to load.
int vk_ffx_available(void);
// The same for frame generation (vk_fg.cpp), which runs through the library's own API calls (vk_ffx_create and the
// rest below), not only an upscaler.
int vk_ffx_framegen_available(void);
// Before the device exists (vk_device.c): 1 when the FSR SDK 2 loader and both its effect libraries are next to the
// executable and ffx_sdk2 is not 0, so the device enables what their ML passes need (cooperative matrices, FP8). Decided
// once.
int vk_ffx_sdk2_wanted(void);
int vk_ffx_sdk2(void);   // after vk_ffx_load: 1 when the FSR SDK 2 loader is the library in use
// Records the temporal upscaler into cb: color (the scene light, render size), depth (D32F), mv (R16G16F, render
// pixels, current to previous), out (the output size, RGBA16F, storage). jx, jy: the jitter the scene took, render
// pixels. fov_y in radians (0: unknown). 0 on failure (the caller falls back as it does when DLSS fails).
int vk_ffx_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                   const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, float fov_y);
void vk_ffx_release(void);   // before the device goes away

#ifdef __cplusplus
// The API's calls themselves, for vk_fg.cpp (0 = FFX_API_RETURN_OK): the library loaded first (vk_ffx_load).
#include "third_party/ffx_sdk24/api/include/ffx_api.h"
#include "third_party/ffx_sdk24/api/include/vk/ffx_api_vk.h"
extern "C" {
ffxReturnCode_t vk_ffx_create(ffxContext *ctx, ffxCreateContextDescHeader *desc);
ffxReturnCode_t vk_ffx_destroy(ffxContext *ctx);
ffxReturnCode_t vk_ffx_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc);
ffxReturnCode_t vk_ffx_query(ffxContext *ctx, ffxQueryDescHeader *desc);
ffxReturnCode_t vk_ffx_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc);
}
// The Vulkan back end description a context creation chains in: SDK 2's (a VkDeviceContext) or the FSR 3.1 library's.
struct VkFfxBackend {
    ffxCreateBackendVKDesc sdk2;
    ffxCreateBackendVKDesc_DEPRECATED v31;
};
ffxApiHeader *vk_ffx_backend(VkFfxBackend *b);   // its pNext is null
// SDK 2 only: when want (an ini value such as "4" or "3.1.6") starts a version name the API offers for the effect of
// createDescType, fills *o to be chained into its creation and returns 1. Logs the offered versions once per effect.
int vk_ffx_version(uint64_t createDescType, const char *what, const char *want, ffxOverrideVersion *o);
// The provider a created context runs (its version name), or "provider unknown".
const char *vk_ffx_provider(ffxContext *ctx, char *buf, size_t n);
#endif
