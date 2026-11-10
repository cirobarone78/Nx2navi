// The op-stream replay core of the Vulkan back end: walks the command handles of
// one submission into its command buffer, keeps render targets and fixed-function state, opens and closes dynamic
// rendering passes and records clears and barriers. Draws, binds and dispatches go to vk_exec_op_draw
// (vk_exec_draw.c); copies, counters, events and conditional rendering to vk_exec_op_xfer (vk_exec_xfer.c). Images
// stay in VK_IMAGE_LAYOUT_GENERAL (vk_texture.c); full memory barriers order passes, clears and whatever those two
// record between them.
//
// The op loop handles END / BEGIN / JUMP / CALL and the pure-state ops, applies the state defaults, keeps render targets
// until they change and skips clears while rendering is disabled. A pass has GENERAL-layout attachments, LOAD/STORE and
// a render area from the smallest attachment, with a full barrier before it.
// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "prof.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_dlaa.h"

#define WEAK __attribute__((weak))
#define CALL_DEPTH 8

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t level, base, layers;
    VkImageView view;
} ViewEnt;

static VkExec X;
static int x_ready;
static _Thread_local int x_here;   // this thread is inside vk_exec_record (vk_exec_inline_begin)

// ---- NX2NAVI_TRACE=gfx frame counters

VkExecFrame vk_exec_frame;
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];   // textures written this frame
static int nfw;

uint64_t vk_exec_recording_fence(void) { return x_here ? X.fence : 0; }

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    for (int i = 0; i < nfw; i++)
        if (fw[i].mem == t->mem) { fw[i].n++; return; }
    if (nfw == 64) return;
    fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
    fw[nfw++].n = 1;
}

uint64_t vk_exec_submit_seq;

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    vk_exec_cond_frame_end(frame);   // a frame boundary in replay order (present_image runs on the replay thread)
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame;
    uint64_t ew, eu, el, ed;
    vk_exec_event_stats(&ew, &eu, &el, &ed);
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu read-before-write %llu; "
           "uniform updates %llu (%llu ended a pass); binds %llu in %.2f ms (textures %.2f, buffers %.2f, sig %.2f, push %.2f ms; "
           "descriptors reused %llu, resolved %llu); dynamic state calls %llu (%llu unchanged, left out); "
           "event waits %llu (%llu before their signal: %llu "
           "signalled later, %.1f submits later on average); targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, (unsigned long long)f->fb_war_barriers, (unsigned long long)f->ub_updates,
           (unsigned long long)f->ub_pass_breaks, (unsigned long long)f->bind_calls,
           (double)(f->bind_ns) / 1e6, (double)(f->bind_tex_ns) / 1e6, (double)(f->bind_buf_ns) / 1e6,
           (double)(f->bind_sig_ns) / 1e6, (double)(f->bind_push_ns) / 1e6, (unsigned long long)f->bind_reused,
           (unsigned long long)f->bind_resolved, (unsigned long long)f->dyn_calls, (unsigned long long)f->dyn_left,
           (unsigned long long)ew, (unsigned long long)eu, (unsigned long long)el,
           el ? (double)ed / (double)el : 0.0, list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

enum { W_UNRENDERABLE, W_LEVEL, W_LAYER, W_VIEW_FORMAT, W_BLOCK_VIEW, W_BLOCK_LAYERS, W_CLEAR_TARGET, W_CLEAR_MASK, W_STENCIL_MASK, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

// NVN defaults of the replay state (nvn_mtl.m).
static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = -1;
}

// ---- render targets

static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

// A view format other than the texture's is used when both map directly and have the same texel size (the image is
// created MUTABLE_FORMAT for colour formats). blocks: t is a BC texture with block_views, and the view an uncompressed
// format whose texel is one of its blocks.
static int view_format(const tex_t *t, const view_t *v, int blocks, VkFormat *out) {
    TexVkFormat a, b;
    int bw, bh, vbw, vbh;
    if (!tex_vk_format(t->d.format, &a) || a.vk != a.ideal || !tex_vk_format(v->format, &b) || b.vk != b.ideal ||
        b.depth || b.stencil)
        return 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    if (!bpb || bpb != vbpb || (!blocks && (bw != 1 || bh != 1)) || vbw != 1 || vbh != 1) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, b.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    *out = b.vk;
    return 1;
}

// ---- 3D BC render targets
// A 3D BC texture has no block views (vk_texture.c create_tex, NX2NAVI_BLOCK_VIEWS_3D): on RDNA2 cards such an image
// made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE took GPU writes past the end of its memory (issue #5). Draws
// into it through an uncompressed view of its blocks (agl's BC4 encoder fills 32x32x32 and 64x64x64 volumes and their
// levels at the logo) go to a stand-in: a 2D array of the blocks of one level in the view's format, one layer per
// depth plane. It starts with the level's content, so a pass that writes part of a plane keeps the rest, and each pass
// end copies the planes it drew into the level (a copy between a compressed and a size-compatible uncompressed format).
typedef struct { VkImage bc, img; VmaAllocation alloc; VkFormat fmt; uint32_t level, w, h, d; } BcStandIn;
#define BC_STANDINS 64   // one per level drawn: 6 volumes of up to 7 levels at the logo
static BcStandIn bcs[BC_STANDINS];
static int nbcs;
static PlatMutex bcs_lock = PLAT_MUTEX_INIT;   // the replay adds and reads; destroy_tex (any thread) removes

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, uint32_t layers) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                           VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The stand-in of level level of a 3D BC image for draws through view format fmt, made on first use; 0 when none can
// be made (logged). Replay thread.
static int bc_standin(const VkTexImage *img, uint32_t level, VkFormat fmt, BcStandIn *out) {
    static int warned;
    plat_mutex_lock(&bcs_lock);
    int full = nbcs == BC_STANDINS;
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == img->image && bcs[i].level == level && bcs[i].fmt == fmt) {
            *out = bcs[i];
            plat_mutex_unlock(&bcs_lock);
            return 1;
        }
    plat_mutex_unlock(&bcs_lock);
    if (full) {
        if (!warned++) rt_log("vk: more than %d 3D BC render targets: the others are left out", BC_STANDINS);
        return 0;
    }
    uint32_t lw = level_extent(img->width, level), lh = level_extent(img->height, level);
    BcStandIn s = {img->image, VK_NULL_HANDLE, NULL, fmt, level, (lw + 3) / 4, (lh + 3) / 4,
                   level_extent(img->depth, level)};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {s.w, s.h, 1}, .mipLevels = 1, .arrayLayers = s.d, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &s.img, &s.alloc, NULL) != VK_SUCCESS) {
        if (!warned++) rt_log("vk: stand-in for a 3D BC render target could not be created: left out");
        return 0;
    }
    // In the upload batch, which goes to the queue before this part: GENERAL, then the texture's blocks.
    VkCommandBuffer cb = vk_upload_begin();
    image_barrier(cb, s.img, VK_IMAGE_LAYOUT_UNDEFINED, s.d);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.d},
                      .extent = {lw, lh, s.d}};   // texels of the compressed source
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_upload_end();
    plat_mutex_lock(&bcs_lock);
    bcs[nbcs++] = s;
    plat_mutex_unlock(&bcs_lock);
    rt_log("vk: 3D BC render target %ux%ux%u level %u drawn through a %ux%u x %u stand-in (format %d)", img->width,
           img->height, img->depth, level, s.w, s.h, s.d, (int)fmt);
    *out = s;
    return 1;
}

