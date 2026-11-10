// Frame generation with AMD FSR (NX2NAVI_FRAMEGEN = fsr3, followed during a run: vk_fg_wanted): AMD's frame generation
// swapchain (vk_ffx.cpp loads the library) takes the place of gfx_present.c's swapchain. Every frame the game presents,
// the swapchain presents a generated frame between it and the one before, on its own present queue and pacing threads,
// so the screen shows twice the game's frame rate.
//
// With the AMD FSR SDK 2 loader (vk_ffx_sdk2) the API picks AMD FSR Frame Generation 4.0 (machine learning) and its
// swapchain 4.0 on RDNA 4, FSR 3.1.x elsewhere. SDK 2 differs from the FSR 3.1 library in these places, handled here:
// - each context creation chains a version structure (required since SDK 2.1); the frame generation context also SDK 2's
//   back end description (vk_ffx_backend), while the swapchain context's own description carries the device;
// - the preparation is ffxDispatchDescFrameGenerationPrepareV2, which carries the camera (position and axes in world
//   space, vk_dlaa.c fg_camera) that Frame Generation 4 requires, and its reset (vk_dlaa_fg_reset: the upscaler's
//   history restarted); configure, prepare, generate run in that order;
// - the VkSwapchainKHR handle is AMD's swapchain object itself. A new size recreates it in place
//   (pOutCreateSwapchainFFXAPI with that handle); it is destroyed only after its context (pOutDestroySwapchainFFXAPI).
//   Before the generation context goes or the swapchain is recreated, generation is switched off in it (AMD's order:
//   its presenter waits for the presents in flight and stops calling into the context). gfx_present.c's calls keep
//   their order: vk_fg_destroy_swapchain only notes the handle, which the next vk_fg_create_swapchain recreates in
//   place or vk_fg_release destroys after the contexts. An in-place recreation keeps AMD's counts of the images
//   acquired and presented, and at the same size the images' old format, so everything is taken down and a new
//   swapchain object made instead when an acquired image was never presented (the present code's recovery after a
//   failed submit or acquire), for another format or colour space, and after a failed recreation.
//
// The generated frame is built from the presented picture and the scene's depth and camera motion vectors, which
// vk_dlaa.c makes for DLSS and FSR upscaling: frame generation needs one of those on (DLAA or Native AA included).
// The HUD is interpolated with the picture (it hardly moves); the menu is not, since generation pauses while it is
// open. The device creates two more queues of the graphics family for it (vk_device.c, vk_fg_queue_count).
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "third_party/ffx_sdk24/api/include/ffx_api.h"
#include "third_party/ffx_sdk24/framegeneration/include/ffx_framegeneration.h"
#include "third_party/ffx_sdk24/framegeneration/include/vk/ffx_api_framegeneration_vk.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_ffx.h"
#include "vk_fg.h"
#include "ui/menu.h"
}

