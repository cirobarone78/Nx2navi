// --probe-fsr3: AMD FSR 3.1 upscaling with the built-in passes (vk_fsr3.c) on a synthetic scene, no game files. The
// scene is a zone plate (rings whose frequency rises with the radius, up to the render size's limit at its edge) with
// thin bars and a bright HDR patch, panning at a constant speed; each frame renders it on the CPU at the render size
// with the frame's jitter (Halton 2, 3 over FSR's phase count), with a flat depth and the motion vectors of the pan,
// and the passes upscale it, 960x540 to 1920x1080. After 48 frames the output is read back and compared with the scene
// rendered at the output size (4x4 supersampled, no jitter), as PSNR over the rings (rings_mask) and over the whole
// picture, against a bilinear enlargement of the same frame rendered without jitter. Runs: the pan; the pan with the
// jitter's sign reversed and with the motion vectors' sign reversed (the conventions vk_dlaa.c hands over: a pixel
// samples the scene at its centre minus the jitter, motion goes from the current frame to the previous one); the pan
// sharpened; the scene still, plain and sharpened. The checks, which amd_fidelityfx_vk.dll passes on the same scene
// with the same inputs (its output and the passes' differ by at most 3 of 255 there): no NaN or infinity; on the rings
// FSR 3.1 beats bilinear by 0.5 dB panning, sharpened FSR 3.1 by 0.5 dB still and plain FSR 3.1 is within 0.5 dB of it
// still; the reversed jitter does worse and the reversed motion much worse; no validation error from the passes (run
// with NX2NAVI_VK_VALIDATION=1). A pan slower than about 0.2 render pixels a frame (NX2NAVI_FSR3_PROBE_PAN=vx,vy) shows
// FSR 3.1's own behaviour there, the library's too: the picture runs about a third of a render pixel ahead of the
// scene and scores below bilinear. Then the GPU time of one upscale at the docked sizes (1707x960 -> 2560x1440, the
// Quality mode) with and without sharpening. NX2NAVI_FSR3_PROBE_PPM=<dir> writes the outputs, the bilinear ones and
// the references as PPM images.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_fsr3.h"

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    VkFormat fmt;
    uint32_t w, h;
} Img;

static int make_img(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags u, VkImageAspectFlags a) {
    memset(m, 0, sizeof *m);
    m->fmt = fmt, m->w = w, m->h = h;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {a, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &m->view) == VK_SUCCESS;
}

static void free_img(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->mem);
    memset(m, 0, sizeof *m);
}

typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    void *map;
} Buf;

static int make_buf(VkCtx *c, Buf *b, VkDeviceSize size, VkBufferUsageFlags u, int readback) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = u};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                           (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                     : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &b->buf, &b->mem, &info) != VK_SUCCESS) return 0;
    b->map = info.pMappedData;
    return 1;
}

static void free_buf(VkCtx *c, Buf *b) {
    if (b->buf) vmaDestroyBuffer(c->vma, b->buf, b->mem);
    memset(b, 0, sizeof *b);
}

// ---- the scene, in render pixels of the 960x540 render (s = how many output pixels make one of them)

static float scene(float x, float y) {
    const float cx = 480.0f, cy = 270.0f, k = 3.14159265f / 2400.0f;   // at r = 600 the rings reach 1 per pixel
    float dx = x - cx, dy = y - cy, r2 = dx * dx + dy * dy, v = 0.5f + 0.4f * cosf(k * r2);
    if (r2 > 600.0f * 600.0f) v = 0.5f;                                         // a flat field around the plate
    float bx = fmodf(x + 4000.0f, 37.0f), by = fmodf(y + 4000.0f, 53.0f);       // thin dark bars, 0.6 px wide
    if (bx < 0.6f || by < 0.6f) v *= 0.15f;
    float px = x - 140.0f, py = y - 120.0f;                                      // a bright patch: HDR values
    if (px > 0 && px < 40 && py > 0 && py < 25) v = 4.0f;
    return v;
}

