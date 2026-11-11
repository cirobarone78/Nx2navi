// Additional screen-space AO for Ocarina's forward frame. The decoded spatial-upscaler picture or temporal-upscaler
// output is RGBA16F, before the HUD; unlike vk_contact.c this path needs neither a G-buffer nor a sun cascade.
// The scene context and projection follow vk_dlaa.c vk_dlaa_draw. Camera bytes use the submission-snapshot helper
// (including its guarded CPU-memory fallback), never a guest pointer kept until a later frame. All scene state is
// guarded by the texture lock, including image retirement on another thread. GPU resources are created lazily,
// and a missing/unsupported input leaves colour alone.
#include "vk_ao.h"

#include <math.h>
#include <string.h>

#include "ambient_occlusion_spv.h"
#include "cfg.h"
#include "host.h"
#include "vk_exec.h"

// BEGIN AO state
struct AoCamera { VkImage depth; VkImageView view; uint64_t address; unsigned draws; float projection[16]; };
static struct {
    VkImage depth;
    VkImageView view;
    uint32_t width, height;
    struct AoCamera cameras[8];
    int count, done, ambiguous;
} S;
// END AO state

static struct {
    int tried, ok, logged;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkSampler sampler;
} A;

static int ao_mode(const char *value) {
    return !strcmp(value, "ssao") ? 1 : !strcmp(value, "hbao") ? 2 : 0;
}

// The shader reconstructs a perspective projection with no XY shear or clip-W translation. Keep offsets (wide
// framing), reject orthographic/invalid matrices rather than turn a bad camera into dark pixels or NaNs.
static int projection_ok(const float p[16]) {
    for (int i = 0; i < 16; i++) if (!isfinite(p[i])) return 0;
    return p[0] > 1e-5f && p[5] > 1e-5f && p[11] > 0 &&
           fabsf(p[14] + 1) < 1e-6f && fabsf(p[1]) < 1e-6f && fabsf(p[3]) < 1e-6f &&
           fabsf(p[4]) < 1e-6f && fabsf(p[7]) < 1e-6f && fabsf(p[8]) < 1e-6f &&
           fabsf(p[9]) < 1e-6f && fabsf(p[12]) < 1e-6f && fabsf(p[13]) < 1e-6f && fabsf(p[15]) < 1e-6f;
}

static void scene_reset(void) { memset(&S, 0, sizeof S); }

// A smaller effects/reflection depth never replaces the main scene. Among cameras drawing into that depth, use the
// most frequent scene context, not whichever small reflection happened to bind last. Bounded storage, no per-draw allocation.
static void scene_note(VkImage depth, VkImageView view, uint32_t width, uint32_t height, uint64_t address,
                       const float projection[16]) {
    if (S.done || (S.depth && (width < S.width || height < S.height))) return;
    if (!S.depth || width > S.width || height > S.height) {
        S.count = 0;   // replace candidates, not the frame's ambiguity or dispatch guards
        S.depth = depth, S.view = view, S.width = width, S.height = height;
    }
    if (S.depth != depth) S.ambiguous = 1;
    int i = 0;
    while (i < S.count && (S.cameras[i].address != address || S.cameras[i].depth != depth)) i++;
    if (i == S.count) {
        if (S.count == 8) return;
        S.cameras[S.count++] = (struct AoCamera){.depth = depth, .view = view, .address = address};
    }
    S.cameras[i].draws++;
    memcpy(S.cameras[i].projection, projection, sizeof S.cameras[i].projection);
}

static const struct AoCamera *scene_pick_depth(VkImage expected) {
    if (!expected && S.ambiguous) return NULL;   // no safe spatial association after two full-size scene depths
    int best = -1;
    for (int i = 0; i < S.count; i++) {
        if (expected && S.cameras[i].depth != expected) continue;
        if (best < 0 || S.cameras[i].draws > S.cameras[best].draws) best = i;
    }
    return best < 0 ? NULL : &S.cameras[best];
}

static const struct AoCamera *scene_pick(void) { return scene_pick_depth(VK_NULL_HANDLE); }

void vk_ao_draw(VkExec *ex) {
    if (!ao_mode(cfg_value("NX2NAVI_AO"))) return;
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    const p_bind_buf *b = &ex->ubo[0][0];
    if (!b->addr || b->size != 0x3500 || !z->image || !z->img || !c->image || ex->pass_ncolor != 1 ||
        z->format != VK_FORMAT_D32_SFLOAT || (c->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
        c->format != VK_FORMAT_R16G16B16A16_SFLOAT) || z->width < 320 || !z->height ||
        c->width != z->width || c->height != z->height || z->samples > 1 || c->samples > 1 ||
        z->level || z->base_layer || z->layers != 1 || z->is3d || c->is3d ||
        !ex->pipe.ds.dt || !ex->pipe.ds.dw || ex->depth_range[0] != 0 || ex->depth_range[1] != 1) return;
    if (ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != z->width ||
                       ex->vp[0][3] != z->height)) return;
    float camera[44];
    if (!vk_usnap_read(b->addr, sizeof camera, camera) || !projection_ok(camera + 28)) return;
    vk_texture_lock();
    if (!S.done && !z->img->gone && !z->img->fb_learned &&
        (z->img->usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        (!S.depth || (z->width >= S.width && z->height >= S.height))) {
        VkImageView view = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT,
                                               0, 0, 1, 0);
        if (view) scene_note(z->image, view, z->width, z->height, b->addr, camera + 28);
    }
    vk_texture_unlock();
}

