// AMD FidelityFX through its API library (amd_fidelityfx_vk.dll next to the executable, loaded at run time): FSR 3.1
// temporal upscaling here, in the place of DLSS in vk_dlaa.c, and frame generation (vk_ffx_fg_*, below). The API's
// headers are in third_party/ffx_api (MIT); they are C++ in places, hence this file. Nothing of it runs unless the
// upscaler or frame generation asks for FSR 3.1.
//
// FSR 3.1 takes the same inputs vk_dlaa.c makes for DLSS: the scene's light target (linear HDR), its D32F depth
// (OpenGL-style, an infinite far plane, not inverted) and the camera motion vectors in render pixels from the current
// frame to the previous one, the convention both share, so the vectors go in unscaled.
#ifdef _WIN32
#include <windows.h>
#endif
#include <atomic>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "third_party/ffx_api/ffx_api.h"
#include "third_party/ffx_api/ffx_upscale.h"
#include "third_party/ffx_api/vk/ffx_api_vk.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_ffx.h"
}

namespace {

struct Api {
    void *lib = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    int tried = 0, ok = 0;
} A;

// What the settings menu may ask before anything has used FSR 3.1 (vk_ffx_available), from any thread.
std::atomic<int> g_loaded{-1};    // what vk_ffx_load found: 1 or 0; -1 while it has not run
std::atomic<int> g_present{-1};   // whether the library file is there: 1 or 0; -1 while not asked

#ifdef _WIN32
// amd_fidelityfx_vk.dll next to the executable
void library_path(char *path, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
    while (n && path[n - 1] != '\\') n--;
    snprintf(path + n, size - n, "amd_fidelityfx_vk.dll");
}
#endif

void message(uint32_t type, const wchar_t *msg) {
    char s[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof s - 1; n++) s[n] = msg[n] < 128 ? (char)msg[n] : '?';
    s[n] = 0;
    static int told;
    if (told++ < 40) rt_log("ffx: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error:" : "warning:", s);
}

// One upscaling context per render and output size: each render scale the menu sets brings its own, and so do the
// game's second scenes. When all are taken, the least recently used one is destroyed for the new size once the GPU is
// idle, as vk_dlaa.c free_size does for DLSS. Without that, a seventh size found no context, and vk_dlaa.c turned
// FSR 3.1 off for the rest of the run: stepping through the render scales in the menu was enough.
struct Up {
    ffxContext ctx = nullptr;
    uint32_t w = 0, h = 0, ow = 0, oh = 0;
    uint64_t used = 0;   // up_tick when context_for last returned it
} U[6];
uint64_t up_tick;
// A context used in the last few calls may sit in a command buffer that is not submitted yet, where waiting for the
// GPU does not cover it: a frame makes one or two calls (the scene, and a second scene after it), so this is a few
// frames back.
const uint64_t UP_RECENT = 8;

FfxApiResource image(const VkFfxImage *i, uint32_t state, uint32_t usage) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i->format;
    ci.extent = {i->width, i->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = i->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0;
    return ffxApiGetResourceVK((void *)i->image, ffxApiGetImageResourceDescriptionVK(i->image, ci, usage), state);
}

Up *context_for(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    up_tick++;
    for (Up &u : U)
        if (u.ctx && u.w == w && u.h == h && u.ow == ow && u.oh == oh) {
            u.used = up_tick;
            return &u;
        }
    Up *slot = nullptr, *lru = nullptr;
    for (Up &u : U) {
        if (!u.ctx) {
            slot = &u;
            break;
        }
        if (up_tick - u.used >= UP_RECENT && (!lru || u.used < lru->used)) lru = &u;
    }
    const VkCtx *c = vk_ctx();
    if (!slot && lru) {
        static int told;
        if (told++ < 20)
            rt_log("ffx: FSR 3.1 context %ux%u -> %ux%u destroyed for %ux%u -> %ux%u (all %d in use)", lru->w, lru->h, lru->ow,
                   lru->oh, w, h, ow, oh, (int)(sizeof U / sizeof U[0]));
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();   // its work may still be in flight
        A.destroy(&lru->ctx, nullptr);
        *lru = Up();
        slot = lru;
    }
    if (!slot) return nullptr;
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.vkDevice = c->device;
    be.vkPhysicalDevice = c->phys;
    be.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescUpscale d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &be.header;
    d.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    d.maxRenderSize = {w, h};
    d.maxUpscaleSize = {ow, oh};
    d.fpMessage = message;
    ffxReturnCode_t r = A.create(&slot->ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        rt_log("ffx: FSR 3.1 context creation at %ux%u -> %ux%u failed (%u)", w, h, ow, oh, (unsigned)r);
        slot->ctx = nullptr;
        return nullptr;
    }
    slot->w = w, slot->h = h, slot->ow = ow, slot->oh = oh, slot->used = up_tick;
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    A.query(&slot->ctx, &v.header);
    rt_log("ffx: FSR 3.1 upscaling at %ux%u -> %ux%u (%s)", w, h, ow, oh, v.versionName ? v.versionName : "provider unknown");
    return slot;
}

}  // namespace

