// Experimental stereo frame inputs, captured in replay order and copied into a present job.
#pragma once
#include "vk_exec.h"

typedef struct {
    VkImage depth;
    VkImageView view;
    uint32_t width, height;
    float projection[4]; // P22, P23, P32, P33
} StereoFrame;

// Draw and frame run on replay; image_gone runs under the texture lock.
int vk_stereo_enabled(void);
void vk_stereo_draw(VkExec *ex, const VkPipeInfo *info);
void vk_stereo_images(VkExec *ex, const VkPipeInfo *info); // graphics imageStore, including task/mesh draws
void vk_stereo_copy(VkImage src, VkImage dst);
void vk_stereo_temporal(VkImage src, VkImage depth, VkImage dst);
void vk_stereo_forget(VkImage image);
StereoFrame vk_stereo_frame(VkImage present);
void vk_stereo_image_gone(VkImage image);
