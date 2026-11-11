// Capture a sampled full-viewport depth and its projection in replay order.
// The caller must copy the frame into the present job before resetting replay state.
#include "vk_stereo.h"
#include "cfg.h"
#include "nvn_api.h"
#include <math.h>
#include <string.h>

#define STEREO_NVN_FS 1 // VkExec texture arrays use NVN stages, not GFX_STAGE_* reflection stages.

/* BEGIN STEREO SELECTION */
#define STEREO_IMAGES 64
static struct {
    StereoFrame frame;
    VkImage seed, images[STEREO_IMAGES];
    unsigned n;
    int ambiguous;
} S;

static int scene_has(VkImage image) {
    for (unsigned i = 0; image && i < S.n; ++i) if (S.images[i] == image) return 1;
    return 0;
}

static void scene_forget(VkImage image) {
    for (unsigned i = 0; image && i < S.n; ++i)
        if (S.images[i] == image) { S.images[i] = S.images[--S.n]; return; }
}

static void scene_copy(VkImage src, VkImage dst) {
    if (!dst || src == dst) return;
    int known = scene_has(src);
    scene_forget(dst);
    if (!known || S.ambiguous) return;
    if (S.n == STEREO_IMAGES) { S.ambiguous = 1; return; }
    S.images[S.n++] = dst;
}

static void scene_note(VkImage color, VkImage depth, VkImageView view, unsigned width, unsigned height, const float *projection) {
    if (S.ambiguous) return;
    if (!S.frame.depth) {
        S.frame = (StereoFrame){.depth = depth, .view = view, .width = width, .height = height};
        S.seed = color;
        S.images[S.n++] = color;
    } else if (S.frame.depth != depth || S.seed != color || S.frame.width != width || S.frame.height != height) {
        S.ambiguous = 1;
        return;
    }
    // This draw can change the shared depth. Older copies no longer represent this geometry generation.
    S.images[0] = color;
    S.n = 1;
    memcpy(S.frame.projection, projection, sizeof S.frame.projection);
}

static void scene_gone(VkImage image) {
    if (!image) return;
    if (S.frame.depth == image || S.seed == image || scene_has(image)) S.ambiguous = 1;
    scene_forget(image);
}

static StereoFrame scene_take(VkImage image) {
    StereoFrame frame = !S.ambiguous && scene_has(image) ? S.frame : (StereoFrame){0};
    memset(&S, 0, sizeof S);
    return frame;
}
/* END STEREO SELECTION */

static int projection_ok(const float p[16]) {
    for (int i = 0; i < 16; i++) if (!isfinite(p[i])) return 0;
    return p[0] > 1e-5f && p[5] > 1e-5f && p[11] > 0 &&
           fabsf(p[14] + 1) < 1e-6f && fabsf(p[1]) < 1e-6f && fabsf(p[3]) < 1e-6f &&
           fabsf(p[4]) < 1e-6f && fabsf(p[7]) < 1e-6f && fabsf(p[8]) < 1e-6f &&
           fabsf(p[9]) < 1e-6f && fabsf(p[12]) < 1e-6f && fabsf(p[13]) < 1e-6f && fabsf(p[15]) < 1e-6f;
}

static int scene_captured;
int vk_stereo_enabled(void) {
    const char *mode = cfg_value("NX2NAVI_STEREO");
    return mode && (!strcmp(mode, "sbs") || !strcmp(mode, "tab") || !strcmp(mode, "anaglyph"));
}

static void scene_draw(VkExec *ex) {
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    const p_bind_buf *b = &ex->ubo[0][0];
    if (!b->addr || b->size != 0x3500 || !z->image || !z->img || !c->image || ex->pass_ncolor != 1 ||
        z->format != VK_FORMAT_D32_SFLOAT || (c->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
        c->format != VK_FORMAT_R16G16B16A16_SFLOAT) || z->width < 320 || !z->height ||
        c->width != z->width || c->height != z->height || z->samples > 1 || c->samples > 1 ||
        z->level || z->base_layer || z->layers != 1 || c->level || c->base_layer || c->layers != 1 || z->is3d || c->is3d ||
        !ex->pipe.ds.dt || !ex->pipe.ds.dw || ex->depth_range[0] != 0 || ex->depth_range[1] != 1) return;
    if (ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != z->width ||
                       ex->vp[0][3] != z->height)) return;
    float camera[44];
    if (!vk_usnap_read(b->addr, sizeof camera, camera) || !projection_ok(camera + 28)) return;
    const float *p = camera + 28;
    const float projection[4] = {p[10], p[11], p[14], p[15]};
    vk_texture_lock();
    if (!z->img->gone && !z->img->fb_learned && (z->img->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        VkImageView view = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT,
                                               0, 0, 1, 0);
        if (view) { scene_note(c->image, z->image, view, z->width, z->height, projection); scene_captured = 1; }
    }
    vk_texture_unlock();
}