static float scene_at(float x, float y, float t, float vx, float vy) { return scene(x - vx * t, y - vy * t); }

// Halton sequence value for index i (1-based) in base b
static float halton(int i, int b) {
    float f = 1, r = 0;
    for (; i > 0; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

static float h2f(uint16_t h) { return (float)*(_Float16 *)&h; }
static uint16_t f2h(float f) {
    _Float16 h = (_Float16)f;
    uint16_t u;
    memcpy(&u, &h, 2);
    return u;
}

typedef struct {
    Img color, depth, mv, out;
    Buf up_color, up_depth, up_mv, down;
    uint32_t w, h, ow, oh;
    float jx, jy;
    int reset, sharpen, readback, upload, ok;
    float sharpness;
    VkQueryPool qp;
    int timed;
} Frame;

static void general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags a) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {a, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void full(VkCommandBuffer cb) {
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    vkCmdPipelineBarrier2(cb, &d);
}

static void copy_in(VkCommandBuffer cb, Buf *b, Img *m, VkImageAspectFlags a) {
    VkBufferImageCopy r = {.imageSubresource = {a, 0, 0, 1}, .imageExtent = {m->w, m->h, 1}};
    vkCmdCopyBufferToImage(cb, b->buf, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void record(VkCommandBuffer cb, void *arg) {
    Frame *f = arg;
    if (f->upload == 2) {   // the first frame: the images into GENERAL
        general(cb, f->color.img, VK_IMAGE_ASPECT_COLOR_BIT);
        general(cb, f->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT);
        general(cb, f->mv.img, VK_IMAGE_ASPECT_COLOR_BIT);
        general(cb, f->out.img, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (f->upload) {
        full(cb);
        copy_in(cb, &f->up_color, &f->color, VK_IMAGE_ASPECT_COLOR_BIT);
        copy_in(cb, &f->up_depth, &f->depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        copy_in(cb, &f->up_mv, &f->mv, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    full(cb);
    if (f->timed) {
        vkCmdResetQueryPool(cb, f->qp, 0, 2);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f->qp, 0);
    }
    VkFfxImage fc = {f->color.img, f->color.fmt, f->w, f->h, 0, f->color.view},
               fz = {f->depth.img, f->depth.fmt, f->w, f->h, 0, f->depth.view},
               fm = {f->mv.img, f->mv.fmt, f->w, f->h, 1, f->mv.view},
               fo = {f->out.img, f->out.fmt, f->ow, f->oh, 1, f->out.view};
    f->ok = vk_fsr3_upscale(cb, &fc, &fz, &fm, &fo, f->jx, f->jy, f->reset, 16.67f, 1.0f, f->sharpen, f->sharpness);
    full(cb);
    if (f->timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f->qp, 1);
    if (f->readback) {
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {f->ow, f->oh, 1}};
        vkCmdCopyImageToBuffer(cb, f->out.img, VK_IMAGE_LAYOUT_GENERAL, f->down.buf, 1, &r);
        full(cb);
    }
}

static int submit_wait(Frame *f) {
    uint64_t v = vk_sync_probe_submit(record, f);
    return v && vk_sync_fence_wait(v, 10000000000ull) && f->ok;
}

// The inputs of frame i of a run: the scene at time i, sampled at each pixel's centre minus the jitter (sign: the
// jitter's sign as handed to the passes, -1 reversed); motion from this frame to the previous one is minus the pan.
static void fill_inputs(Frame *f, int i, float vx, float vy, int phases, float sign, float mvsign) {
    f->jx = halton(i % phases + 1, 2) - 0.5f, f->jy = halton(i % phases + 1, 3) - 0.5f;
    uint16_t *c = f->up_color.map, *m = f->up_mv.map;
    float *z = f->up_depth.map;
    for (uint32_t y = 0; y < f->h; y++)
        for (uint32_t x = 0; x < f->w; x++) {
            float sx = (float)x + 0.5f - sign * f->jx, sy = (float)y + 0.5f - sign * f->jy;
            uint16_t v = f2h(scene_at(sx, sy, (float)i, vx, vy)), one = f2h(1.0f);
            size_t k = (size_t)y * f->w + x;
            c[4 * k] = c[4 * k + 1] = c[4 * k + 2] = v, c[4 * k + 3] = one;
            m[2 * k] = f2h(-mvsign * vx), m[2 * k + 1] = f2h(-mvsign * vy);
            z[k] = 0.9f;
        }
    vmaFlushAllocation(vk_ctx()->vma, f->up_color.mem, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(vk_ctx()->vma, f->up_depth.mem, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(vk_ctx()->vma, f->up_mv.mem, 0, VK_WHOLE_SIZE);
}

static void write_ppm(const char *dir, const char *name, const float *v, uint32_t w, uint32_t h) {
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (size_t k = 0; k < (size_t)w * h; k++) {
        float x = v[k] / (1.0f + v[k]) * 2.0f;   // a soft roll-off so the HDR patch stays visible
        int b = (int)(powf(x < 0 ? 0 : x > 1 ? 1 : x, 1 / 2.2f) * 255.0f + 0.5f);
        fputc(b, fp), fputc(b, fp), fputc(b, fp);
    }
    fclose(fp);
    printf("probe-fsr3: wrote %s\n", path);
}

// PSNR over [0, 1] (the HDR patch clamped) of a against the reference, over the pixels where mask is set (NULL: all)
static double psnr(const float *a, const float *ref, const uint8_t *mask, size_t n) {
    double se = 0;
    size_t m = 0;
    for (size_t k = 0; k < n; k++) {
        if (mask && !mask[k]) continue;
        double x = a[k] > 1 ? 1 : a[k] < 0 ? 0 : a[k], y = ref[k] > 1 ? 1 : ref[k];
        se += (x - y) * (x - y), m++;
    }
    return se > 0 ? 10.0 * log10((double)m / se) : 99.0;
}

// The rings alone at time t: output pixels at least 2.5 render pixels from a bar, away from the HDR patch and the
// image's border. The bars are 0.6 render pixels wide: thinner than any reconstruction from render-size samples can
// keep at full contrast, so they weigh on the whole picture's PSNR the same for every upscaler (and favour the
// bilinear one, whose unjittered samples sit on them).
static void rings_mask(uint8_t *m, uint32_t w, uint32_t ow, uint32_t oh, float t, float vx, float vy) {
    const float s = (float)w / ow;
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            float sx = ((float)x + 0.5f) * s - vx * t, sy = ((float)y + 0.5f) * s - vy * t;
            float bx = fmodf(sx + 4000.0f, 37.0f), by = fmodf(sy + 4000.0f, 53.0f);
            int bar = bx < 2.5f || bx > 35.0f || by < 2.5f || by > 51.0f;
            int patch = sx > 130 && sx < 190 && sy > 110 && sy < 155;
            int edge = x < 20 || y < 20 || x >= ow - 20 || y >= oh - 20;
            m[(size_t)y * ow + x] = !bar && !patch && !edge;
        }
}

// One run of n frames; fills out[] with the last output (luma = the red channel, the scene is grey). Returns 0 when a
// frame failed or the output holds a NaN or an infinity.
static int run(Frame *f, int n, float vx, float vy, float sign, float mvsign, float *out) {
    int phases = (int)(8.0f * ((float)f->ow / f->w) * ((float)f->ow / f->w));
    for (int i = 0; i < n; i++) {
        fill_inputs(f, i, vx, vy, phases, sign, mvsign);
        f->reset = i == 0, f->readback = i == n - 1;
        if (!submit_wait(f)) {
            printf("probe-fsr3: FAIL frame %d was not recorded or not completed\n", i);
            return 0;
        }
        f->upload = 1;
    }
    vmaInvalidateAllocation(vk_ctx()->vma, f->down.mem, 0, VK_WHOLE_SIZE);
    const uint16_t *d = f->down.map;
    size_t bad = 0;
    for (size_t k = 0; k < (size_t)f->ow * f->oh; k++) {
        float r = h2f(d[4 * k]);
        if (!isfinite(r)) bad++;
        out[k] = r;
    }
    if (bad) printf("probe-fsr3: FAIL %zu output pixels are NaN or infinite\n", bad);
    return !bad;
}

// The reference for time t (the scene, 4x4 samples per output pixel) and the bilinear enlargement of the frame at t
// rendered without jitter (lr: render-size scratch)
static void reference(float *ref, float *bil, float *lr, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, float t,
                      float vx, float vy) {
    const float s = (float)w / ow;
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            float a = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    a += scene_at(((float)x + (i + 0.5f) / 4) * s, ((float)y + (j + 0.5f) / 4) * s, t, vx, vy);
            ref[(size_t)y * ow + x] = a / 16;
        }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) lr[(size_t)y * w + x] = scene_at(x + 0.5f, y + 0.5f, t, vx, vy);
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            float sx = ((float)x + 0.5f) * s - 0.5f, sy = ((float)y + 0.5f) * s - 0.5f;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float fx = sx - x0, fy = sy - y0;
#define LR(X, Y) lr[(size_t)((Y) < 0 ? 0 : (Y) >= (int)h ? (int)h - 1 : (Y)) * w + ((X) < 0 ? 0 : (X) >= (int)w ? (int)w - 1 : (X))]
            bil[(size_t)y * ow + x] = (LR(x0, y0) * (1 - fx) + LR(x0 + 1, y0) * fx) * (1 - fy) +
                                      (LR(x0, y0 + 1) * (1 - fx) + LR(x0 + 1, y0 + 1) * fx) * fy;
#undef LR
        }
}

int probe_fsr3(void) {
    if (!vk_init()) {
        printf("probe-fsr3: FAIL vk_init\n");
        return 1;
    }
    VkCtx *c = vk_ctx();
    // what an older validation layer reports about the device's creation (structures newer than it) is not ours
    long errors0 = vk_validation_errors_total();
    printf("probe-fsr3: GPU %s\n", c->props.deviceName);
    int bad = 0;
    if (!vk_fsr3_init()) {
        printf("probe-fsr3: FAIL the built-in passes are not available (see the log)\n");
        bad = 1;
    }
    const uint32_t w = 960, h = 540, ow = 1920, oh = 1080;   // Performance mode (2x)
    const int frames = 48;
    float vx = 1.0f, vy = 0.5f;   // the pan, render pixels per frame (NX2NAVI_FSR3_PROBE_PAN=vx,vy: another)
    const char *pan = cfg_get("NX2NAVI_FSR3_PROBE_PAN");
    if (pan && sscanf(pan, "%f,%f", &vx, &vy) == 2) printf("probe-fsr3: pan %.3f, %.3f\n", vx, vy);
    Frame f = {.w = w, .h = h, .ow = ow, .oh = oh, .upload = 2};
    VkImageUsageFlags in = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!bad && (!make_img(c, &f.color, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, in, VK_IMAGE_ASPECT_COLOR_BIT) ||
                 !make_img(c, &f.depth, w, h, VK_FORMAT_D32_SFLOAT, in, VK_IMAGE_ASPECT_DEPTH_BIT) ||
                 !make_img(c, &f.mv, w, h, VK_FORMAT_R16G16_SFLOAT, in, VK_IMAGE_ASPECT_COLOR_BIT) ||
                 !make_img(c, &f.out, ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT) ||
                 !make_buf(c, &f.up_color, (VkDeviceSize)w * h * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) ||
                 !make_buf(c, &f.up_depth, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) ||
                 !make_buf(c, &f.up_mv, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) ||
                 !make_buf(c, &f.down, (VkDeviceSize)ow * oh * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1))) {
        printf("probe-fsr3: FAIL test images\n");
        bad = 1;
    }
    size_t n = (size_t)ow * oh;
    float *ref = calloc(n, sizeof *ref), *bil = calloc(n, sizeof *bil), *sref = calloc(n, sizeof *sref),
          *sbil = calloc(n, sizeof *sbil), *lr = calloc((size_t)w * h, sizeof *lr), *o = calloc(n, sizeof *o);
    double p[6] = {0}, q[6] = {0}, p_bil = 0, p_sbil = 0, q_bil = 0, q_sbil = 0;
    uint8_t *mk = calloc(n, 1), *smk = calloc(n, 1);
    static const char *const what[6] = {"fsr3_native", "fsr3_jitter_reversed", "fsr3_motion_reversed",
                                        "fsr3_native_sharpened", "fsr3_static", "fsr3_static_sharpened"};
    const char *dir = cfg_get("NX2NAVI_FSR3_PROBE_PPM");
    if (!bad) {
        reference(ref, bil, lr, w, h, ow, oh, (float)(frames - 1), vx, vy);
        reference(sref, sbil, lr, w, h, ow, oh, 0, 0, 0);
        rings_mask(mk, w, ow, oh, (float)(frames - 1), vx, vy);
        rings_mask(smk, w, ow, oh, 0, 0, 0);
        p_bil = psnr(bil, ref, NULL, n), p_sbil = psnr(sbil, sref, NULL, n);
        q_bil = psnr(bil, ref, mk, n), q_sbil = psnr(sbil, sref, smk, n);
        // the pan as it should be, with the jitter reversed, with the motion reversed, sharpened; then still
        const float jit[6] = {1, -1, 1, 1, 1, 1}, mvs[6] = {1, 1, -1, 1, 1, 1};
        for (int r = 0; r < 6 && !bad; r++) {
            int still = r >= 4;
            f.sharpen = r == 3 || r == 5, f.sharpness = 0.5f;
            bad = !run(&f, frames, still ? 0 : vx, still ? 0 : vy, jit[r], mvs[r], o);
            if (!bad) {
                p[r] = psnr(o, still ? sref : ref, NULL, n), q[r] = psnr(o, still ? sref : ref, still ? smk : mk, n);
                write_ppm(dir, what[r], o, ow, oh);
            }
        }
        f.sharpen = 0;
    }
    if (!bad) {
        printf("probe-fsr3: %ux%u -> %ux%u, %d frames panning (%.2f, %.2f) px/frame, PSNR on the rings (whole picture): "
               "FSR 3.1 %.2f dB (%.2f), sharpened (0.5) %.2f (%.2f), jitter reversed %.2f (%.2f), motion reversed %.2f "
               "(%.2f); bilinear %.2f (%.2f)\n",
               w, h, ow, oh, frames, vx, vy, q[0], p[0], q[3], p[3], q[1], p[1], q[2], p[2], q_bil, p_bil);
        printf("probe-fsr3: the same scene still, PSNR on the rings (whole picture): FSR 3.1 %.2f dB (%.2f), sharpened "
               "%.2f (%.2f); bilinear %.2f (%.2f)\n", q[4], p[4], q[5], p[5], q_sbil, p_sbil);
        if (!(q[0] > q_bil + 0.5)) printf("probe-fsr3: FAIL FSR 3.1 is not better than bilinear while panning\n"), bad = 1;
        if (!(q[5] > q_sbil + 0.5)) printf("probe-fsr3: FAIL sharpened FSR 3.1 is not better than bilinear still\n"), bad = 1;
        if (!(q[4] > q_sbil - 0.5)) printf("probe-fsr3: FAIL FSR 3.1 is worse than bilinear still\n"), bad = 1;
        if (!(q[0] > q[1] + 0.5)) printf("probe-fsr3: FAIL the reversed jitter is not worse\n"), bad = 1;
        if (!(q[0] > q[2] + 5.0)) printf("probe-fsr3: FAIL the reversed motion is not much worse\n"), bad = 1;
        write_ppm(dir, "fsr3_reference", ref, ow, oh);
        write_ppm(dir, "bilinear", bil, ow, oh);
        write_ppm(dir, "fsr3_static_reference", sref, ow, oh);
        write_ppm(dir, "bilinear_static", sbil, ow, oh);
    }
    // GPU time at the docked sizes, the inputs uploaded once
    if (!bad) {
        Frame g = {.w = 1707, .h = 960, .ow = 2560, .oh = 1440, .upload = 2};
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
        int ok = vkCreateQueryPool(c->device, &qi, NULL, &g.qp) == VK_SUCCESS &&
                 make_img(c, &g.color, g.w, g.h, VK_FORMAT_R16G16B16A16_SFLOAT, in, VK_IMAGE_ASPECT_COLOR_BIT) &&
                 make_img(c, &g.depth, g.w, g.h, VK_FORMAT_D32_SFLOAT, in, VK_IMAGE_ASPECT_DEPTH_BIT) &&
                 make_img(c, &g.mv, g.w, g.h, VK_FORMAT_R16G16_SFLOAT, in, VK_IMAGE_ASPECT_COLOR_BIT) &&
                 make_img(c, &g.out, g.ow, g.oh, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
                 make_buf(c, &g.up_color, (VkDeviceSize)g.w * g.h * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) &&
                 make_buf(c, &g.up_depth, (VkDeviceSize)g.w * g.h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) &&
                 make_buf(c, &g.up_mv, (VkDeviceSize)g.w * g.h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0);
        if (ok) {
            int phases = (int)(8.0f * 1.5f * 1.5f);
            fill_inputs(&g, 0, vx, vy, phases, 1.0f, 1.0f);
            float ns = c->props.limits.timestampPeriod;
            for (int sh = 0; sh < 2 && ok; sh++) {
                g.sharpen = sh, g.sharpness = 0.5f;
                double sum = 0, best = 1e9;
                int runs = 0;
                for (int i = 0; i < 40 && ok; i++) {
                    g.jx = halton(i % phases + 1, 2) - 0.5f, g.jy = halton(i % phases + 1, 3) - 0.5f;
                    g.reset = i == 0, g.timed = i >= 8;
                    ok = submit_wait(&g);
                    g.upload = 0;
                    if (ok && g.timed) {
                        uint64_t ts[2];
                        if (vkGetQueryPoolResults(c->device, g.qp, 0, 2, sizeof ts, ts, 8,
                                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                            double ms = (double)(ts[1] - ts[0]) * ns / 1e6;
                            sum += ms, runs++;
                            if (ms < best) best = ms;
                        }
                    }
                }
                if (ok && runs)
                    printf("probe-fsr3: GPU time %ux%u -> %ux%u%s: %.3f ms average, %.3f ms best (%d frames)\n", g.w,
                           g.h, g.ow, g.oh, sh ? " sharpened" : "", sum / runs, best, runs);
            }
        }
        if (!ok) printf("probe-fsr3: FAIL the timing run\n"), bad = 1;
        free_img(c, &g.color), free_img(c, &g.depth), free_img(c, &g.mv), free_img(c, &g.out);
        free_buf(c, &g.up_color), free_buf(c, &g.up_depth), free_buf(c, &g.up_mv);
        if (g.qp) vkDestroyQueryPool(c->device, g.qp, NULL);
    }
    vk_sync_shutdown();
    free(ref), free(bil), free(sref), free(sbil), free(lr), free(o), free(mk), free(smk);
    free_img(c, &f.color), free_img(c, &f.depth), free_img(c, &f.mv), free_img(c, &f.out);
    free_buf(c, &f.up_color), free_buf(c, &f.up_depth), free_buf(c, &f.up_mv), free_buf(c, &f.down);
    vk_fsr3_release();
    vk_mem_shutdown();
    vk_shutdown();
    long errors = vk_validation_errors_total() - errors0;
    if (errors0) printf("probe-fsr3: %ld validation errors at device creation, before the passes, not counted\n", errors0);
    if (bad || errors) {
        printf("probe-fsr3: FAILED (%s, %ld validation errors)\n", bad ? "a check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-fsr3: passed (0 validation errors)\n");
    return 0;
}