// vk_exec_draw.c vk_exec_image_gone: the stand-in of a BC image that goes, freed once the GPU is done with it.
void vk_exec_bc_gone(VkImage image) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == image) {
            vk_defer_image(bcs[i].img, bcs[i].alloc);
            bcs[i--] = bcs[--nbcs];
        }
    plat_mutex_unlock(&bcs_lock);
}

// At a pass end: the planes the pass drew into a stand-in go to its texture.
static void bc_copy_back(VkExec *ex, const VkExecTarget *t) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++) {
        if (bcs[i].img != t->image) continue;
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->base_layer, t->layers},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, bcs[i].level, 0, 1},
                          .dstOffset = {0, 0, (int32_t)t->base_layer},
                          .extent = {bcs[i].w, bcs[i].h, t->layers}};   // texels of the uncompressed source
        vk_gpu_gap_note(VK_GAP_BC_BACK, 0);
        vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, bcs[i].bc, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ex->recorded++;
        break;
    }
    plat_mutex_unlock(&bcs_lock);
}

// A draw into a 3D BC image without block views through an uncompressed view of its blocks: the target is the
// image's stand-in (bc_standin). 1 when o is filled (target or left out), 0 for any other target.
static int bc_target(const tex_t *t, const view_t *v, int has_v, const VkTexImage *img, VkExecTarget *o) {
    int bw, bh;
    if (img->type != VK_IMAGE_TYPE_3D || img->block_views || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 4 || bh != 4)
        return 0;
    VkFormat fmt;
    BcStandIn s;
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return 1;
    }
    if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt) || !bc_standin(img, level, fmt, &s)) {
        warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                  t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
        return 1;
    }
    uint32_t base = 0, layers = s.d;
    if (v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < s.d ? s.d - base : 0);
    }
    if (base >= s.d || !layers) return 1;
    if (base + layers > s.d) layers = s.d - base;
    o->image = s.img;   // not img->image: view_of makes this command buffer's view of the stand-in
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = 0, o->base_layer = base, o->layers = layers;
    o->width = s.w, o->height = s.h, o->samples = 1;
    o->nvn_format = v->format;
    return 1;
}

static void resolve_target(const tex_t *t, const view_t *v, int has_v, int depth_slot, VkExecTarget *o) {
    memset(o, 0, sizeof *o);
    o->tex = t;
    if (!t) return;
    const VkTexImage *img = vk_texture_get(t);
    if (img && !depth_slot && bc_target(t, v, has_v, img, o)) return;   // a 3D BC image: its stand-in
    VkImageUsageFlags need = depth_slot ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot;   // slices drawn through 2D views (2D_ARRAY_COMPATIBLE)
    if (!img || !(img->usage & need) || (img->type != VK_IMAGE_TYPE_2D && !is3d)) {
        warn_once(W_UNRENDERABLE, "render target format %#x target %d (%ux%u) is not renderable here: left out",
                  t->d.format, t->d.target, t->d.width, t->d.height);
        return;
    }
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return;
    }
    // A 3D level's depth planes take the place of layers (Vulkan views them as the array layers of a 2D array view).
    uint32_t nlay = is3d ? level_extent(img->depth, level) : img->layers;
    uint32_t base = 0, layers = nlay;
    if (has_v && v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < nlay ? nlay - base : 0);
    }
    if (base >= nlay || !layers) {
        warn_once(W_LAYER, "render target layer %u of a %u-layer texture: left out", base, nlay);
        return;
    }
    if (base + layers > nlay) layers = nlay - base;
    VkFormat fmt = img->format;
    int32_t nvn_fmt = t->d.format;
    int blocks = !depth_slot && img->block_views;
    if (blocks) {   // a BC texture takes draws only through an uncompressed view of its blocks
        if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt)) {
            warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                      t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
            return;
        }
        nvn_fmt = v->format;
        if (layers > 1) {   // an uncompressed view of a BC image has one layer (no maintenance6 here)
            warn_once(W_BLOCK_LAYERS, "render target format %#x: block view of %u layers, first layer only", t->d.format,
                      layers);
            layers = 1;
        }
    } else if (!depth_slot && has_v && v->set_format && v->format != t->d.format) {
        if (view_format(t, v, 0, &fmt)) nvn_fmt = v->format;
        else warn_once(W_VIEW_FORMAT, "render target view format %#x of a format %#x texture: texture format used",
                       v->format, t->d.format);
    }
    o->image = img->image;
    o->img = img;
    o->scale = img->scale ? img->scale : 1;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = depth_slot ? img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = level, o->base_layer = base, o->layers = layers;
    o->is3d = (uint8_t)is3d;
    o->blocks = (uint8_t)blocks;
    o->width = level_extent(img->width, level), o->height = level_extent(img->height, level);
    if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4;   // the view's extent: 4x4 blocks
    o->samples = img->samples ? img->samples : 1;
    o->nvn_format = nvn_fmt;
    if (!o->aspect) o->image = VK_NULL_HANDLE;
}

/* The size of a pass without attachments: the far corner of viewport 0, else of the scissor, 1..16384.
   NX2navi draws a fragment shader that only writes a storage buffer into a 1x1
   viewport; a pass sized by the viewport's width alone puts an offset viewport outside the scissor, and the cave rock
   loses its ambient light. */
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (ex->has_vp) {
        x1 = ceil(fmax(ex->vp[0][0], ex->vp[0][0] + ex->vp[0][2]));
        y1 = ceil(fmax(ex->vp[0][1], ex->vp[0][1] + ex->vp[0][3]));
    }
    if ((x1 < 1 || y1 < 1) && ex->has_scissor) x1 = (double)ex->scissor.x + ex->scissor.w, y1 = (double)ex->scissor.y + ex->scissor.h;
    *w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1;
    *h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
}