static void scene_associate_draw(VkExec *ex, const VkPipeInfo *info) {
    if (!vk_stereo_enabled()) return;
    scene_captured = 0;
    scene_draw(ex);
    if (scene_captured) return;
    const VkExecTarget *dst = &ex->color[0];
    if (ex->pass_ncolor == 1 && !ex->pipe.ds.dw) {
        vk_texture_lock();
        // Blended UI/effects retain the underlying scene contribution; their own depth is not reconstructed.
        int overlay = (ex->pipe.color.blend_en & 1) && scene_has(dst->image);
        vk_texture_unlock();
        if (overlay) return;
        // Follow the observed single-source screen-space pass, not stale handles left in unused texture slots.
        // This is a bounded post-process association, not a proof that an arbitrary shader preserves UVs.
        int full = !ex->has_vp || (ex->vp[0][0] == 0 && ex->vp[0][1] == 0 &&
                    ex->vp[0][2] == dst->width && ex->vp[0][3] == dst->height);
        full &= !ex->has_scissor || (ex->scissor.x <= 0 && ex->scissor.y <= 0 &&
                    (int64_t)ex->scissor.x + ex->scissor.w >= dst->width &&
                    (int64_t)ex->scissor.y + ex->scissor.h >= dst->height);
        int slot = -1, ntex = 0;
        for (uint32_t i = 0; info && i < info->n_bindings; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            if (b->kind != GFX_RES_TEX || b->stage != GFX_STAGE_FS || !b->res) continue;
            ntex += b->count ? b->count : 1;
            if (b->res->type == 3 && !b->res->separate && !b->res->pool) slot = b->res->slot;
        }
        if (full && info && !info->vs_inputs && !ex->pipe.ds.dt && !(ex->pipe.color.blend_en & 1) &&
            !dst->level && !dst->base_layer && dst->layers == 1 && !dst->is3d && dst->samples <= 1 &&
            ntex == 1 && slot >= 0 && slot < 128 && ex->tex[STEREO_NVN_FS][slot]) {
            view_t view; int has_view = 0;
            const VkTexImage *src = vk_texture_by_id(NVN_HANDLE_TEX(ex->tex[STEREO_NVN_FS][slot]), &view, &has_view);
            if (src && !src->gone && !has_view && src->samples == 1 && src->aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
                src->width == dst->width && src->height == dst->height) {
                vk_stereo_copy(src->image, dst->image);
                return;
            }
        }
    }
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; ++i) vk_stereo_forget(ex->color[i].image);
    if (ex->pipe.ds.dw) vk_stereo_forget(ex->depth.image);
}

void vk_stereo_images(VkExec *ex, const VkPipeInfo *info) {
    if (!vk_stereo_enabled() || !info) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        if (b->kind != GFX_RES_IMG) continue;
        const GfxShaderRes *r = b->res;
        if (r && !(r->flags & 4)) continue; // reflection's image-store usage flag
        uint32_t count = b->count ? b->count : 1;
        if (!r || r->pool || r->slot < 0 || r->slot >= 32 || count > 32u - (uint32_t)r->slot) {
            vk_texture_lock(); S.ambiguous = 1; vk_texture_unlock();
            continue; // an unresolved write must not retain any depth association
        }
        // Match vk_exec_draw.c nvn_stage_of; reflection stages and NVN binding indices differ.
        int stage = b->stage == GFX_STAGE_VS ? 0 : b->stage == GFX_STAGE_FS ? 1 :
                    b->stage == GFX_STAGE_TS ? 6 : b->stage == GFX_STAGE_MS ? 7 : 5;
        for (uint32_t j = 0; j < count; j++) {
            uint64_t handle = ex->img[stage][r->slot + j];
            const VkTexImage *image = handle ? vk_texture_by_id(NVN_HANDLE_TEX(handle), NULL, NULL) : NULL;
            if (image) vk_stereo_forget(image->image);
            else if (handle) { vk_texture_lock(); S.ambiguous = 1; vk_texture_unlock(); }
        }
    }
}

void vk_stereo_draw(VkExec *ex, const VkPipeInfo *info) {
    if (!vk_stereo_enabled()) return;
    scene_associate_draw(ex, info);
    vk_stereo_images(ex, info); // apply unknown image writes after any attachment-based association
}

void vk_stereo_copy(VkImage src, VkImage dst) {
    if (!vk_stereo_enabled()) return;
    vk_texture_lock();
    scene_copy(src, dst);
    vk_texture_unlock();
}

void vk_stereo_temporal(VkImage src, VkImage depth, VkImage dst) {
    if (!vk_stereo_enabled()) return;
    vk_texture_lock();
    if (depth == S.frame.depth && depth) scene_copy(src, dst);
    else scene_forget(dst);
    vk_texture_unlock();
}

void vk_stereo_forget(VkImage image) {
    if (!vk_stereo_enabled()) return;
    vk_texture_lock();
    scene_forget(image);
    if (image && image == S.frame.depth) S.ambiguous = 1;
    vk_texture_unlock();
}

StereoFrame vk_stereo_frame(VkImage present) {
    vk_texture_lock();
    StereoFrame frame = scene_take(present);
    vk_texture_unlock();
    return frame;
}

void vk_stereo_image_gone(VkImage image) { scene_gone(image); }
