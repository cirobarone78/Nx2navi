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
#include "platform/platform.h"
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
} A, A4;

// What the settings menu may ask before anything has used FSR 3.1 / FSR 4, from any thread.
std::atomic<int> g_loaded{-1}, g_present{-1};
std::atomic<int> g_loaded4{-1}, g_present4{-1};

#ifdef _WIN32
#define FFX_LIBRARY "amd_fidelityfx_vk.dll"
#define FFX_FSR4_LIBRARY NULL
// amd_fidelityfx_vk.dll next to the executable
void library_path(const char *lib_name, char *path, size_t size) {
    if (!lib_name) { path[0] = 0; return; }
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
    while (n && path[n - 1] != '\\') n--;
    snprintf(path + n, size - n, "%s", lib_name);
}
#else
// amd_fidelityfx_upscaler_vk.so (FSR 4, from fsr4-vk) next to the executable; FSR 3.1 is Windows-only on main.
#define FFX_LIBRARY NULL
#define FFX_FSR4_LIBRARY "amd_fidelityfx_upscaler_vk.so"
void library_path(const char *lib_name, char *path, size_t size) {
    if (!lib_name || size <= 64) { path[0] = 0; return; }
    char *cut = plat_exe_path(path, size - 64) ? plat_last_sep(path) : NULL;
    if (cut) snprintf(cut + 1, size - (size_t)(cut + 1 - path), "%s", lib_name);
    else path[0] = 0;
}
#endif

int load_api(Api *api, const char *lib_name, const char *label, std::atomic<int> *loaded_flag) {
    if (api->tried) return api->ok;
    api->tried = 1;
    if (!lib_name) {
        rt_log("ffx: no %s library in this build: %s off", label, label);
        loaded_flag->store(0, std::memory_order_release);
        return 0;
    }
#ifdef _WIN32
    char path[MAX_PATH];
    library_path(lib_name, path, sizeof path);
    HMODULE m = path[0] ? LoadLibraryA(path) : NULL;
    if (!m) {
        rt_log("ffx: %s not found: %s off", path, label);
        loaded_flag->store(0, std::memory_order_release);
        return 0;
    }
    api->lib = m;
    api->create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    api->destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    api->configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    api->query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    api->dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    api->ok = api->create && api->destroy && api->configure && api->query && api->dispatch;
#else
    char path[4096];
    library_path(lib_name, path, sizeof path);
    void *m = path[0] ? plat_lib_open(path) : NULL;
    if (!m) {
        rt_log("ffx: %s not found: %s off", path[0] ? path : lib_name, label);
        loaded_flag->store(0, std::memory_order_release);
        return 0;
    }
    api->lib = m;
    api->create = (PfnFfxCreateContext)plat_lib_sym(m, "ffxCreateContext");
    api->destroy = (PfnFfxDestroyContext)plat_lib_sym(m, "ffxDestroyContext");
    api->configure = (PfnFfxConfigure)plat_lib_sym(m, "ffxConfigure");
    api->query = (PfnFfxQuery)plat_lib_sym(m, "ffxQuery");
    api->dispatch = (PfnFfxDispatch)plat_lib_sym(m, "ffxDispatch");
    api->ok = api->create && api->destroy && api->configure && api->query && api->dispatch;
#endif
    rt_log("ffx: %s %s", lib_name, api->ok ? "loaded" : "lacks the API entry points: off");
    loaded_flag->store(api->ok, std::memory_order_release);
    return api->ok;
}

int file_available(std::atomic<int> *loaded_flag, std::atomic<int> *present_flag, const char *lib_name) {
    if (!lib_name) return 0;
    int loaded = loaded_flag->load(std::memory_order_acquire);
    if (loaded >= 0) return loaded;
    int present = present_flag->load(std::memory_order_relaxed);
    if (present < 0) {
#ifdef _WIN32
        char path[MAX_PATH];
        library_path(lib_name, path, sizeof path);
        DWORD attr = path[0] ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
        present = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
        char path[4096];
        library_path(lib_name, path, sizeof path);
        present = path[0] && plat_file_exists(path);
#endif
        present_flag->store(present, std::memory_order_relaxed);
    }
    return present;
}