// Resolves ex->rt to this command buffer's images and derives the pass parameters and the pipeline's target formats.
static void resolve_rt(VkExec *ex) {
    if (ex->rt_resolved) return;
    ex->rt_resolved = 1;
    const p_set_rt *r = &ex->rt;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0, scale = 1;
    for (int i = 0; i < 8; i++) {
        VkExecTarget *t = &ex->color[i];
        resolve_target(i < n ? r->color[i] : NULL, &r->cview[i], i < n && r->has_cview[i], 0, t);
        ex->pass_color[i] = t->image ? t->format : VK_FORMAT_UNDEFINED;
        ex->pipe.color_fmt[i] = t->image ? t->nvn_format : 0;
        if (!t->image) continue;
        if (t->width < w) w = t->width;
        if (t->height < h) h = t->height;
        if (t->layers < layers) layers = t->layers;
        if (!samples) samples = t->samples;
        if (t->scale > scale) scale = t->scale;
    }
    VkExecTarget *d = &ex->depth;
    resolve_target(r->depth, &r->dview, r->has_dview, 1, d);
    ex->pass_depth = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pass_stencil = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = d->image ? d->nvn_format : 0;
    if (d->image) {
        if (d->width < w) w = d->width;
        if (d->height < h) h = d->height;
        if (d->layers < layers) layers = d->layers;
        if (!samples) samples = d->samples;
        if (d->scale > scale) scale = d->scale;
    }
    ex->pass_noatt = w == UINT32_MAX;
    if (ex->pass_noatt) {
        vk_exec_noatt_dims(ex, &w, &h);
        layers = 1;
        static int logged;
        if (logged < 8 && ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != w || ex->vp[0][3] != h))
            logged++, rt_log("vk_exec: pass without attachments %ux%u for viewport %g,%g %gx%g", w, h, ex->vp[0][0],
                             ex->vp[0][1], ex->vp[0][2], ex->vp[0][3]);
    }
    ex->pass_w = w, ex->pass_h = h, ex->pass_layers = layers;
    ex->pass_scale = ex->pass_noatt ? 1 : scale;
    ex->pass_ncolor = (uint32_t)n;
    ex->pass_samples = samples ? samples : 1;
    ex->pipe.samples = (uint8_t)ex->pass_samples;
}

// The attachment view of a target. A registered texture's view lives as long as its image (vk_exec_target_view); any
// other image's view, and that of a texture destroyed since the target was resolved (gone: its record is freed only
// after this submission, vk_defer_free), is made for this command buffer and freed at its fence.
static VkImageView view_of(VkExec *ex, const VkExecTarget *t) {
    if (t->img && t->img->image == t->image && !t->img->gone)
        return vk_exec_target_view(t->img, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, t->blocks);
    ViewEnt *v = ex->views;
    for (int i = 0; i < ex->nviews; i++)
        if (v[i].image == t->image && v[i].format == t->format && v[i].type == t->view_type && v[i].aspect == t->aspect &&
            v[i].level == t->level && v[i].base == t->base_layer && v[i].layers == t->layers)
            return v[i].view;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = t->view_type, .format = t->format,
                                .subresourceRange = {t->aspect, t->level, 1, t->base_layer, t->layers}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (t->blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
        rt_fatal("vk: render target view (format %d, %ux%u) could not be created", (int)t->format, t->width, t->height);
    if (ex->nviews == ex->cap_views) {
        ex->cap_views = ex->cap_views ? ex->cap_views * 2 : 16;
        ex->views = realloc(ex->views, sizeof(ViewEnt) * (size_t)ex->cap_views);
        if (!ex->views) rt_fatal("vk: out of memory");
        v = ex->views;
    }
    v[ex->nviews++] = (ViewEnt){t->image, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, view};
    return view;
}

typedef struct { int n; VkImageView v[]; } ViewList;

static void free_views(void *arg) {   // completion thread, once the submission that used them is done
    ViewList *l = arg;
    VkDevice dev = vk_ctx()->device;
    for (int i = 0; i < l->n; i++) vkDestroyImageView(dev, l->v[i], NULL);
    free(l);
}

// ---- target history
// The presented frame (gfx_counters.frames + 1; 0: never) each target subresource was last rendered into, and the one
// before. prepare_draw lets a draw into a target no recent frame rendered (a one-shot draw: a volume or LUT generated
// once) wait for its shaders and pipeline instead of skipping it. Per subresource, not per image: a volume may be
// generated a few slices per frame.
// Recent means within TFR_RECENT frames, not the frame before alone: the game's output images are a ring of three, its
// probe and shadow layers are refreshed in turn every second to fourth frame, and the mips of its small effect buffers
// alternate between images. Sets of TFR_WAYS, the oldest entry replaced: hundreds of subresources are rendered a
// frame, and in a direct-mapped table of 1024 a fifth of the scene passes and most of the probe layers read as never
// rendered because another subresource had taken their slot. Each misread made a draw whose pipeline was still
// building wait for it (ONESHOT_WAIT_MS), so a new area was entered one pipeline at a time.
#define TFR_SETS 1024
#define TFR_WAYS 4
#define TFR_RECENT 4
typedef struct { VkImage image; uint32_t level, layer; uint64_t last, prev; } TargetFrames;
static TargetFrames tfr[TFR_SETS * TFR_WAYS];

// The target's entry, or with make the entry it takes (the set's oldest, an empty one first); NULL without make
// when it has none.
static TargetFrames *tfr_find(const VkExecTarget *t, int make) {
    uint64_t h = ((uint64_t)(uintptr_t)t->image ^ (uint64_t)t->level << 56 ^ (uint64_t)t->base_layer << 40) *
                 0x9E3779B97F4A7C15ull;
    TargetFrames *set = &tfr[(h >> 54) * TFR_WAYS], *oldest = set;
    for (int i = 0; i < TFR_WAYS; i++) {
        if (set[i].image == t->image && set[i].level == t->level && set[i].layer == t->base_layer) return &set[i];
        if (set[i].last < oldest->last) oldest = &set[i];
    }
    if (!make) return NULL;
    *oldest = (TargetFrames){t->image, t->level, t->base_layer, 0, 0};
    return oldest;
}

static void tfr_note(const VkExecTarget *t, uint64_t stamp) {
    TargetFrames *e = tfr_find(t, 1);
    if (e->last != stamp) e->prev = e->last, e->last = stamp;
}

// The frame before the current one that last rendered the target, 0 for none.
static uint64_t tfr_before(const VkExecTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_find(t, 0);
    return !e ? 0 : e->last == stamp ? e->prev : e->last;
}

static int tfr_recent(const VkExecTarget *t, uint64_t stamp) {
    uint64_t before = tfr_before(t, stamp);
    return before && stamp - before <= TFR_RECENT;
}

// No frame before the current one rendered the target.
static int tfr_first(const VkExecTarget *t, uint64_t stamp) { return !tfr_before(t, stamp); }

// With any, a recent depth target alone does not make the pass per-frame when a colour target is rendered for the
// first time: the game's item icon capture (an inventory icon) draws into a new colour target with the
// per-frame depth buffer, once. Its draws skipped there stay missing from the icon until the item is captured again.
int vk_exec_targets_recent(VkExec *ex, int any) {
    resolve_rt(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    int n = 0, color = 0, depth = 0, color_first = 0;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        n++;
        if (tfr_recent(t, stamp)) *(i < 8 ? &color : &depth) = 1;
        else if (!any) return 0;
        else if (i < 8 && tfr_first(t, stamp)) color_first = 1;
    }
    return any ? color || (depth && !color_first) : n > 0;
}

// One line naming each target of the current pass: format, extent, layer and the two frames it was last rendered in
// (prepare_draw's pipeline wait log).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap) {
    resolve_rt(ex);
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < 9 && n < cap; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        const TargetFrames *e = tfr_find(t, 0);
        int w = snprintf(buf + n, cap - n, "%s%c%d fmt %d %ux%u L%u seen %llu/%llu", n ? "; " : "", i < 8 ? 'c' : 'd',
                         i < 8 ? i : 0, (int)t->format, t->width, t->height, t->base_layer,
                         (unsigned long long)(e ? e->last : 0), (unsigned long long)(e ? e->prev : 0));
        if (w < 0) break;
        n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "no targets");
}