namespace {

struct {
    VkQueue present = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE;
    ffxContext sc = nullptr;   // the swapchain context
    ffxContext fg = nullptr;   // the frame generation context
    VkExtent2D fg_ext = {0, 0};
    VkExtent2D fg_max = {0, 0};   // the context's largest render size
    VkFormat fg_fmt = VK_FORMAT_UNDEFINED;
    ffxQueryDescSwapchainReplacementFunctionsVK fn = {};
    uint64_t frame = 0;
    int failed = 0;
    uint64_t generated = 0, paused = 0, no_inputs = 0, hudless = 0, no_camera = 0;
    int on = 0;   // the last configured frame had generation on (vk_fg_generating)
    // SDK 2 (sdk2): AMD's swapchain object (cur) with the format and colour space it was made for, one gfx_present.c
    // gave up (pending: recreated in place by the next creation, or destroyed after the contexts), the last camera for a
    // frame that has none, and a history restart not yet handed to a preparation
    int sdk2 = 0;
    VkSwapchainKHR cur = VK_NULL_HANDLE, pending = VK_NULL_HANDLE;
    VkFormat sc_fmt = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR sc_cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkDlaaCamera cam = {};
    int have_cam = 0, reset = 0;
    // SDK 2: the images acquired from AMD's swapchain object and presented to it since it was made (vk_fg_acquire,
    // vk_fg_present, both under gfx_present.c's lock): equal when no acquired image waits for its present
    uint64_t acquired = 0, presented = 0;
} F;

VkResult submit_game(uint32_t n, const VkSubmitInfo *si, VkFence fence) {
    vk_queue_lock();   // the game's queue is shared with the recorder and the present path
    VkResult r = vkQueueSubmit(vk_ctx()->gfx_queue, n, si, fence);
    vk_queue_unlock();
    return r;
}

ffxReturnCode_t generate(ffxDispatchDescFrameGeneration *d, void *user) {
    ffxContext *ctx = (ffxContext *)user;
    if (!ctx || !*ctx) {   // no context (any more): nothing generated, so the swapchain presents no stale output
        d->numGeneratedFrames = 0;
        return FFX_API_RETURN_ERROR;
    }
    F.generated++;
    ffxReturnCode_t r = vk_ffx_dispatch(ctx, &d->header);
    if (r != FFX_API_RETURN_OK) d->numGeneratedFrames = 0;
    return r;
}

FfxApiResource image(const VkFfxImage *i) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i->format;
    ci.extent = {i->width, i->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    return ffxApiGetResourceVK((void *)i->image, ffxApiGetImageResourceDescriptionVK(i->image, ci, 0),
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

// SDK 2: generation off in AMD's swapchain swc (no callbacks, no HUD-less picture). Its presenter waits for the
// presents in flight and stops; the next configuration with generation on starts it again.
void fg_disable(VkSwapchainKHR swc) {
    __atomic_store_n(&F.on, 0, __ATOMIC_RELAXED);
    if (!F.fg || !swc) return;
    ffxConfigureDescFrameGeneration cfg = {};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void *)swc;
    cfg.frameGenerationEnabled = false;
    cfg.frameID = ++F.frame;
    vk_ffx_configure(&F.fg, &cfg.header);
}

// SDK 2: everything down in AMD's order: generation off, the generation context, the swapchain context, then AMD's
// swapchain object (the one gfx_present.c gave up, or the current one).
void sdk2_teardown(void) {
    VkSwapchainKHR obj = F.pending ? F.pending : F.cur;
    fg_disable(obj);
    if (F.fg) vk_ffx_destroy(&F.fg), F.fg = nullptr;
    if (F.sc) vk_ffx_destroy(&F.sc), F.sc = nullptr;
    if (obj && F.fn.pOutDestroySwapchainFFXAPI) F.fn.pOutDestroySwapchainFFXAPI(vk_ctx()->device, obj, nullptr, nullptr);
    F.pending = F.cur = VK_NULL_HANDLE;
    F.have_cam = 0, F.reset = 0, F.acquired = F.presented = 0;
}

void fail(const char *what, unsigned r) {
    rt_log("fg: %s failed (%u): frame generation off for this run", what, r);
    F.failed = 1;
    vk_dlaa_hudless_wanted = 0;   // nothing reads the HUD-less copy any more: vk_dlaa.c stops making it
    // SDK 2: generation off in AMD's swapchain at once. Presents still go through it (vk_fg_active) until the present
    // code replaces it with a plain one (vk_fg_wanted is 0 after a failure), and vk_fg_release takes the rest down.
    if (F.sdk2) fg_disable(F.cur);
    else __atomic_store_n(&F.on, 0, __ATOMIC_RELAXED);
}

}  // namespace

// Follows the setting: gfx_present.c's window_settings_live remakes the swapchain when this stops matching vk_fg_active.
// It needs DLSS's or FSR's motion vectors, so it also needs one of them on, and a library FSR runs through
// (vk_ffx_framegen_available: the FSR 3.1 library, or the FSR SDK 2 loader with both effect libraries): a build without
// one keeps the plain swapchain whatever the setting says, with one line in the log. Not on a device made through
// Streamline's proxies (NVIDIA's frame generation set at the start, vk_dlssg_dpa): AMD's swapchain there crashed in
// NVIDIA's driver when the menu switched from NVIDIA's frame generation to AMD's. That switch waits for a restart,
// as the menu says (menu.cpp row_framegen). Not after a failure (fail): the present code then takes AMD's swapchain
// down as it does for a switch in the menu, and a plain one follows.
extern "C" int vk_fg_wanted(void) {
    if (F.failed || strcmp(cfg_value("NX2NAVI_FRAMEGEN"), "fsr3") || !cfg_bool("NX2NAVI_DLAA") || !vk_dlaa_wanted() ||
        vk_dlssg_dpa())
        return 0;
    if (vk_ffx_framegen_available()) return 1;
    static int told;
    if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
        rt_log("fg: AMD FSR 3.1 frame generation is set, but FSR 3.1 is not available here: off");
    return 0;
}

// Always the three queues: the setting can be switched on during a run, and idle queues cost nothing.
extern "C" uint32_t vk_fg_queue_count(void) { return 3; }

extern "C" void vk_fg_queues(VkQueue present, VkQueue acquire) { F.present = present, F.acquire = acquire; }

// AMD's swapchain is in place: the window's swapchain handle is AMD's object, and every swapchain call has to go
// through AMD's functions. That holds after a failure too, until the present code has replaced the swapchain
// (vk_fg_wanted): the plain Vulkan calls must never get that handle.
extern "C" int vk_fg_active(void) { return F.sc != nullptr; }

extern "C" int vk_fg_generating(void) { return vk_fg_active() && !F.failed && __atomic_load_n(&F.on, __ATOMIC_RELAXED); }

extern "C" VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    if (F.sc && !F.sdk2) return F.fn.pOutCreateSwapchainFFXAPI(vk_ctx()->device, ci, nullptr, sc, F.sc);
    if (F.sc) {   // SDK 2: the same swapchain object again, made for the new size or mode
        VkSwapchainKHR obj = F.pending ? F.pending : F.cur;
        F.pending = VK_NULL_HANDLE, F.cur = obj;
        if (obj && ci->imageFormat == F.sc_fmt && ci->imageColorSpace == F.sc_cs && F.acquired == F.presented) {
            fg_disable(obj);   // the next vk_fg_frame switches generation on again
            VkSwapchainKHR out = obj;
            VkResult r = F.fn.pOutCreateSwapchainFFXAPI(vk_ctx()->device, ci, nullptr, &out, (void *)obj);
            if (r == VK_SUCCESS) {
                F.cur = out, *sc = out;
                return VK_SUCCESS;
            }
            rt_log("fg: AMD's swapchain could not be recreated at %ux%u (%d): made anew", ci->imageExtent.width,
                   ci->imageExtent.height, (int)r);
        } else if (obj && F.acquired != F.presented) {
            // its counts would stay one image apart for good: the image presented a frame late, generation given the
            // previous frame's picture, and in the end every acquire refused
            rt_log("fg: an image acquired from AMD's swapchain was never presented: AMD's swapchain made anew");
        } else if (obj) {
            rt_log("fg: another swapchain format or colour space: AMD's swapchain made anew");
        }
        sdk2_teardown();   // then a new one below, as at the first creation
    }
    if (!vk_fg_wanted() || F.failed || !F.present || !vk_ffx_load()) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkCtx *c = vk_ctx();
    ffxCreateContextDescFrameGenerationSwapChainVK d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    d.physicalDevice = c->phys;
    d.device = c->device;
    VkSwapchainKHR none = VK_NULL_HANDLE;
    d.swapchain = &none;
    d.createInfo = *ci;
    d.createInfo.oldSwapchain = VK_NULL_HANDLE;
    d.gameQueue = {c->gfx_queue, c->gfx_family, submit_game};
    d.asyncComputeQueue = {VK_NULL_HANDLE, c->gfx_family, nullptr};
    d.presentQueue = {F.present, c->gfx_family, nullptr};
    d.imageAcquireQueue = {F.acquire, c->gfx_family, nullptr};
    F.sdk2 = vk_ffx_sdk2();
    ffxCreateContextDescFrameGenerationSwapChainVersionVK ver = {};
    if (F.sdk2) {
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
        ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
        d.header.pNext = &ver.header;
    }
    ffxReturnCode_t r = vk_ffx_create(&F.sc, &d.header);
    if (r != FFX_API_RETURN_OK) {
        F.sc = nullptr;
        fail("the frame generation swapchain", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    F.cur = none, F.pending = VK_NULL_HANDLE, F.acquired = F.presented = 0;
    F.sc_fmt = ci->imageFormat, F.sc_cs = ci->imageColorSpace;
    F.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (vk_ffx_query(&F.sc, &F.fn.header) != FFX_API_RETURN_OK || !F.fn.pOutQueuePresentKHR) {
        // with the context goes the swapchain it made: nothing was handed out yet. SDK 2's swapchain object outlives
        // its context and goes through AMD's own call, when the query got that far.
        vk_ffx_destroy(&F.sc), F.sc = nullptr;
        if (F.sdk2 && none && F.fn.pOutDestroySwapchainFFXAPI)
            F.fn.pOutDestroySwapchainFFXAPI(c->device, none, nullptr, nullptr);
        F.cur = VK_NULL_HANDLE;
        fail("the swapchain functions", 0);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *sc = none;
    char pv[96];
    rt_log("fg: AMD %s frame generation swapchain (%s), %ux%u", F.sdk2 ? "FSR SDK 2" : "FSR 3.1",
           vk_ffx_provider(&F.sc, pv, sizeof pv), ci->imageExtent.width, ci->imageExtent.height);
    return VK_SUCCESS;
}

extern "C" void vk_fg_destroy_swapchain(VkSwapchainKHR sc) {
    if (!F.sc || !sc) return;
    if (F.sdk2) {   // recreated in place (vk_fg_create_swapchain), or destroyed after the contexts (vk_fg_release)
        F.pending = sc;
        return;
    }
    F.fn.pOutDestroySwapchainFFXAPI(vk_ctx()->device, sc, nullptr, F.sc);
}

extern "C" VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img) {
    return F.fn.pOutGetSwapchainImagesKHR(vk_ctx()->device, sc, n, img);
}

// Both counted whatever they return (vk_fg_create_swapchain compares the counts). AMD's swapchain counts an acquire even
// when its own submission fails, and counting one it did not costs only a new swapchain object at the next recreation.
extern "C" VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx) {
    F.acquired++;
    return F.fn.pOutAcquireNextImageKHR(vk_ctx()->device, sc, timeout, sem, VK_NULL_HANDLE, idx);
}

extern "C" VkResult vk_fg_present(const VkPresentInfoKHR *pi) {
    F.presented++;
    return F.fn.pOutQueuePresentKHR(vk_ctx()->gfx_queue, pi);
}

extern "C" uint64_t vk_fg_presents(VkSwapchainKHR sc) {
    return vk_fg_active() && F.fn.pOutGetLastPresentCountFFXAPI && sc ? F.fn.pOutGetLastPresentCountFFXAPI(sc) : 0;
}

// The HDR10 metadata goes to AMD's swapchain through its own call (vk_hdr.c vk_hdr_metadata).
extern "C" PFN_vkSetHdrMetadataEXT vk_fg_hdr_metadata_fn(void) {
    return vk_fg_active() ? F.fn.pOutSetHdrMetadataEXT : nullptr;
}

// An HDR10 swapchain (10-bit PQ or 16-bit float): frame generation is told its inputs are HDR.
static bool hdr_format(VkFormat f) {
    return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
           f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

extern "C" void vk_fg_frame(VkCommandBuffer cb, VkSwapchainKHR sc, VkFormat fmt, VkExtent2D ext, VkImage hudless,
                            VkImageView hudless_view, const int pic[4]) {
    (void)hudless_view;
    if (!vk_fg_active() || F.failed) return;
    vk_dlaa_hudless_wanted = 1;   // the upscaling pass keeps a copy of the picture before the HUD (vk_dlaa.c hudless)
    const VkCtx *c = vk_ctx();
    VkFfxImage depth, mv;
    float jx = 0, jy = 0;
    int reset = 0;
    int have = vk_dlaa_fg_inputs(&depth, &mv, &jx, &jy, &reset);
    // SDK 2: Frame Generation 4 requires the camera. A frame without one takes the last frame's; before any camera
    // the frame is not generated. A restarted upscaler history (a cut) restarts generation's too, at the next preparation.
    VkDlaaCamera cam = {};
    if (F.sdk2) {
        F.reset |= vk_dlaa_fg_reset() | reset;
        if (have) {
            if (vk_dlaa_fg_camera(&cam)) F.cam = cam, F.have_cam = 1;
            else if (F.have_cam) cam = F.cam, F.no_camera++;
            else have = 0, F.no_camera++;
        }
    }
    // The context's largest render size covers the depth and motion vectors it is given. They have the scene's size,
    // which DLAA and the widescreen pictures make larger than the window, and a context made for the window's size alone
    // wrote past its own resources: a crash in amd_fidelityfx_vk.dll, device faults, striped frames. The scene is never
    // larger than the picture (widescreen.c wide_picture), so the context is made for that once; remaking it while the
    // swapchain still generated from the old one hung the present. A frame with a larger depth anyway is not generated.
    unsigned pw, ph;
    wide_picture(&pw, &ph);
    VkExtent2D max_render = {ext.width > pw ? ext.width : pw, ext.height > ph ? ext.height : ph};
    if (F.fg && (F.fg_ext.width != ext.width || F.fg_ext.height != ext.height || F.fg_fmt != fmt)) {
        if (F.sdk2) fg_disable(sc);   // AMD's order: generation off before its context goes
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();   // a new window size: the context is made for its display size
        vk_ffx_destroy(&F.fg);
        F.fg = nullptr;
    }
    if (!F.fg) {
        VkFfxBackend be;
        // the HUD-less picture has the swapchain's format (gfx_present.c hudless_record)
        ffxCreateContextDescFrameGenerationHudless hl = {};
        hl.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hl.header.pNext = vk_ffx_backend(&be);
        hl.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
        ffxCreateContextDescFrameGeneration d = {};
        d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        d.header.pNext = &hl.header;
        d.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE | (hdr_format(fmt) ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0);
        d.displaySize = {ext.width, ext.height};
        d.maxRenderSize = {max_render.width, max_render.height};
        d.backBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
        ffxCreateContextDescFrameGenerationVersion ver = {};
        ffxOverrideVersion ov = {};
        if (F.sdk2) {
            ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
            ver.version = FFX_FRAMEGENERATION_VERSION;
            ver.header.pNext = d.header.pNext;
            d.header.pNext = &ver.header;
            if (vk_ffx_version(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, "frame generation",
                               cfg_get("NX2NAVI_FFX_FRAMEGEN"), &ov)) {
                ov.header.pNext = d.header.pNext;
                d.header.pNext = &ov.header;
            }
        }
        ffxReturnCode_t r = vk_ffx_create(&F.fg, &d.header);
        if (r != FFX_API_RETURN_OK) return F.fg = nullptr, fail("the frame generation context", r);
        F.fg_ext = ext, F.fg_fmt = fmt, F.fg_max = max_render;
        char pv[96];
        rt_log("fg: frame generation context (%s) for %ux%u%s, render size up to %ux%u", vk_ffx_provider(&F.fg, pv, sizeof pv),
               ext.width, ext.height, hdr_format(fmt) ? " (HDR)" : "", max_render.width, max_render.height);
    }
    if (have && (depth.width > F.fg_max.width || depth.height > F.fg_max.height)) {
        static int told;
        if (!told++)
            rt_log("fg: a %ux%u scene is larger than the context's %ux%u: those frames are not generated", depth.width,
                   depth.height, F.fg_max.width, F.fg_max.height);
        have = 0;
    }
    int on = have && !menu_is_open();
    __atomic_store_n(&F.on, on, __ATOMIC_RELAXED);
    if (!have) F.no_inputs++;
    else if (!on) F.paused++;
    F.frame++;
    ffxConfigureDescFrameGeneration cfg = {};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void *)sc;
    cfg.frameGenerationCallback = generate;
    cfg.frameGenerationCallbackUserContext = &F.fg;
    cfg.frameGenerationEnabled = on != 0;
    cfg.allowAsyncWorkloads = false;
    // the picture's rectangle: with black bars beside or below it (a window of another shape) a generation over the
    // whole swapchain warped the picture's edges into the bars
    cfg.generationRect = pic && pic[2] > 0 && pic[3] > 0 ? FfxApiRect2D{pic[0], pic[1], pic[2], pic[3]}
                                                         : FfxApiRect2D{0, 0, (int32_t)ext.width, (int32_t)ext.height};
    if (on && hudless) {
        VkFfxImage h = {hudless, fmt, ext.width, ext.height, 1};
        cfg.HUDLessColor = image(&h);
        F.hudless++;
    }
    cfg.frameID = F.frame;
    ffxReturnCode_t r = vk_ffx_configure(&F.fg, &cfg.header);
    if (r != FFX_API_RETURN_OK) return fail("frame generation configuration", r);
    if (!on) return;
    static uint64_t last_ns;
    uint64_t now = plat_now_ns();
    double dt = last_ns ? (now - last_ns) / 1e6 : 16.67;
    last_ns = now;
    float dt_ms = dt > 0 && dt < 100 ? (float)dt : 16.67f;
    if (F.sdk2) {
        ffxDispatchDescFrameGenerationPrepareV2 p = {};
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        p.frameID = F.frame;
        p.commandList = cb;
        p.renderSize = {depth.width, depth.height};
        p.jitterOffset = {jx, jy};
        p.motionVectorScale = {1.0f, 1.0f};   // render pixels, current to previous (vk_dlaa.c mv_pass), as FSR takes them
        p.frameTimeDelta = dt_ms;
        p.reset = F.reset != 0;
        F.reset = 0;
        p.cameraNear = 0.1f;   // Ocarina's projection: near 0.1, far at infinity (vk_ffx.cpp)
        p.cameraFar = 1e8f;
        p.cameraFovAngleVertical = cam.fov_y > 0.05f && cam.fov_y < 3.0f ? cam.fov_y : 1.0f;
        p.viewSpaceToMetersFactor = 0.01f;
        p.depth = image(&depth);
        p.motionVectors = image(&mv);
        for (int i = 0; i < 3; i++)
            p.cameraPosition[i] = cam.pos[i], p.cameraUp[i] = cam.up[i], p.cameraRight[i] = cam.right[i],
            p.cameraForward[i] = cam.fwd[i];
        r = vk_ffx_dispatch(&F.fg, &p.header);
    } else {   // the FSR 3.1 library takes only this first preparation, which SDK 2's headers mark deprecated
        ffxDispatchDescFrameGenerationPrepare p = {};
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        p.frameID = F.frame;
        p.commandList = cb;
        p.renderSize = {depth.width, depth.height};
        p.jitterOffset = {jx, jy};
        p.motionVectorScale = {1.0f, 1.0f};
        p.frameTimeDelta = dt_ms;
        p.cameraNear = 0.1f;
        p.cameraFar = 1e8f;
        p.cameraFovAngleVertical = 1.0f;
        p.viewSpaceToMetersFactor = 0.01f;
        p.depth = image(&depth);
        p.motionVectors = image(&mv);
        r = vk_ffx_dispatch(&F.fg, &p.header);
    }
    if (r != FFX_API_RETURN_OK) return fail("frame generation preparation", r);
    if (F.frame == 2 || F.frame % 3600 == 0)
        rt_log("fg: %llu frames presented, %llu generated (%llu with the HUD kept out); paused %llu (menu), %llu without depth "
               "and motion%s", (unsigned long long)F.frame, (unsigned long long)F.generated, (unsigned long long)F.hudless,
               (unsigned long long)F.paused, (unsigned long long)F.no_inputs,
               F.sdk2 && F.no_camera ? ", some without a camera of their own" : "");
}

extern "C" void vk_fg_release(void) {
    if (F.sdk2) sdk2_teardown();
    else {
        if (F.fg) vk_ffx_destroy(&F.fg), F.fg = nullptr;
        if (F.sc) vk_ffx_destroy(&F.sc), F.sc = nullptr;
    }
    __atomic_store_n(&F.on, 0, __ATOMIC_RELAXED);
}
