// Execute the complete presentation component with recorded Vulkan/VMA calls, never a driver.
#include "vk_stereo_present.c"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#define H(t, n) ((t)(uintptr_t)(n))
static VkCtx context;
static const char *mode = "sbs", *framegen = "off";
static int separation = 20, convergence = 100, swap_eyes;
static int hdr_wanted, hdr_active, hdr_native, fg_active, dlssg_active, ngx_on;
static int step, fail_step, live, dispatches, descriptor_calls, push_calls, barrier_calls;
static uintptr_t next_handle = 100;
static struct StereoPush recorded;
static VkImageView source_view, depth_view, output_view;
static uint32_t dispatch_x, dispatch_y;

const char *cfg_value(const char *key) { return !strcmp(key, "NX2NAVI_STEREO") ? mode : framegen; }
int cfg_int(const char *key) { return !strcmp(key, "NX2NAVI_STEREO_SEPARATION") ? separation : convergence; }
int cfg_bool(const char *key) { assert(!strcmp(key, "NX2NAVI_STEREO_SWAP")); return swap_eyes; }
int vk_hdr_wanted(void) { return hdr_wanted; }
int vk_hdr_active(void) { return hdr_active; }
int vk_hdr_native(void) { return hdr_native; }
int vk_fg_active(void) { return fg_active; }
int vk_dlssg_active(void) { return dlssg_active; }
int vk_ngxfg_on(void) { return ngx_on; }
VkCtx *vk_ctx(void) { return &context; }
void rt_log(const char *format, ...) { (void)format; }
static VkResult create(uintptr_t *value) {
    if (++step == fail_step) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *value = ++next_handle; live++; return VK_SUCCESS;
}
#define CREATE(name, type, info_type) \
VkResult name(VkDevice d, const info_type *info, const VkAllocationCallbacks *a, type *out) { \
    (void)d; (void)info; (void)a; uintptr_t value; VkResult r = create(&value); \
    if (r == VK_SUCCESS) *out = H(type, value); return r; }
