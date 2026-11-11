// Experimental depth-reprojected stereo at SDR presentation, before the normal scaler and settings overlay.
// The replay thread supplies one immutable frame; vk_present_drain parks later writers until this submission is queued.
// Game images/views use the existing deferred retirement timeline. Scratch images are private to a completed present slot.
#include "vk_stereo_present.h"

#include <math.h>
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "stereo_spv.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_fg.h"

#define STEREO_SLOTS 8
struct StereoSlot {
    VkImage image;
    VmaAllocation allocation;
    VkImageView view, source;
    uint32_t width, height;
};
static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkSampler sampler;
    struct StereoSlot slots[STEREO_SLOTS];
    StereoFrame frame;
} P;

struct StereoPush {
    int32_t crop[4], output[4];
    float projection[4], options[4];
};
_Static_assert(sizeof(struct StereoPush) == 64, "stereo shader push ABI");

static int stereo_mode(void) {
    const char *v = cfg_value("NX2NAVI_STEREO");
    return !strcmp(v, "sbs") ? 1 : !strcmp(v, "tab") ? 2 : !strcmp(v, "anaglyph") ? 3 : 0;
}

static int compatible(void) {
    return !vk_hdr_wanted() && !vk_hdr_active() && !vk_hdr_native() &&
           !strcmp(cfg_value("NX2NAVI_FRAMEGEN"), "off") && !vk_fg_active() &&
           !vk_dlssg_active() && !vk_ngxfg_on();
}

void vk_stereo_present_frame(StereoFrame frame) { P.frame = frame; }

static void free_slot(VkCtx *c, struct StereoSlot *s) {
    if (s->source) vkDestroyImageView(c->device, s->source, NULL);
    if (s->view) vkDestroyImageView(c->device, s->view, NULL);
    if (s->image) vmaDestroyImage(c->vma, s->image, s->allocation);
    memset(s, 0, sizeof *s);
}

static void free_pipeline(VkCtx *c) {
    if (P.pipeline) vkDestroyPipeline(c->device, P.pipeline, NULL);
    if (P.sampler) vkDestroySampler(c->device, P.sampler, NULL);
    if (P.layout) vkDestroyPipelineLayout(c->device, P.layout, NULL);
    if (P.descriptors) vkDestroyDescriptorSetLayout(c->device, P.descriptors, NULL);
    P.pipeline = VK_NULL_HANDLE, P.sampler = VK_NULL_HANDLE;
    P.layout = VK_NULL_HANDLE, P.descriptors = VK_NULL_HANDLE;
}

static int init(VkCtx *c) {
    if (P.tried) return P.ok;
    P.tried = 1;
    P.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 3, .pBindings = b};
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct StereoPush)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &P.descriptors, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof stereo_spv, .pCode = stereo_spv};
    VkShaderModule module = VK_NULL_HANDLE;
    int ok = P.push && vkCreateDescriptorSetLayout(c->device, &di, NULL, &P.descriptors) == VK_SUCCESS &&
        vkCreatePipelineLayout(c->device, &li, NULL, &P.layout) == VK_SUCCESS &&
        vkCreateSampler(c->device, &si, NULL, &P.sampler) == VK_SUCCESS &&
        vkCreateShaderModule(c->device, &mi, NULL, &module) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = P.layout};
        ok = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pi, NULL, &P.pipeline) == VK_SUCCESS;
    }
    if (module) vkDestroyShaderModule(c->device, module, NULL);
    if (!ok) {
        free_pipeline(c);
        rt_log("vk: experimental stereo unavailable: compute pipeline creation failed");
    }
    return P.ok = ok;
}

static int ensure_image(VkCtx *c, struct StereoSlot *s, uint32_t w, uint32_t h) {
    if (s->image && s->width == w && s->height == h) return 1;
    // gfx_present.c waited for this slot's fence; no other slot shares these objects.
    free_slot(c, s);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &s->image, &s->allocation, NULL) != VK_SUCCESS) {
        s->image = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &s->view) != VK_SUCCESS) {
        s->view = VK_NULL_HANDLE;
        free_slot(c, s);
        return 0;
    }
    s->width = w, s->height = h;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageAspectFlags aspect,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da,
        .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

VkImage vk_stereo_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt,
                        int width, int height, const int crop[4], int output_width, int output_height) {
    int mode = stereo_mode();
    if (!mode || !compatible() || !cb || !src || !crop || slot >= STEREO_SLOTS || width <= 0 || height <= 0 ||
        output_width < 2 || output_height < 2 ||
        crop[0] < 0 || crop[1] < 0 || crop[2] < 2 || crop[3] < 2 || crop[2] > width || crop[3] > height ||
        crop[0] > width - crop[2] || crop[1] > height - crop[3]) return VK_NULL_HANDLE;
    int srgb = fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (!srgb && fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_B8G8R8A8_UNORM &&
        fmt != VK_FORMAT_R16G16B16A16_SFLOAT && fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return VK_NULL_HANDLE;
    VkCtx *c = vk_ctx();
    struct StereoSlot *s = &P.slots[slot];
    if (!init(c) || !ensure_image(c, s, (uint32_t)output_width, (uint32_t)output_height)) return VK_NULL_HANDLE;
    if (s->source) vkDestroyImageView(c->device, s->source, NULL), s->source = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &s->source) != VK_SUCCESS) {
        s->source = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    StereoFrame frame = P.frame;
    int depth = frame.depth && frame.view && frame.width && frame.height &&
        fabs((double)width * frame.height / ((double)height * frame.width) - 1.0) <= 0.01;
    int separation = cfg_int("NX2NAVI_STEREO_SEPARATION"), convergence = cfg_int("NX2NAVI_STEREO_CONVERGENCE");
    struct StereoPush pc = {.crop = {crop[0], crop[1], crop[2], crop[3]},
        .output = {output_width, output_height, mode, cfg_bool("NX2NAVI_STEREO_SWAP")},
        .options = {(separation < 0 ? 0 : separation > 100 ? 100 : separation) * 0.0003f,
                    (float)(convergence < 1 ? 1 : convergence > 1000 ? 1000 : convergence), (float)depth, 0}};
    if (depth) memcpy(pc.projection, frame.projection, sizeof pc.projection);
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, ALL, RW, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (depth) barrier(cb, frame.depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT,
                       ALL, RW, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, s->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT,
            ALL, RW, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo images[3] = {{P.sampler, s->source, VK_IMAGE_LAYOUT_GENERAL},
        {P.sampler, depth ? frame.view : s->source, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, s->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[3];
    for (int i = 0; i < 3; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = (uint32_t)i, .descriptorCount = 1,
        .descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &images[i]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.pipeline);
    rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.layout, 0, 3, writes);
    vkCmdPushConstants(cb, P.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, ((uint32_t)output_width + 7) / 8, ((uint32_t)output_height + 7) / 8, 1);
    barrier(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, CS,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL, RW);
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
            CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ALL, RW);
    if (depth) barrier(cb, frame.depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT,
                       CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ALL, RW);
    return s->image;
}

void vk_stereo_release(void) {
    VkCtx *c = vk_ctx();
    for (unsigned i = 0; i < STEREO_SLOTS; i++) free_slot(c, &P.slots[i]);
    free_pipeline(c);
    memset(&P, 0, sizeof P);
}
