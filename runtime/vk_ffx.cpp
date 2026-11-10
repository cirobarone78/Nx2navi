// AMD FidelityFX / FSR through the AMD FSR API, loaded at run time: temporal upscaling here, in the place of DLSS in
// vk_dlaa.c, and frame generation (vk_fg.cpp, through vk_ffx_create and the rest). The API's headers are in
// third_party/ffx_sdk24 (AMD FSR SDK 2.4.0, MIT); they are C++ in places, hence this file. Nothing of it runs unless the
// upscaler or frame generation asks for FSR.
//
// Two libraries can serve it, both found next to the executable:
// - the AMD FSR SDK 2 loader, amd_fidelityfx_loader_vk.dll, which loads amd_fidelityfx_upscaler_vk.dll and
//   amd_fidelityfx_framegeneration_vk.dll from beside it and offers each effect in the versions this GPU runs: FSR 4
//   upscaling (machine learning; FP8 on Radeon RX 9000 cards, INT8 on RX 7000 cards) and FSR Frame Generation 4 on
//   RX 9000 cards, FSR 3.1.x elsewhere. Its contexts take SDK 2's back end description (a VkDeviceContext) and a version
//   structure;
// - the FSR 3.1 library, amd_fidelityfx_vk.dll, as before, with the deprecated back end description.
// The loader is used when it and both effect libraries are there, unless nx2navi.ini has ffx_sdk2 = 0. ffx_upscaler and
// ffx_framegen (for example "3.1" or "4") pick one of the versions the loader offers by the start of its name; unset,
// the API's default for the GPU.
//
// The upscaler takes the same inputs vk_dlaa.c makes for DLSS: the scene's light target (linear HDR), its D32F depth
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

#include "third_party/ffx_sdk24/api/include/ffx_api.h"
#include "third_party/ffx_sdk24/upscalers/include/ffx_upscale.h"
#include "third_party/ffx_sdk24/api/include/vk/ffx_api_vk.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_ffx.h"
#include "vk_dlaa.h"
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
    int sdk2 = 0;   // the FSR SDK 2 loader is the library in use
} A;

// What the settings menu may ask before anything has used FSR (vk_ffx_available), from any thread.
std::atomic<int> g_loaded{-1};    // what vk_ffx_load found: 1 or 0; -1 while it has not run
std::atomic<int> g_present{-1};   // whether a library vk_ffx_load takes is there: 1 or 0; -1 while not asked

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
    const char *lib = A.sdk2 ? "AMD FSR SDK 2" : "FSR 3.1";
    if (!slot && lru) {
        static int told;
        if (told++ < 20)
            rt_log("ffx: %s context %ux%u -> %ux%u destroyed for %ux%u -> %ux%u (all %d in use)", lib, lru->w, lru->h,
                   lru->ow, lru->oh, w, h, ow, oh, (int)(sizeof U / sizeof U[0]));
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();   // its work may still be in flight
        A.destroy(&lru->ctx, nullptr);
        *lru = Up();
        slot = lru;
    }
    if (!slot) return nullptr;
    VkFfxBackend be;
    ffxCreateContextDescUpscale d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = vk_ffx_backend(&be);
    d.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    d.maxRenderSize = {w, h};
    d.maxUpscaleSize = {ow, oh};
    d.fpMessage = message;
    // SDK 2: the version the integration was built against (required since SDK 2.1), and the user's pick if any
    ffxCreateContextDescUpscaleVersion ver = {};
    ffxOverrideVersion ov = {};
    if (A.sdk2) {
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        ver.version = FFX_UPSCALER_VERSION;
        ver.header.pNext = d.header.pNext;
        d.header.pNext = &ver.header;
        if (vk_ffx_version(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, "upscaler", cfg_get("NX2NAVI_FFX_UPSCALER"), &ov)) {
            ov.header.pNext = d.header.pNext;
            d.header.pNext = &ov.header;
        }
    }
    ffxReturnCode_t r = A.create(&slot->ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        rt_log("ffx: %s context creation at %ux%u -> %ux%u failed (%u)", lib, w, h, ow, oh, (unsigned)r);
        slot->ctx = nullptr;
        return nullptr;
    }
    slot->w = w, slot->h = h, slot->ow = ow, slot->oh = oh, slot->used = up_tick;
    char pv[96];
    rt_log("ffx: %s upscaling at %ux%u -> %ux%u (%s)", lib, w, h, ow, oh, vk_ffx_provider(&slot->ctx, pv, sizeof pv));
    return slot;
}