// ---- passes and barriers

static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

// Full barrier. The images in the feedback layout become want[] (pass begin) or none (every other barrier): the others
// go back to GENERAL and the new ones leave it, in the same command.
static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant) {
    vk_exec_pass_end(ex);
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    vk_exec_frame.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) { barrier_to(ex, NULL, NULL, 0); }

void vk_exec_pass_begin(VkExec *ex) {
    if (ex->in_pass) return;
    resolve_rt(ex);
    vk_contact_pass_begin(ex);
    vk_dlaa_pass(ex);   // DLAA runs before the bloom chain's first pass (vk_dlaa.c)
    // Learned targets (VkTexImage.fb_learned) are attached in the feedback layout
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, nwant, t->image)) continue;
        want[nwant] = t->image, wasp[nwant++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    barrier_to(ex, want, wasp, nwant);   // passes, clears, copies and dispatches see each other's results
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->fb_read = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    // The start mark goes between the barrier and vkCmdBeginRendering, its end right after vkCmdEndRendering
    // (vk_exec_pass_end): outside the pass, and without breaking it.
    VkPassMark *m = ex->mark = vk_pass_mark_begin(ex->cb, VK_MARK_PASS, ex->fence);
    if (m) {
        m->step = ex->mark_passes++;
        m->w = ex->pass_w, m->h = ex->pass_h, m->d = ex->pass_layers;
        m->layer = ex->depth.image ? ex->depth.base_layer : 0;
        m->ncolor = (uint8_t)(ex->pass_ncolor < 8 ? ex->pass_ncolor : 8);
        for (int i = 0; i < m->ncolor; i++) m->color[i] = ex->pass_color[i];
        m->depth = ex->pass_depth ? ex->pass_depth : ex->pass_stencil;
        // The shaders bound now are the first draw's when it repeats the pipeline state; when it does not,
        // vk_exec_draw.c prepare_draw replaces them as it records the new state.
        vk_exec_draw_shaders(&m->first[0], &m->first[1]);
        ex->mark_draws0 = vk_exec_frame.draws;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    vkCmdBeginRendering(ex->cb, &ri);
    vk_exec_frame.passes++;
    ex->recorded++;
    ex->in_pass = 1;
}

void vk_exec_pass_end(VkExec *ex) {
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    vkCmdEndRendering(ex->cb);
    ex->recorded++;
    ex->in_pass = 0;
    if (ex->mark) {
        // vk_exec_frame_log resets the frame counters at a present (NX2NAVI_TRACE=gfx): then only the draws since it count
        uint64_t d = vk_exec_frame.draws, d0 = ex->mark_draws0;
        ex->mark->draws = (uint32_t)(d >= d0 ? d - d0 : d);
        if (ex->mark->draws) vk_exec_draw_shaders(&ex->mark->last[0], &ex->mark->last[1]);
        vk_pass_mark_end(ex->cb, ex->mark);
        ex->mark = NULL;
    }
    for (int i = 0; i < ex->pass_ncolor && i < 8; i++)   // stand-ins of 3D BC targets go to their textures
        if (ex->color[i].image && ex->color[i].img && ex->color[i].img->image != ex->color[i].image)
            bc_copy_back(ex, &ex->color[i]);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) vk_texture_gpu_written(ex->color[i].tex), tfr_note(&ex->color[i], stamp);
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex), tfr_note(&ex->depth, stamp);
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = &X;
    *reopen = 0;
    if (!x_here || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; feedback-layout targets go back to GENERAL
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {
    X.recorded++;
    if (reopen) vk_exec_pass_begin(&X);
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {   // completion thread
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(X.fence, free_inline_buf, t);
    return t->b;
}

VkRect2D vk_exec_scissor(const VkExec *ex) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!ex->has_scissor) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t k = ex->pass_scale > 1 ? ex->pass_scale : 1;
    int64_t x0 = ex->scissor.x * k, y0 = ex->scissor.y * k;
    int64_t x1 = x0 + (int64_t)ex->scissor.w * k, y1 = y0 + (int64_t)ex->scissor.h * k;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

// ---- clears

// Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the
// image directly instead of opening a pass for it.
static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    // vkCmdClear*Image addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it
    // cannot clear a compressed image: views of BC blocks clear in a pass too
    if (ex->in_pass || t->is3d || t->blocks || t->width != ex->pass_w || t->height != ex->pass_h) return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    uint32_t bit = a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
    vk_exec_feedback_war(ex, bit);   // a draw of this pass sampled it: its reads finish before the clear writes
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= bit;
}

