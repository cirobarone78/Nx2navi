// Optional screen-space ambient occlusion on Ocarina's decoded scene, before the HUD.
#pragma once
#include "vk.h"

typedef struct VkExec VkExec;
void vk_ao_draw(VkExec *ex);   // capture camera/depth from a scene draw, independently of temporal upscaling
void vk_ao_frame(void);       // replay-order present boundary, never reuse a previous frame's camera
void vk_ao_image_gone(VkImage image);   // texture lock held: forget a destroyed/reused depth image
// scene: RGBA16F storage view in GENERAL. expected_depth is the temporal upscaler's depth, or null for the spatial path.
void vk_ao_record(VkCommandBuffer cb, VkImageView scene, uint32_t width, uint32_t height, VkImage expected_depth);
void vk_ao_release(void);     // device idle, before its destruction; also handles partial initialization