#ifdef _WIN32
// The executable's folder with its trailing backslash, in path (MAX_PATH); 0 when unknown.
int exe_dir(char *path) {
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    while (n && path[n - 1] != '\\') n--;
    path[n] = 0;
    return n != 0;
}

int bind(HMODULE m) {
    A.create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    A.query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    return A.create && A.destroy && A.configure && A.query && A.dispatch;
}

// The loader and both effect libraries next to the executable (a loader alone, without its effects, would leave
// FSR off instead of falling back to the FSR 3.1 library), and not switched off in nx2navi.ini.
int sdk2_wanted_now(void) {
    const char *e = cfg_get("NX2NAVI_FFX_SDK2");
    if (e && !strcmp(e, "0")) return 0;
    char dir[MAX_PATH];
    if (!exe_dir(dir)) return 0;
    static const char *const need[] = {"amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll",
                                       "amd_fidelityfx_framegeneration_vk.dll"};
    for (const char *n : need) {
        char path[MAX_PATH + 64];
        snprintf(path, sizeof path, "%s%s", dir, n);
        DWORD a = GetFileAttributesA(path);
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    }
    return 1;
}
#endif

}  // namespace

// Decided once, before the device exists (vk_device.c); the settings menu asks again from its own thread
// (vk_ffx_available).
extern "C" int vk_ffx_sdk2_wanted(void) {
#ifdef _WIN32
    static std::atomic<int> v{-1};
    int r = v.load(std::memory_order_acquire);
    if (r < 0) {
        r = sdk2_wanted_now();
        v.store(r, std::memory_order_release);
    }
    return r;
#else
    return 0;
#endif
}

extern "C" int vk_ffx_sdk2(void) { return A.ok && A.sdk2; }

extern "C" int vk_ffx_load(void) {
    if (A.tried) return A.ok;
    A.tried = 1;
#ifdef _WIN32
    if (vk_ffx_sdk2_wanted()) {
        char dir[MAX_PATH], sdk2[MAX_PATH + 64];
        if (!exe_dir(dir)) dir[0] = 0;
        snprintf(sdk2, sizeof sdk2, "%samd_fidelityfx_loader_vk.dll", dir);
        HMODULE m = LoadLibraryA(sdk2);
        if (m && bind(m)) {
            A.lib = m, A.sdk2 = 1, A.ok = 1;
            rt_log("ffx: AMD FSR SDK 2 loader amd_fidelityfx_loader_vk.dll loaded (FSR 4 and FSR Frame Generation 4 "
                   "where this GPU has them)");
            g_loaded.store(1, std::memory_order_release);
            return 1;
        }
        rt_log("ffx: %s %s: the FSR 3.1 library instead", sdk2, m ? "lacks the API entry points" : "did not load");
        if (m) FreeLibrary(m);
        A.create = nullptr, A.destroy = nullptr, A.configure = nullptr, A.query = nullptr, A.dispatch = nullptr;
    }
    char path[MAX_PATH];
    library_path(path, sizeof path);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        rt_log("ffx: %s not found: FSR 3.1 off", path);
        g_loaded.store(0, std::memory_order_release);
        return 0;
    }
    A.lib = m;
    A.ok = bind(m);
    rt_log("ffx: amd_fidelityfx_vk.dll %s", A.ok ? "loaded" : "lacks the API entry points: FSR 3.1 off");
#else
    // Only the Windows build loads AMD's library: there is nothing to load here
    rt_log("ffx: this build has no FSR 3.1 (it needs amd_fidelityfx_vk.dll, which only the Windows build loads): FSR 3.1 off");
