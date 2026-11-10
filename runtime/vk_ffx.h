// AMD FidelityFX (vk_ffx.cpp): FSR 3.1 temporal upscaling through amd_fidelityfx_vk.dll, loaded at run time, or else
// through the runtime's own build of the upscaler's passes (vk_fsr3.c).
#pragma once
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;   // level 0, layer 0, in VK_IMAGE_LAYOUT_GENERAL
    VkFormat format;
    uint32_t width, height;
    int storage;     // created with VK_IMAGE_USAGE_STORAGE_BIT (FSR writes it)
    VkImageView view;   // a 2D view of level 0, layer 0 (the depth aspect for depth): the built-in passes bind it
} VkFfxImage;

// 1 when FSR 3.1 upscaling can run: the library is there with its entry points (loaded once), or else the built-in
// passes are made (they need the device: 0 before it exists, and asked again then).
int vk_ffx_load(void);
// Whether FSR 3.1 upscaling can run in this build, for the settings menu: after vk_ffx_load ran, what it found; before,
// whether the library file is there (asked once without loading it) or else whether the built-in passes may run.
int vk_ffx_available(void);
// The same for frame generation (vk_fg.cpp), which runs through the library's own API calls (vk_ffx_create and the
// rest below), not only an upscaler: never with the built-in passes, so never on Linux.
int vk_ffx_framegen_available(void);
// Records FSR 3.1 into cb: color (the scene light, render size), depth (D32F), mv (R16G16F, render pixels, current to
// previous), out (the output size, RGBA16F, storage). jx, jy: the jitter the scene took, render pixels. fov_y in
// radians (0: unknown). 0 on failure (the caller falls back as it does when DLSS fails).
int vk_ffx_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                   const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, float fov_y);
void vk_ffx_release(void);   // before the device goes away

#ifdef __cplusplus
// The API's calls themselves, for vk_fg.cpp (0 = FFX_API_RETURN_OK): the library loaded first (vk_ffx_load), not
// the built-in passes.
#include "third_party/ffx_api/ffx_api.h"
extern "C" {
ffxReturnCode_t vk_ffx_create(ffxContext *ctx, ffxCreateContextDescHeader *desc);
ffxReturnCode_t vk_ffx_destroy(ffxContext *ctx);
ffxReturnCode_t vk_ffx_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc);
ffxReturnCode_t vk_ffx_query(ffxContext *ctx, ffxQueryDescHeader *desc);
ffxReturnCode_t vk_ffx_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc);
}
#endif
