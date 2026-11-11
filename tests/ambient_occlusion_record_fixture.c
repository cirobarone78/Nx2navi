// Real renderer declarations; the generated translation unit inserts AO bodies below.
#include "vk_exec.h"
#include "cfg.h"
#include "host.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define H(type, n) ((type)(uintptr_t)(n))
static const char *mode = "ssao";
static int strength = 75, radius = 40, quality = 1, snap_ok = 1, pipeline_ok = 1;
static float snapshot[44];
static int locks, snapshots, views, init_calls, barriers, binds, pushes, constants, dispatches, resets;
static float pushed[16];
static VkImageView expected_view;
static uint32_t gx, gy, gz;
const char *cfg_value(const char *name) { (void)name; return mode; }
int cfg_int(const char *name) {
    if (!strcmp(name, "NX2NAVI_AO_STRENGTH")) return strength;
    if (!strcmp(name, "NX2NAVI_AO_RADIUS")) return radius;
    if (!strcmp(name, "NX2NAVI_AO_QUALITY")) return quality;
    assert(0); return 0;
}
int vk_usnap_read(uint64_t addr, uint64_t n, void *dst) {
    assert(!locks && addr == 0x1000 && n == sizeof snapshot); snapshots++;
    if (snap_ok) memcpy(dst, snapshot, sizeof snapshot);
    return snap_ok;
}
void vk_texture_lock(void) { locks++; }
void vk_texture_unlock(void) { assert(locks > 0); locks--; }
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat fmt, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks) {
    assert(locks && img && fmt == VK_FORMAT_D32_SFLOAT && type == VK_IMAGE_VIEW_TYPE_2D &&
           aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !level && !base && layers == 1 && !blocks);
    views++; return H(VkImageView, (uintptr_t)img->image + 1);
}
void vk_exec_draw_new_cb(void) { assert(!locks); resets++; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rc_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    assert(cb && locks && di->memoryBarrierCount == 1); barriers++;
}
void rc_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe) {
    assert(cb && locks && bp == VK_PIPELINE_BIND_POINT_COMPUTE && pipe); binds++;
}
void rec_push(PFN_vkCmdPushDescriptorSetKHR fn, VkCommandBuffer cb, VkPipelineBindPoint bp,
              VkPipelineLayout layout, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    (void)fn; assert(cb && locks && bp == VK_PIPELINE_BIND_POINT_COMPUTE && layout && !set && n == 2);
    assert(w[0].dstBinding == 0 && w[0].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
           w[0].pImageInfo->imageView == expected_view &&
           w[0].pImageInfo->imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    assert(w[1].dstBinding == 1 && w[1].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
           w[1].pImageInfo->imageView == H(VkImageView, 3) &&
           w[1].pImageInfo->imageLayout == VK_IMAGE_LAYOUT_GENERAL); pushes++;
}
void rc_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stage,
                           uint32_t offset, uint32_t n, const void *data) {
    assert(cb && locks && layout && stage == VK_SHADER_STAGE_COMPUTE_BIT && !offset && n == sizeof pushed);
    memcpy(pushed, data, n); constants++;
}
void rc_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    assert(cb && locks); gx=x; gy=y; gz=z; dispatches++;
}
/* AO_STATE */
static int init(void) {
    assert(locks); init_calls++;
    A.layout = H(VkPipelineLayout, 4); A.pipeline = H(VkPipeline, 5); A.sampler = H(VkSampler, 6);
    return pipeline_ok;
}
/* AO_FUNCTIONS */