extern "C" int vk_ffx_load(void) {
    if (A.tried) return A.ok;
    A.tried = 1;
#ifdef _WIN32
    char path[MAX_PATH];
    library_path(path, sizeof path);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        rt_log("ffx: %s not found: FSR 3.1 off", path);
        g_loaded.store(0, std::memory_order_release);
        return 0;
    }
    A.lib = m;
    A.create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    A.query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    A.ok = A.create && A.destroy && A.configure && A.query && A.dispatch;
    rt_log("ffx: amd_fidelityfx_vk.dll %s", A.ok ? "loaded" : "lacks the API entry points: FSR 3.1 off");
#else
    // Only the Windows build loads AMD's library: there is nothing to load here
    rt_log("ffx: this build has no FSR 3.1 (it needs amd_fidelityfx_vk.dll, which only the Windows build loads): FSR 3.1 off");
#endif
    g_loaded.store(A.ok, std::memory_order_release);
    return A.ok;
}

extern "C" int vk_ffx_available(void) {
    int loaded = g_loaded.load(std::memory_order_acquire);
    if (loaded >= 0) return loaded;
#ifdef _WIN32
    int present = g_present.load(std::memory_order_relaxed);
    if (present < 0) {   // the menu asks every frame: look once, and leave the loading to the first use
        char path[MAX_PATH];
        library_path(path, sizeof path);
        DWORD attr = GetFileAttributesA(path);
        present = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        g_present.store(present, std::memory_order_relaxed);
    }
    return present;
#else
    return 0;
#endif
}

extern "C" int vk_ffx_framegen_available(void) { return vk_ffx_available(); }   // the same library, its API calls

extern "C" int vk_ffx_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                              const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, float fov_y) {
    if (!vk_ffx_load()) return 0;
    Up *u = context_for(color->width, color->height, out->width, out->height);
    if (!u) return 0;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    // Every image is in GENERAL, which the backend reads as its unordered-access state, and is left there
    d.color = image(color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.depth = image(depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.motionVectors = image(mv, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.output = image(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {color->width, color->height};
    d.upscaleSize = {out->width, out->height};
    // The sharpness slider (0-100) drives FSR's own RCAS here, as it does for FSR 1
    int sharp = cfg_int("NX2NAVI_SHARPNESS");
    d.enableSharpening = sharp > 0;
    d.sharpness = sharp > 100 ? 1.0f : sharp / 100.0f;
    d.frameTimeDelta = dt_ms;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    d.cameraNear = 0.1f;   // Ocarina's projection (vk_dlaa.c ocam_vp): near 0.1, far at infinity
    d.cameraFar = 1e8f;
    d.cameraFovAngleVertical = fov_y > 0 ? fov_y : 1.0f;
    d.viewSpaceToMetersFactor = 0.01f;   // the game's units are centimetres
    ffxReturnCode_t r = A.dispatch(&u->ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int told;
        if (told++ < 8) rt_log("ffx: FSR 3.1 dispatch failed (%u)", (unsigned)r);
        return 0;
    }
    return 1;
}

extern "C" ffxReturnCode_t vk_ffx_create(ffxContext *ctx, ffxCreateContextDescHeader *desc) {
    return vk_ffx_load() ? A.create(ctx, desc, nullptr) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx_destroy(ffxContext *ctx) { return A.ok ? A.destroy(ctx, nullptr) : FFX_API_RETURN_ERROR; }
extern "C" ffxReturnCode_t vk_ffx_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc) {
    return A.ok ? A.configure(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx_query(ffxContext *ctx, ffxQueryDescHeader *desc) {
    return A.ok ? A.query(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc) {
    return A.ok ? A.dispatch(ctx, desc) : FFX_API_RETURN_ERROR;
}

extern "C" void vk_ffx_release(void) {
    if (!A.ok) return;
    for (Up &u : U)
        if (u.ctx) A.destroy(&u.ctx, nullptr), u.ctx = nullptr;
}