#endif
    g_loaded.store(A.ok, std::memory_order_release);
    return A.ok;
}

ffxApiHeader *vk_ffx_backend(VkFfxBackend *b) {
    const VkCtx *c = vk_ctx();
    memset(b, 0, sizeof *b);
    if (A.sdk2) {
        b->sdk2.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        b->sdk2.deviceContext.vkDevice = c->device;
        b->sdk2.deviceContext.vkPhysicalDevice = c->phys;
        b->sdk2.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
        return &b->sdk2.header;
    }
    b->v31.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
    b->v31.vkDevice = c->device;
    b->v31.vkPhysicalDevice = c->phys;
    b->v31.vkDeviceProcAddr = vkGetDeviceProcAddr;
    return &b->v31.header;
}

int vk_ffx_version(uint64_t type, const char *what, const char *want, ffxOverrideVersion *o) {
    if (!A.ok || !A.sdk2) return 0;
    const VkCtx *c = vk_ctx();
    VkDeviceContext dc = {c->device, c->phys, vkGetDeviceProcAddr};
    uint64_t count = 16, ids[16] = {};
    const char *names[16] = {};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = type;
    q.device = &dc;
    q.outputCount = &count;
    q.versionIds = ids;
    q.versionNames = names;
    static uint64_t logged[8];
    static int n_logged;
    int seen = 0;
    for (int i = 0; i < n_logged; i++) seen |= logged[i] == type;
    if (A.query(nullptr, &q.header) != FFX_API_RETURN_OK) {
        if (!seen && n_logged < 8) logged[n_logged++] = type, rt_log("ffx: %s: the version query failed", what);
        return 0;
    }
    if (count > 16) count = 16;
    // the names may live in memory a later query overwrites (the SDK's documentation): copied at once
    char copy[16][32], list[16 * 34] = "";
    for (uint64_t i = 0; i < count; i++) {
        snprintf(copy[i], sizeof copy[i], "%s", names[i] ? names[i] : "?");
        if (i) strcat(list, ", ");
        strcat(list, copy[i]);
    }
    if (!seen && n_logged < 8) logged[n_logged++] = type, rt_log("ffx: %s versions offered: %s", what, count ? list : "none");
    if (!want || !*want || !strcmp(want, "default") || !strcmp(want, "auto")) return 0;
    size_t wl = strlen(want);
    for (uint64_t i = 0; i < count; i++)
        if (!strncmp(copy[i], want, wl)) {
            memset(o, 0, sizeof *o);
            o->header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            o->versionId = ids[i];
            if (!seen) rt_log("ffx: %s %s chosen (nx2navi.ini asks for %s)", what, copy[i], want);
            return 1;
        }
    if (!seen) rt_log("ffx: %s: no version offered starts with '%s': the API's default", what, want);
    return 0;
}

const char *vk_ffx_provider(ffxContext *ctx, char *buf, size_t n) {
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (A.ok && A.query(ctx, &v.header) == FFX_API_RETURN_OK && v.versionName) snprintf(buf, n, "%s", v.versionName);
    else snprintf(buf, n, "provider unknown");
    return buf;
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
        // vk_ffx_load takes the FSR SDK 2 loader (with both effect libraries) or the FSR 3.1 library: either will do
        present = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) || vk_ffx_sdk2_wanted();
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
    // SDK 2: the camera's own field of view when the caller has none (vk_dlaa.c fg_camera reads it each frame)
    if (A.sdk2 && fov_y <= 0) {
        VkDlaaCamera cam;
        if (vk_dlaa_fg_camera(&cam) && cam.fov_y > 0.05f && cam.fov_y < 3.0f) fov_y = cam.fov_y;
    }
    d.cameraFovAngleVertical = fov_y > 0 ? fov_y : 1.0f;
    d.viewSpaceToMetersFactor = 0.01f;   // the game's units are centimetres
    ffxReturnCode_t r = A.dispatch(&u->ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int told;
        if (told++ < 8) rt_log("ffx: upscaling dispatch failed (%u)", (unsigned)r);
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
