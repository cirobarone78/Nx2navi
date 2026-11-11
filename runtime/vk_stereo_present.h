// Presentation-thread stereo composition. Frame inputs are copied from one immutable PresentJob.
#pragma once
#include "vk_stereo.h"

void vk_stereo_present_frame(StereoFrame frame);
// SDR GENERAL sampled source, valid crop, presentation slot whose fence has completed.
// Returns an output-sized RGBA16F GENERAL image, or null to preserve the normal monoscopic path.
VkImage vk_stereo_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt,
                        int width, int height, const int crop[4], int output_width, int output_height);
void vk_stereo_release(void); // device idle, before destruction