VkImageAspectFlags image_aspect(const VkFfxImage *i) {
    if (i->format == VK_FORMAT_D32_SFLOAT || i->format == VK_FORMAT_D16_UNORM || i->format == VK_FORMAT_X8_D24_UNORM_PACK32)
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (i->format == VK_FORMAT_D32_SFLOAT_S8_UINT || i->format == VK_FORMAT_D24_UNORM_S8_UINT || i->format == VK_FORMAT_D16_UNORM_S8_UINT)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

void transition_inputs(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv, int to_read) {
    const VkFfxImage *imgs[3] = {color, depth, mv};
    VkImageMemoryBarrier b[3] = {};
    for (int k = 0; k < 3; k++) {
        b[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[k].oldLayout = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[k].newLayout = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        b[k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[k].image = imgs[k]->image;
        b[k].subresourceRange = {image_aspect(imgs[k]), 0, 1, 0, 1};
        if (to_read) {
            b[k].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            b[k].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        } else {
            b[k].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b[k].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
    }
    vkCmdPipelineBarrier(cb, to_read ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         to_read ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 3, b);
}

void message(uint32_t type, const wchar_t *msg) {
    char s[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof s - 1; n++) s[n] = msg[n] < 128 ? (char)msg[n] : '?';
    s[n] = 0;
    static int told;
    if (told++ < 40) rt_log("ffx: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error:" : "warning:", s);
}

// One upscaling context per backend (FSR 3.1 vs FSR 4), render size and output size. When all are taken, the least
// recently used one is destroyed for the new size once the GPU is idle, as vk_dlaa.c free_size does for DLSS.
struct Up {
    ffxContext ctx = nullptr;
    uint32_t w = 0, h = 0, ow = 0, oh = 0;
    uint64_t used = 0;   // up_tick when context_for last returned it
    int is_fsr4 = 0;
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

Up *context_for(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int fsr4) {
    up_tick++;
    for (Up &u : U)
        if (u.ctx && u.w == w && u.h == h && u.ow == ow && u.oh == oh && u.is_fsr4 == fsr4) {
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
            rt_log("ffx: %s context %ux%u -> %ux%u destroyed for %ux%u -> %ux%u (all %d in use)", lru->is_fsr4 ? "FSR 4" : "FSR 3.1",
                   lru->w, lru->h, lru->ow, lru->oh, w, h, ow, oh, (int)(sizeof U / sizeof U[0]));
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();   // its work may still be in flight
        (lru->is_fsr4 ? A4 : A).destroy(&lru->ctx, nullptr);
        *lru = Up();
        slot = lru;
    }
    if (!slot) return nullptr;
    Api &api = fsr4 ? A4 : A;
    const char *label = fsr4 ? "FSR 4" : "FSR 3.1";
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
    ffxReturnCode_t r = api.create(&slot->ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        rt_log("ffx: %s context creation at %ux%u -> %ux%u failed (%u)", label, w, h, ow, oh, (unsigned)r);
        slot->ctx = nullptr;
        return nullptr;
    }
    slot->w = w, slot->h = h, slot->ow = ow, slot->oh = oh, slot->used = up_tick, slot->is_fsr4 = fsr4;
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    api.query(&slot->ctx, &v.header);
    rt_log("ffx: %s upscaling at %ux%u -> %ux%u (%s)", label, w, h, ow, oh, v.versionName ? v.versionName : "provider unknown");
    return slot;
}

}  // namespace

extern "C" int vk_ffx_load(void) { return load_api(&A, FFX_LIBRARY, "FSR 3.1", &g_loaded); }

extern "C" int vk_ffx_load_fsr4(void) {
    const char *lib4 = FFX_FSR4_LIBRARY;
    if (!lib4) {
        A4.tried = 1, A4.ok = 0;
        g_loaded4.store(0, std::memory_order_release);
        return 0;
    }
    if (!A4.tried && vk_ctx()->device && !vk_ctx()->has_descriptor_buffer) {
        A4.tried = 1, A4.ok = 0;
        g_loaded4.store(0, std::memory_order_release);
        rt_log("ffx: VK_EXT_descriptor_buffer unavailable: FSR 4 off");
        return 0;
    }
    return load_api(&A4, lib4, "FSR 4", &g_loaded4);
}

extern "C" int vk_ffx_available(void) { return file_available(&g_loaded, &g_present, FFX_LIBRARY); }

extern "C" int vk_ffx_fsr4_available(void) {
    if (vk_ctx()->device && !vk_ctx()->has_descriptor_buffer) return 0;
    return file_available(&g_loaded4, &g_present4, FFX_FSR4_LIBRARY);
}

extern "C" int vk_ffx_framegen_available(void) { return vk_ffx_available(); }   // the same library, its API calls

extern "C" int vk_ffx_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                              const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, float fov_y, int fsr4) {
    if (fsr4 ? !vk_ffx_load_fsr4() : !vk_ffx_load()) return 0;
    Up *u = context_for(color->width, color->height, out->width, out->height, fsr4);
    if (!u) return 0;
    if (fsr4) transition_inputs(cb, color, depth, mv, 1);
    const uint32_t in_state = fsr4 ? FFX_API_RESOURCE_STATE_COMPUTE_READ : FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    // FSR 3.1 reads GENERAL (unordered-access); FSR 4 (fsr4-vk) reads SHADER_READ_ONLY_OPTIMAL (compute read).
    d.color = image(color, in_state, 0);
    d.depth = image(depth, in_state, 0);
    d.motionVectors = image(mv, in_state, 0);
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
    ffxReturnCode_t r = (fsr4 ? A4 : A).dispatch(&u->ctx, &d.header);
    if (fsr4) transition_inputs(cb, color, depth, mv, 0);
    if (r != FFX_API_RETURN_OK) {
        static int told;
        if (told++ < 8) rt_log("ffx: %s dispatch failed (%u)", fsr4 ? "FSR 4" : "FSR 3.1", (unsigned)r);
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
    for (Up &u : U)
        if (u.ctx) {
            Api &api = u.is_fsr4 ? A4 : A;
            if (api.ok) api.destroy(&u.ctx, nullptr);
            u.ctx = nullptr;
        }
}