// NX2NAVI_LOG_RCOND=1: clears issued under a render condition (the first 400), run or skipped, with their target.
static void clear_cond_log(const VkExec *ex, const char *what, int index) {
    static int on = -1, n;
    if (on < 0) on = rt_knob("LOG_RCOND", "0");
    if (on == 2 && index < 0 && ex->has_render_cond && ex->depth.width >= 1600) {   // full-size depth clears, all of them
        char why[256];
        vk_exec_cond_why(&ex->render_cond, why, sizeof why);
        rt_log("SEQ depth clear %#llx %s; %s", ex->depth.img ? (unsigned long long)ex->depth.img->addr : 0ull,
               ex->render_disabled ? "skipped" : "RUN", why);
        return;
    }
    if (!on || !ex->has_render_cond || n >= 400) return;
    n++;
    const VkExecTarget *t = index < 0 ? &ex->depth : index < (int)ex->pass_ncolor ? &ex->color[index] : NULL;
    rt_log("vk: %s clear %d under the condition at %#llx (mode %d): %s, target %ux%u", what, index,
           (unsigned long long)ex->render_cond.addr, ex->render_cond.mode, ex->render_disabled ? "skipped" : "run",
           t ? t->width : 0, t ? t->height : 0);
}

static void clear_color(VkExec *ex, const p_clear_color *c) {
    vk_exec_frame.clears++;
    clear_cond_log(ex, "colour", c->index);
    if (ex->has_render_cond) ex->render_disabled ? vk_cond_clears_skip++ : vk_cond_clears_run++;
    if (ex->render_disabled) return;
    resolve_rt(ex);
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        warn_once(W_CLEAR_TARGET, "colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {   // vkCmdClearAttachments ignores write masks; a masked clear needs a draw (vk_exec_draw.c)
        warn_once(W_CLEAR_MASK, "colour clear with channel mask %#x is not replayed: skipped", mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);   // integer targets: the payload's bits as the front end stored them
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        vk_gpu_gap_note(VK_GAP_CLEAR_TARGET, (uint64_t)t->width * t->height);
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

static void clear_ds(VkExec *ex, const p_clear_ds *c) {
    clear_cond_log(ex, "depth", -1);
    if (ex->has_render_cond) ex->render_disabled ? vk_cond_clears_skip++ : (vk_cond_clears_run++, vk_cond_depth_run++);
    if (ex->render_disabled) return;
    resolve_rt(ex);
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (sm == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else warn_once(W_STENCIL_MASK, "stencil clear with mask %#x is not replayed: skipped", sm);
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        vk_gpu_gap_note(VK_GAP_CLEAR_TARGET, (uint64_t)t->width * t->height);
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}

// ---- the op loop

static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;   // the same targets again: the pass goes on
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

// ---- command buffer parts
// NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after
// the whole replay. Between passes, once NX2NAVI_SPLIT_DRAWS (64; 0 off) draws and dispatches were recorded since the
// last part, the part goes to the queue (vk_sync_split) and the replay continues in a new command buffer. Only
// command-buffer state is lost there (bindings and dynamic state: vk_exec_draw_new_cb, ex->dirty); barriers, layouts and
// queries are not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image,
// and occlusion queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last
// part signals the value. The texture pool set a part binds is sealed when the part goes (vk_exec_pool_split): later
// parts write and bind another, and a part is not split off while no other set is free of this submission.
// A part also spans at least NX2NAVI_SPLIT_US (1000) of replay time: a split costs about 40 us of the render thread
// (vkQueueSubmit2, a command buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X;
// a millisecond of recording is a fraction of a millisecond of GPU work, so the GPU still starts early.
// With NX2NAVI_SPLIT_IDLE=1, neither limit holds the first part of a submission back while the GPU has run everything
// submitted: it goes at the first pass boundary after a draw or dispatch. Each frame the game's thread waits for the GPU
// to run the frame's first submission (nvn_api_core.c nvnQueueSubmitCommands, NX2NAVI_SUBMIT_FINISH), so the GPU is idle
// when the next one replays, and waited for the first 64 draws and 1 ms of it in Kokiri Forest (2026-11-09). Off by
// default: with it the foliage of Kokiri Forest flickered (RTX 5090, 2026-11-09), and the parts it cuts that early are
// the one thing it changes; why they matter is not known yet.
// Both limits also split inside a pass (NX2NAVI_SPLIT_IN_PASS, on): the pass ends there and the next draw begins it
// again in the new part, as after a barrier (attachments load and store; occlusion queries continue in a new segment).
// Kokiri Forest's two scene passes of ~500 draws take 2-4 ms each to replay and most of the frame's GPU time; split only
// between passes, the GPU waited for each whole pass, idle up to 3.6 ms a frame (2026-11-09).
// After the time limit held a part back, the clock is read again only 8 draws later (split_at): read after every
// command, it was 4% of the replay in Kokiri Forest.
static int split_n = -1, split_idle, split_in_pass, split_draws, split_at, part_start;
static uint64_t split_ns, part_t0;
static uint64_t n_parts, n_subs, n_idle_parts, n_pass_splits;

static int split_wanted(void) {
    if (split_draws >= split_at) return 1;
    // the first part only: later parts of the submission run on the GPU, while done and issued stay as they were
    return split_idle && split_draws && !part_start && vk_sync_fence_done() >= vk_sync_issued();
}

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("NX2NAVI_SPLIT_DRAWS");
        if (split_n < 0) split_n = 0;
        int us = cfg_int("NX2NAVI_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        split_idle = split_n && cfg_bool("NX2NAVI_SPLIT_IDLE");
        split_in_pass = split_n && cfg_bool("NX2NAVI_SPLIT_IN_PASS");
        rt_log("vk: replay command buffer split every %d draws %s, parts of at least %d us%s%s", split_n,
               split_in_pass ? "(inside passes too)" : "between passes", us > 0 ? us : 0, split_n ? "" : " (off)",
               split_idle ? "; a first part at once while the GPU is idle" : "");
        split_at = split_n ? split_n : INT_MAX;
    }
    if (!split_n) return;
    // the first part while the GPU is idle: at a pass boundary only, where a split costs no pass restart
    int idle = !ex->in_pass && split_idle && split_draws && !part_start && vk_sync_fence_done() >= vk_sync_issued();
    if (!idle) {
        if (ex->in_pass && !split_in_pass) return;
        if (split_draws < split_n) return;
        if (split_ns && kern_now_ns() - part_t0 < split_ns) {   // split_draws stays: a later draw asks again
            split_at = split_draws + 8;
            return;
        }
    }
    split_draws = 0;
    split_at = split_n;
    if (ex->recorded == part_start || !vk_exec_pool_split(ex->fence)) return;
    if (idle) n_idle_parts++;
    if (ex->in_pass) vk_exec_pass_end(ex), n_pass_splits++;   // the next draw begins the pass again, in the new part
    ex->cb = vk_sync_split();
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
}

// ---- event order across the game's queues
// Ocarina runs work on a second queue that hands results back and forth with the main one by events: each frame the
// worker submits first, its command buffer waits (WaitEvent) for a signal the main queue gives in its submission a few
// submits later, does a step, signals, waits for the next point of the main queue's frame, and so on, while the main
// queue waits for the worker's signals before it uses the results (Kokiri Forest: about 13 waits a frame, 7 of them
// reached before their signal in submission order). On one Vulkan queue in submission order the worker's steps would
// run before the work they wait for and read the previous frame's results: Link and the other characters were
// drawn much too dark, at any time of day.
// So a top-level wait whose condition does not hold in replay order (vk_exec_wait_event: memory, or the signals
// replayed so far) stops its submission there: the rest becomes a continuation of that queue (Cont), and later
// submissions of the queue line up behind it. After every replayed signal the continuations whose wait now holds go
// on, inside the command buffer being recorded: the open pass ends, a full barrier orders them after what came before
// (the event's memory dependency), the queue's own state replaces the running one while they run, and another barrier
// and the running state come back after. One that would still wait after CONT_MAX submissions goes on regardless, as
// it did before (counted, logged). Only with the replay on the submitting thread: the recording copies a continuation
// reads stay alive there until the game recycles them, which it does only after the work ran on the console.
// NX2NAVI_EVENT_ORDER=0: every submission in the order it came.
#define CONT_MAX 36
#define EVO_QUEUES 16
typedef struct Cont {
    struct Cont *next;      // the next submission of the same queue
    uint64_t *h;            // private copies of the recordings it has still to walk (cont_new), the one being walked
    int n, i;
    const op_hdr *at;       // the wait it stopped at in h[i] (NULL: not started)
    uint64_t since;         // vk_exec_submit_seq when it stopped
    int has_st;             // st holds the queue's state (else the running state carries over, as for any submission)
    uint8_t *st;
} Cont;
static struct { uintptr_t q; Cont *head, *tail; } evq[EVO_QUEUES];
static int evo_on = -1, evo_in;     // knob; inside a continuation
static uintptr_t evo_queue;         // queue of the stream being walked (0: it cannot stop at a wait)
static int evo_pending;             // continuations held
static uint64_t evo_held, evo_resumed, evo_forced, evo_switches;   // since the last perf line
int vk_exec_held(void) { return evo_pending; }
#define ST_LO offsetof(VkExec, rt)
#define ST_HI offsetof(VkExec, views)

static const op_hdr *walk(VkExec *ex, const op_hdr *h, int depth);

static int evo_enabled(void) {
    if (evo_on < 0) {
        const char *e = cfg_get("NX2NAVI_EVENT_ORDER");
        evo_on = !(e && !strcmp(e, "0"));
        if (evo_on && gfx_replay_async()) {
            evo_on = 0;
            rt_log("vk: event order across queues off: the replay runs on its own thread (NX2NAVI_REPLAY_ASYNC)");
        } else rt_log("vk: event order across queues %s (NX2NAVI_EVENT_ORDER)", evo_on ? "on" : "off");
    }
    return evo_on;
}

static int evq_slot(uintptr_t q) {
    int free_i = -1;
    for (int i = 0; i < EVO_QUEUES; i++) {
        if (evq[i].q == q) return i;
        if (free_i < 0 && !evq[i].head) free_i = i;
    }
    if (free_i >= 0) evq[free_i].q = q;
    return free_i;
}

// Holds h[from..n), stopped at `at` in h[from] (NULL: from its start). The recordings are copied: the game records a
// command buffer again once the fence it took after the submission is done, and that fence does not wait for a held
// part (it signals with the submission that held it).
static Cont *cont_new(const uint64_t *h, int n, int from, const op_hdr *at) {
    Cont *c = calloc(1, sizeof *c);
    if (!c || !(c->h = calloc((size_t)(n - from) + 1, sizeof *c->h))) rt_fatal("vk: out of memory (held submission)");
    for (int k = from; k < n; k++) {
        const op_hdr *x = nvn_op_first(h[k]);
        uint64_t bytes = 0;
        for (; x->op != OP_END; x = nvn_op_next(x)) bytes += x->size;
        bytes += x->size;
        uint8_t *copy = malloc(bytes);
        if (!copy) rt_fatal("vk: out of memory (held submission, %llu bytes)", (unsigned long long)bytes);
        memcpy(copy, (const void *)(uintptr_t)h[k], bytes);
        c->h[k - from] = (uintptr_t)copy;
        if (k == from && at) c->at = (const op_hdr *)(const void *)(copy + ((const uint8_t *)at - (const uint8_t *)(uintptr_t)h[k]));
    }
    c->n = n - from;
    c->since = vk_exec_submit_seq;
    return c;
}

static void cont_free(Cont *c) {
    for (int k = 0; k < c->n; k++) free((void *)(uintptr_t)c->h[k]);
    free(c->h), free(c->st), free(c);
}

static void cont_push(int slot, Cont *c) {
    if (evq[slot].tail) evq[slot].tail->next = c;
    else evq[slot].head = c;
    evq[slot].tail = c;
    evo_pending++;
}

// Swaps the queue state in ex with buf (ST_HI - ST_LO bytes); what the swapped-in state names is bound again.
static void st_swap(VkExec *ex, uint8_t *buf) {
    static uint8_t tmp[ST_HI - ST_LO];
    memcpy(tmp, (uint8_t *)ex + ST_LO, sizeof tmp);
    memcpy((uint8_t *)ex + ST_LO, buf, sizeof tmp);
    memcpy(buf, tmp, sizeof tmp);
    ex->rt_resolved = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->tex_serial++;
    vk_exec_draw_new_cb();
}

static int cont_ready(const Cont *c) {
    return !c->at || vk_exec_event_met(nvn_op_payload(c->at)) || vk_exec_submit_seq - c->since > CONT_MAX;
}

int vk_exec_held_ready(void) {
    for (int s = 0; evo_pending && s < EVO_QUEUES; s++)
        if (evq[s].head && cont_ready(evq[s].head)) return 1;
    return 0;
}

// Held work that goes on in a submission without commands of its own (the present thread's empty one after its blit,
// vk_backend.c present_main, or the one a waiting present makes, present_held) gets what any submission's recording
// gets: the VRAM mirror sync before and the write-back after (vk_mem_mirror_begin/end), and a uniform copy (vk_usnap).
// Without them its draws read mirrors that missed the CPU's writes since the last submission, and its GPU writes never
// went back to guest memory: the next vk_mem_mirror_begin dropped the write-back list. NX2NAVI_HELD_SYNC=0: as before.
static int held_sync(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("HELD_SYNC", "1");
    return on;
}

// Walks the continuation from where it stopped; 1 when it stopped at another wait (c->i, c->at updated), 0 when done.
static int cont_walk(VkExec *ex, Cont *c) {
    if (c->at && !vk_exec_event_met(nvn_op_payload(c->at))) {
        evo_forced++;
        static int logged;
        if (logged++ < 8)
            rt_log("vk: event order: a held submission goes on after %llu submissions without its signal",
                   (unsigned long long)(vk_exec_submit_seq - c->since));
    }
    for (; c->i < c->n; c->i++) {
        const op_hdr *from = c->at ? nvn_op_next(c->at) : nvn_op_first(c->h[c->i]);
        c->at = NULL;
        const op_hdr *stop = walk(ex, from, 0);
        if (stop) {
            c->at = stop;
            c->since = vk_exec_submit_seq;
            return 1;
        }
    }
    return 0;
}

// Runs the held continuations whose wait holds now, in the command buffer being recorded, until none can go on.
static void evo_resume(VkExec *ex) {
    if (evo_in || !evo_pending) return;
    vk_exec_signalled = 0;
    uintptr_t run_q = evo_queue;
    for (int progress = 1; progress;) {
        progress = 0;
        for (int s = 0; s < EVO_QUEUES; s++) {
            Cont *c = evq[s].head;
            if (!c || !cont_ready(c)) continue;
            progress = 1;
            evo_switches++;
            vk_exec_pass_end(ex);
            vk_exec_barrier(ex);
            uint8_t *mine = malloc(ST_HI - ST_LO);
            if (!mine) rt_fatal("vk: out of memory (queue state)");
            memcpy(mine, (uint8_t *)ex + ST_LO, ST_HI - ST_LO);
            if (c->has_st) st_swap(ex, c->st);
            evo_in = 1, evo_queue = evq[s].q;
            for (;;) {
                evo_resumed++;
                if (cont_walk(ex, c)) {   // waits again: the queue's state stays with it
                    vk_exec_pass_end(ex);
                    if (!c->st && !(c->st = malloc(ST_HI - ST_LO))) rt_fatal("vk: out of memory (queue state)");
                    memcpy(c->st, (uint8_t *)ex + ST_LO, ST_HI - ST_LO);
                    c->has_st = 1;
                    break;
                }
                evq[s].head = c->next;   // done: the queue's next submission starts with the state this one left
                if (!evq[s].head) evq[s].tail = NULL;
                evo_pending--;
                cont_free(c);
                c = evq[s].head;
                if (!c) break;
                c->has_st = 0;
            }
            evo_in = 0, evo_queue = run_q;
            vk_exec_pass_end(ex);
            vk_exec_barrier(ex);
            memcpy((uint8_t *)ex + ST_LO, mine, ST_HI - ST_LO);   // the running stream's state back
            ex->rt_resolved = 0;
            ex->dirty = VK_EXEC_DIRTY_ALL;
            ex->tex_serial++;
            vk_exec_draw_new_cb();
            free(mine);
        }
    }
}

static const op_hdr *walk(VkExec *ex, const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        switch (h->op) {
        case OP_END: return NULL;
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        case OP_SET_RT: set_rt(ex, a); break;
        case OP_CLEAR_COLOR: clear_color(ex, a); break;
        case OP_CLEAR_DS: clear_ds(ex, a); break;
        case OP_VIEWPORTS: {
            const p_viewports *v = a;
            for (int i = 0; i < v->n && v->first + i < 16; i++)
                if (v->first + i >= 0) memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]);
            ex->has_vp = 1;
            ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
            break;
        }
        case OP_SCISSOR:
            ex->scissor = *(const p_scissor *)a;
            ex->has_scissor = 1;
            ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
            break;
        case OP_DEPTH_RANGE: {
            const p_depth_range *r = a;
            ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
            ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
            break;
        }
        case OP_STENCIL: {
            const p_stencil *s = a;
            for (int f = 0; f < 2; f++) {
                if (!(s->face & (1 << f))) continue;
                if (s->which == 0) ex->st_vmask[f] = (uint32_t)s->value_mask;
                else if (s->which == 1) ex->st_wmask[f] = (uint32_t)s->mask;
                else ex->st_ref[f] = (uint32_t)s->ref;
            }
            ex->dirty |= VK_EXEC_DIRTY_STENCIL;
            break;
        }
        case OP_POLY_OFFSET:
            ex->poly_offset = *(const p_poly_offset *)a;
            ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
            break;
        case OP_BLEND_COLOR:
            memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
            ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
            break;
        case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
        case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
        case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
        case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
        case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool; break;
        case OP_BARRIER: vk_exec_barrier(ex); break;
        case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
            if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND) vk_exec_frame.draws_seen++, split_draws++;
            if (h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) vk_exec_frame.dispatches++, split_draws++;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CC:
        case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
            vk_exec_op_draw(ex, h->op, a);
            break;
        case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
            if (h->op != OP_CLEAR_TEX) vk_exec_frame.copies++;
            if (vk_dump_armed) vk_dump_copy(ex, h->op, a);
            // fall through
        case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
        case OP_CLEAR_BUF:
            vk_exec_op_xfer(ex, h->op, a);
            break;
        case OP_WAIT_EVENT:
            if (!vk_exec_wait_event(a) && depth == 0 && evo_queue) {
                vk_exec_pass_end(ex);
                return h;
            }
            break;
        default: rt_fatal("vk: op %s has no replay handler", nvn_op_name(h->op));
        }
        if (vk_exec_signalled && depth == 0 && !evo_in) evo_resume(ex);
        if (split_draws && split_wanted()) maybe_split(ex);
        h = nvn_op_next(h);
    }
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    // The replay records on the thread the game submitted from, and the kernel schedules that like any other guest
    // thread: a preemption in the middle of a part leaves the submission late for the frame it belongs to (Kokiri
    // Forest: 274 of 2,737 frames had a late submit). Recording keeps the highest priority so the scheduler prefers it
    // over the guest's other cores; NX2NAVI_RECORDER_PRIO=0 leaves the guest's priorities alone.
    static int prio_done;
    static uint32_t rec_thread;   // the thread that got the priority and the core
    if (!prio_done) {
        prio_done = 1;
        const char *e = cfg_get("NX2NAVI_RECORDER_PRIO");
        if (!(e && !strcmp(e, "0"))) {
            plat_thread_priority(PLAT_PRIO_HIGHEST);
            rt_log("vk: the recording thread keeps the highest scheduling priority (NX2NAVI_RECORDER_PRIO=0 turns it off)");
        }
        // The recorder also gets a core of its own (plat_cpu_reserve): every other thread of the process, the guest's
        // cores, the audio, the shader and pipeline workers, is kept off it, so nothing of this process preempts the
        // recording. That core is then lost to the rest, so NX2NAVI_RECORDER_CORE=auto (the default) reserves it only
        // with 12 or more logical processors; 1 always, 0 never.
        const char *rc = cfg_value("NX2NAVI_RECORDER_CORE");
        unsigned ncpu = plat_cpu_usable();
        if (!strcmp(rc, "1") || (!strcmp(rc, "auto") && ncpu >= 12)) {
            int n = plat_cpu_reserve();
            if (n) rt_log("vk: the recording thread has a core of its own (%d logical processor%s of %u; "
                          "NX2NAVI_RECORDER_CORE=0 turns it off)", n, n > 1 ? "s" : "", ncpu);
            else rt_log("vk: could not reserve a core for the recording thread (NX2NAVI_RECORDER_CORE)");
        }
        rec_thread = plat_thread_id();
    }
    if (rec_thread && plat_thread_id() != rec_thread) {   // the priority and the core stay with the first thread
        static int said;
        if (!said++) rt_log("vk: a submission is recorded on another thread than the first one; it runs without the "
                            "recorder's priority and core");
    }
    perf_lock_timing_here();   // NX2NAVI_LOCK_TIME
    if (prof_replay_tid != plat_thread_id()) perf_replay_thread();   // the profiler follows the thread replaying now
    perf_rec_waits = rec_wait_totals;
    VkExec *ex = &X;
    if (!x_ready) state_reset(ex), vk_dump_init(), x_ready = 1;
    const tex_t *keep[9];   // the render targets carry over to this submission (set_rt compares them by pointer)
    memcpy(keep, ex->rt.color, sizeof ex->rt.color);
    keep[8] = ex->rt.depth;
    nvn_pins_sweep(nvn_submit_count(), keep, 9);
    ex->cb = cb;
    ex->fence = fence;
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->mark = NULL;
    ex->mark_passes = ex->mark_dispatches = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;   // images may have changed since the last command buffer
    ex->nviews = 0;
    ex->barrier_at = -1;
    ex->nfbl = 0;   // the previous command buffer ended with a full barrier: every image is in GENERAL
    vk_exec_frame.submits++;
    vk_exec_submit_seq++;
    split_draws = part_start = 0;
    split_at = split_n < 0 ? 0 : split_n ? split_n : INT_MAX;   // 0 until maybe_split reads the settings
    part_t0 = kern_now_ns();
    x_here = 1;
    int evo = evo_enabled() && gfx_submit_queue;
    int slot = evo ? evq_slot(gfx_submit_queue) : -1;
    if (evo && slot < 0) evo = 0;
    int synced = n || (evo_pending && held_sync() && vk_exec_held_ready());   // held_sync
    if (synced) {
        vk_mem_mirror_begin();
        if (evo_pending) {   // the uniform snapshot covers what held submissions bind too: they may go on here
            int tot = n;
            for (int s = 0; s < EVO_QUEUES; s++)
                for (Cont *c = evq[s].head; c; c = c->next) tot += c->n;
            uint64_t *all = malloc(sizeof *all * (size_t)tot);
            if (!all) rt_fatal("vk: out of memory (snapshot handles)");
            if (n) memcpy(all, handles, sizeof *all * (size_t)n);
            int k = n;
            for (int s = 0; s < EVO_QUEUES; s++)
                for (Cont *c = evq[s].head; c; c = c->next) memcpy(all + k, c->h, sizeof *all * (size_t)c->n), k += c->n;
            vk_usnap_begin(all, tot, fence);
            free(all);
        } else vk_usnap_begin(handles, n, fence);
    }
    for (int i = 0; i < n; i++)
        if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
    if (evo_pending) evo_resume(ex);   // signals the CPU gave since, and holds past CONT_MAX
    if (evo && evq[slot].head) {   // the queue waits: this submission lines up behind it
        if (n) cont_push(slot, cont_new(handles, n, 0, NULL)), evo_held++;
    } else {
        evo_queue = evo ? gfx_submit_queue : 0;
        for (int i = 0; i < n; i++) {
            const op_hdr *stop = walk(ex, nvn_op_first(handles[i]), 0);
            if (!stop) continue;
            cont_push(slot, cont_new(handles, n, i, stop));   // the rest of this submission waits for its signal
            evo_held++;
            break;
        }
        evo_queue = 0;
    }
    vk_exec_pass_end(ex);
    x_here = 0;
    if (ex->recorded) vk_exec_barrier(ex), vk_exec_pool_marks(ex);   // later submissions see this one's writes
    // The write-backs in a part of their own, after one that signals the executed timeline (vk_sync.c exec_tl): the
    // wait after a frame's first submission then ends when the submission's passes are through, not its write-backs.
    if (synced && ex->recorded && vk_sync_wb_split() && vk_mem_mirror_wb_pending() && vk_exec_pool_split(ex->fence)) {
        ex->cb = vk_sync_split_exec();
        ex->dirty = VK_EXEC_DIRTY_ALL;
        vk_exec_draw_new_cb();
    }
    if (synced) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);   // VRAM mirror bytes the GPU wrote go back to guest memory
    if (ex->nviews) {
        ViewList *l = malloc(sizeof *l + sizeof(VkImageView) * (size_t)ex->nviews);
        if (!l) rt_fatal("vk: out of memory");
        l->n = ex->nviews;
        for (int i = 0; i < ex->nviews; i++) l->v[i] = ((ViewEnt *)ex->views)[i].view;
        vk_completion_add(fence, free_views, l);
        ex->nviews = 0;
    }
    ex->cb = VK_NULL_HANDLE;
    if (n) n_subs++;
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f (%.2f first parts sent while the GPU was idle, %.2f "
               "splits inside a pass); event order: held %llu, resumed %llu (%llu without their signal), switches %llu, "
               "held now %d", (double)(n_subs + n_parts) / (double)n_subs, (double)n_idle_parts / (double)n_subs,
               (double)n_pass_splits / (double)n_subs, (unsigned long long)evo_held, (unsigned long long)evo_resumed,
               (unsigned long long)evo_forced, (unsigned long long)evo_switches, evo_pending);
        evo_held = evo_resumed = evo_forced = evo_switches = 0;
        n_subs = n_parts = n_idle_parts = n_pass_splits = 0;
    }
    return ex->recorded - part_start;   // what the last part holds
}