CREATE(vkCreateDescriptorSetLayout, VkDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo)
CREATE(vkCreatePipelineLayout, VkPipelineLayout, VkPipelineLayoutCreateInfo)
CREATE(vkCreateSampler, VkSampler, VkSamplerCreateInfo)
CREATE(vkCreateShaderModule, VkShaderModule, VkShaderModuleCreateInfo)
CREATE(vkCreateImageView, VkImageView, VkImageViewCreateInfo)
VkResult vkCreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
    const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)cache; (void)a; assert(n == 1 && infos->layout && infos->stage.module);
    uintptr_t value; VkResult r = create(&value); if (r == VK_SUCCESS) *out = H(VkPipeline, value); return r;
}
#define DESTROY(name, type) \
void name(VkDevice d, type object, const VkAllocationCallbacks *a) { (void)d; (void)a; assert(object && live > 0); live--; }
DESTROY(vkDestroyDescriptorSetLayout, VkDescriptorSetLayout)
DESTROY(vkDestroyPipelineLayout, VkPipelineLayout)
DESTROY(vkDestroySampler, VkSampler)
DESTROY(vkDestroyShaderModule, VkShaderModule)
DESTROY(vkDestroyImageView, VkImageView)
DESTROY(vkDestroyPipeline, VkPipeline)
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ii, const VmaAllocationCreateInfo *ai,
    VkImage *image, VmaAllocation *allocation, VmaAllocationInfo *info) {
    (void)a; (void)ai; (void)info;
    assert(ii->format == VK_FORMAT_R16G16B16A16_SFLOAT && ii->extent.width && ii->extent.height);
    assert((ii->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ==
           (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    uintptr_t value; VkResult r = create(&value);
    if (r == VK_SUCCESS) { *image = H(VkImage, value); *allocation = H(VmaAllocation, value + 10000); } return r;
}
void vmaDestroyImage(VmaAllocator a, VkImage image, VmaAllocation allocation) {
    (void)a; assert(image && allocation && live > 0); live--;
}
static void fake_push(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                      uint32_t count, const VkWriteDescriptorSet *writes) {
    (void)cb; (void)bp; (void)layout; (void)set; (void)count; (void)writes;
}
static PFN_vkVoidFunction fake_dpa(VkDevice device, const char *name) {
    (void)device; assert(!strcmp(name, "vkCmdPushDescriptorSetKHR")); return (PFN_vkVoidFunction)fake_push;
}
PFN_vkGetDeviceProcAddr vk_dpa = fake_dpa;
void rec_push(PFN_vkCmdPushDescriptorSetKHR fn, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
              uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    assert(fn && cb && bp == VK_PIPELINE_BIND_POINT_COMPUTE && layout && !set && count == 3);
    for (uint32_t i = 0; i < count; i++) {
        assert(writes[i].dstBinding == i && writes[i].descriptorCount == 1);
        assert(writes[i].descriptorType == (i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
        assert(writes[i].pImageInfo->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    }
    source_view = writes[0].pImageInfo->imageView; depth_view = writes[1].pImageInfo->imageView;
    output_view = writes[2].pImageInfo->imageView; descriptor_calls++;
}
void rc_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    assert(cb && di->imageMemoryBarrierCount == 1);
    const VkImageMemoryBarrier2 *b = di->pImageMemoryBarriers;
    assert(b->image && b->newLayout == VK_IMAGE_LAYOUT_GENERAL && b->subresourceRange.layerCount == 1);
    barrier_calls++;
}
void rc_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe) {
    assert(cb && bp == VK_PIPELINE_BIND_POINT_COMPUTE && pipe);
}
void rc_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags flags,
                          uint32_t offset, uint32_t bytes, const void *data) {
    assert(cb && layout && flags == VK_SHADER_STAGE_COMPUTE_BIT && !offset && bytes == 64);
    memcpy(&recorded, data, bytes); push_calls++;
}
void rc_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    assert(cb && x && y && z == 1); dispatch_x = x; dispatch_y = y; dispatches++;
}
static void reset(void) {
    vk_stereo_release(); assert(!live);
    mode = "sbs"; framegen = "off"; separation = 20; convergence = 100; swap_eyes = 0;
    hdr_wanted = hdr_active = hdr_native = fg_active = dlssg_active = ngx_on = 0;
    step = fail_step = dispatches = descriptor_calls = push_calls = barrier_calls = 0;
}
static VkImage record(unsigned slot, VkFormat fmt, int width, int height, const int *crop) {
    return vk_stereo_record(H(VkCommandBuffer, 1), slot, H(VkImage, 2), fmt, width, height, crop, crop[2], crop[3]);
}
int main(void) {
    const int crop[4] = {2, 3, 64, 24};
    reset(); mode = "off"; assert(!record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop) && !step && !dispatches);
    mode = "unknown"; assert(!record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop) && !step);
    for (int i = 0; i < 7; i++) {
        reset(); int *guards[] = {&hdr_wanted, &hdr_active, &hdr_native, &fg_active, &dlssg_active, &ngx_on};
        if (i < 6) *guards[i] = 1; else framegen = "fsr3";
        assert(!record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop) && !step && !dispatches);
    }
    reset();
    assert(!record(8, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop) && !step);
    const int invalid[][4] = {{-1, 0, 64, 24}, {0, 0, 81, 24}, {79, 0, 2, 24}, {0, 0, 1, 24}, {0, 39, 64, 2}};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++)
        assert(!record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, invalid[i]) && !step);
    for (int fail = 1; fail <= 8; fail++) {
        reset(); fail_step = fail;
        assert(!record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop) && !dispatches);
        vk_stereo_release(); assert(!live);
    }
    reset();
    StereoFrame frame = {.depth = H(VkImage, 8), .view = H(VkImageView, 9), .width = 80, .height = 40,
                         .projection = {1.002f, 2.002f, -1, 0}};
    vk_stereo_present_frame(frame); frame.projection[0] = 99; // copy, never retain caller memory
    separation = 101; convergence = 2000; swap_eyes = 1;
    VkImage a = record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop);
    assert(a && dispatches == 1 && push_calls == 1 && descriptor_calls == 1 && barrier_calls == 6);
    assert(dispatch_x == 8 && dispatch_y == 3 && !memcmp(recorded.crop, crop, sizeof crop));
    assert(recorded.output[0] == 64 && recorded.output[1] == 24 && recorded.output[2] == 1 && recorded.output[3] == 1);
    assert(fabsf(recorded.options[0] - .03f) < 1e-6f && recorded.options[1] == 1000 && recorded.options[2] == 1);
    assert(recorded.projection[0] == 1.002f && depth_view == H(VkImageView, 9) && source_view != depth_view && output_view);
    mode = "tab"; separation = -1; convergence = -10;
    VkImage b = record(1, VK_FORMAT_R8G8B8A8_SRGB, 80, 40, crop);
    assert(b && a != b && recorded.output[2] == 2 && recorded.options[0] == 0 && recorded.options[1] == 1 && recorded.options[3] == 0);
    vk_stereo_present_frame((StereoFrame){0}); mode = "anaglyph";
    assert(record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop) == a);
    assert(recorded.output[2] == 3 && recorded.options[2] == 0 && source_view == depth_view);
    frame.height = 60; vk_stereo_present_frame(frame);
    assert(record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, crop) && recorded.options[2] == 0);
    const int resized[4] = {0, 0, 48, 16};
    assert(record(0, VK_FORMAT_R8G8B8A8_UNORM, 80, 40, resized) != a);
    assert(vk_stereo_record(H(VkCommandBuffer,1),0,H(VkImage,2),VK_FORMAT_R8G8B8A8_UNORM,80,40,crop,96,48));
    assert(recorded.output[0]==96 && recorded.output[1]==48 && dispatch_x==12 && dispatch_y==6);
    reset();
    puts("Stereo present: actual pipeline failures/cleanup, compatibility, crop, slots, descriptors, 64-byte ABI and dispatch passed");
    return 0;
}