static VkExec ex;
static VkTexImage image;
static void setup(void) {
    memset(&ex, 0, sizeof ex); memset(&image, 0, sizeof image);
    memset(snapshot, 0, sizeof snapshot);
    snapshot[28]=1.2f; snapshot[33]=1.7f; snapshot[30]=0.1f; snapshot[34]=-0.2f;
    snapshot[38]=1.002f; snapshot[39]=0.2f; snapshot[42]=-1.0f;
    ex.ubo[0][0].addr=0x1000; ex.ubo[0][0].size=0x3500;
    ex.color[0].image=H(VkImage, 7); ex.color[0].format=VK_FORMAT_R16G16B16A16_SFLOAT;
    ex.depth.image=H(VkImage, 1); ex.depth.img=&image; ex.depth.format=VK_FORMAT_D32_SFLOAT;
    ex.depth.layers=1; ex.depth.width=ex.color[0].width=1280; ex.depth.height=ex.color[0].height=720;
    ex.depth.samples=ex.color[0].samples=1; ex.pass_ncolor=1;
    ex.pipe.ds.dt=1; ex.pipe.ds.dw=1; ex.depth_range[1]=1;
    image.image=ex.depth.image; image.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    expected_view=H(VkImageView, 2);
    mode="ssao"; strength=75; radius=40; quality=1; snap_ok=1; pipeline_ok=1;
    vk_ao_frame(); assert(!locks);
    snapshots=views=init_calls=barriers=binds=pushes=constants=dispatches=resets=0;
}
static void record(VkImage depth) { vk_ao_record(H(VkCommandBuffer, 8), H(VkImageView, 3), 1280, 720, depth); }
#define REJECT(expr) do { setup(); expr; vk_ao_draw(&ex); assert(!S.count && !views); } while (0)
int main(void) {
    setup(); mode="off"; vk_ao_draw(&ex); record(0);
    assert(!snapshots && !views && !init_calls && !dispatches);
    setup(); vk_ao_draw(&ex); assert(snapshots==1 && views==1 && S.count==1 && scene_pick()->address==0x1000);
    REJECT(snap_ok=0);
    REJECT(snapshot[28]=NAN);
    REJECT(snapshot[42]=0);
    REJECT(ex.depth.samples=4);
    REJECT(ex.color[0].samples=4);
    REJECT(ex.depth.level=1);
    REJECT(ex.depth.base_layer=1);
    REJECT(ex.depth.layers=2);
    REJECT(ex.has_vp=1; ex.vp[0][2]=640; ex.vp[0][3]=720);
    REJECT(ex.depth.format=VK_FORMAT_D16_UNORM);
    REJECT(ex.color[0].format=VK_FORMAT_R8G8B8A8_UNORM);
    REJECT(image.usage=0);
    REJECT(image.gone=1);
    setup(); record(0); assert(!dispatches && !init_calls);
    setup(); vk_ao_draw(&ex); vk_ao_frame(); record(0); assert(!dispatches && !init_calls);
    setup(); vk_ao_draw(&ex); vk_ao_image_gone(H(VkImage,1)); record(0); assert(!dispatches && !init_calls);
    setup(); vk_ao_draw(&ex); record(H(VkImage,9)); assert(!dispatches && !init_calls);
    setup(); vk_ao_draw(&ex); vk_ao_record(H(VkCommandBuffer,8),H(VkImageView,3),1280,800,0); assert(!dispatches && !init_calls);
    setup(); vk_ao_draw(&ex); strength=0; record(0); assert(!dispatches && !init_calls);
    setup(); vk_ao_draw(&ex); pipeline_ok=0; record(0); assert(init_calls==1 && !dispatches && !pushes);
    setup(); vk_ao_draw(&ex); snapshot[28]=9; mode="hbao"; strength=150; radius=-4; quality=7; record(H(VkImage,1));
    assert(dispatches==1 && binds==1 && pushes==1 && constants==1 && barriers==2 && resets==1);
    assert(gx==160 && gy==90 && gz==1);
    const float expected[16]={1.2f,1.7f,0.1f,-0.2f,1.002f,0.2f,-1,0,1280,720,1.0f/1280,1.0f/720,2,1,1,2};
    assert(!memcmp(pushed,expected,sizeof expected));
    record(0); assert(dispatches==1);
    vk_ao_image_gone(H(VkImage,1)); vk_ao_draw(&ex); record(0); assert(dispatches==1);
    vk_ao_frame(); vk_ao_draw(&ex); radius=101; quality=-1; strength=1; record(0);
    assert(dispatches==2 && pushed[13]==0.01f && pushed[14]==100 && pushed[15]==0);
    // The same camera address may render to two different full-size depth targets.
    setup();
    for (int i=0; i<8; i++) vk_ao_draw(&ex);
    image.image=ex.depth.image=H(VkImage, 9); vk_ao_draw(&ex);
    record(0); assert(!dispatches && !init_calls);
    record(H(VkImage, 1)); assert(dispatches==1 && pushes==1);
    setup(); vk_ao_draw(&ex);
    image.image=ex.depth.image=H(VkImage, 9); vk_ao_draw(&ex);
    vk_ao_image_gone(H(VkImage, 1)); vk_ao_image_gone(H(VkImage, 9));
    image.image=ex.depth.image=H(VkImage, 11); vk_ao_draw(&ex);
    expected_view=H(VkImageView, 12);
    record(0); assert(!dispatches && !init_calls);
    vk_ao_frame(); vk_ao_draw(&ex);
    record(0); assert(dispatches==1 && pushes==1);
    setup(); vk_ao_draw(&ex);
    image.image=ex.depth.image=H(VkImage, 9); vk_ao_draw(&ex);
    expected_view=H(VkImageView, 10);
    record(H(VkImage, 9)); assert(dispatches==1 && pushes==1);
    setup(); vk_ao_draw(&ex);
    image.image=ex.depth.image=H(VkImage, 9); vk_ao_draw(&ex);
    vk_ao_image_gone(H(VkImage, 9)); record(H(VkImage, 9)); assert(!dispatches);
    record(0); assert(!dispatches);
    record(H(VkImage, 1)); assert(dispatches==1 && pushes==1);
    puts("AO draw/record offline: selection, rejects, ABI, dispatch, clamps, retirement and frame reset passed");
    return 0;
}