void vk_ao_frame(void) {
    vk_texture_lock();
    scene_reset();
    vk_texture_unlock();
}

void vk_ao_image_gone(VkImage image) {
    for (int i = 0; i < S.count;) {
        if (S.cameras[i].depth != image) { i++; continue; }
        memmove(&S.cameras[i], &S.cameras[i + 1], (size_t)(S.count - i - 1) * sizeof S.cameras[0]);
        S.count--;
    }
    S.depth = S.count ? S.cameras[0].depth : VK_NULL_HANDLE;
    S.view = S.count ? S.cameras[0].view : VK_NULL_HANDLE;
    // Retirement itself clears neither done nor ambiguity: it is not a new frame.
}

static void resources_free(void) {
    VkCtx *c = vk_ctx();
    if (A.pipeline) vkDestroyPipeline(c->device, A.pipeline, NULL);
    if (A.sampler) vkDestroySampler(c->device, A.sampler, NULL);
    if (A.layout) vkDestroyPipelineLayout(c->device, A.layout, NULL);
    if (A.dsl) vkDestroyDescriptorSetLayout(c->device, A.dsl, NULL);
    A.pipeline = VK_NULL_HANDLE, A.sampler = VK_NULL_HANDLE, A.layout = VK_NULL_HANDLE, A.dsl = VK_NULL_HANDLE;
}

static int init(void) {
    if (A.tried) return A.ok;
    A.tried = 1;
    VkCtx *c = vk_ctx();
    A.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &A.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof ambient_occlusion_spv, .pCode = ambient_occlusion_spv};
    VkShaderModule module = VK_NULL_HANDLE;
    int ok = A.push && vkCreateDescriptorSetLayout(c->device, &di, NULL, &A.dsl) == VK_SUCCESS &&
        vkCreatePipelineLayout(c->device, &li, NULL, &A.layout) == VK_SUCCESS &&
        vkCreateSampler(c->device, &si, NULL, &A.sampler) == VK_SUCCESS &&
        vkCreateShaderModule(c->device, &mi, NULL, &module) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module, .pName = "main"}, .layout = A.layout};
        ok = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pi, NULL, &A.pipeline) == VK_SUCCESS;
    }
    if (module) vkDestroyShaderModule(c->device, module, NULL);
    if (!ok) resources_free(), rt_log("vk: ambient occlusion unavailable: compute pipeline creation failed");
    return A.ok = ok;
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

void vk_ao_record(VkCommandBuffer cb, VkImageView scene, uint32_t width, uint32_t height, VkImage expected_depth) {
    int mode = ao_mode(cfg_value("NX2NAVI_AO")), strength = cfg_int("NX2NAVI_AO_STRENGTH");
    if (!mode || strength <= 0 || !scene || !width || !height) return;
    vk_texture_lock();
    const struct AoCamera *camera = expected_depth ? scene_pick_depth(expected_depth) : scene_pick();
    if (S.done || !camera || !camera->view ||
        fabs((double)width * S.height / ((double)height * S.width) - 1) > 0.01 || !init()) {
        vk_texture_unlock();
        return;
    }
    int radius = cfg_int("NX2NAVI_AO_RADIUS"), quality = cfg_int("NX2NAVI_AO_QUALITY");
    const float *p = camera->projection;
    float pc[16] = {p[0], p[5], p[2], p[6], p[10], p[11], p[14], p[15],
        (float)width, (float)height, 1.0f / width, 1.0f / height,
        (float)mode, (strength > 100 ? 100 : strength) / 100.0f,
        (float)(radius < 1 ? 1 : radius > 100 ? 100 : radius), (float)(quality < 0 ? 0 : quality > 2 ? 2 : quality)};
    VkDescriptorImageInfo images[2] = {{A.sampler, camera->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, scene, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &images[1]}};
    barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, A.pipeline);
    rec_push(A.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, A.layout, 0, 2, writes);
    vkCmdPushConstants(cb, A.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (width + 7) / 8, (height + 7) / 8, 1);
    barrier(cb);
    S.done = 1;
    if (!A.logged) {
        A.logged = 1;
        rt_log("vk: experimental ambient occlusion applied, depth %ux%u, picture %ux%u", S.width, S.height, width, height);
    }
    vk_texture_unlock();
    vk_exec_draw_new_cb();   // the replay must bind its compute pipeline/descriptors again
}

void vk_ao_release(void) {
    vk_ao_frame();
    if (vk_ctx() && A.tried) resources_free();
    memset(&A, 0, sizeof A);
}
