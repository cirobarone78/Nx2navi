"""Small standalone headless Vulkan compute fixture for stereo_gpu.py."""
import contextlib
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile

import vulkan as v

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Runner:
    def __init__(self):
        self.resources = contextlib.ExitStack()
        try:
            compiler = os.environ.get('GLSLC') or shutil.which('glslc')
            validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
            if not compiler or not validator:
                raise RuntimeError('Set GLSLC and SPIRV_VAL to the shader tools')
            source = pathlib.Path(os.environ.get('STEREO_SHADER', ROOT / 'runtime/shaders/stereo.comp'))
            if not source.is_file():
                raise FileNotFoundError(f'Missing stereo shader: {source}')
            build = ROOT / 'build'
            build.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(dir=build) as tmp:
                spv = pathlib.Path(tmp) / 'stereo.spv'
                subprocess.run([compiler, '-O', '--target-env=vulkan1.1', '-fshader-stage=compute', str(source), '-o', str(spv)], check=True, timeout=30)
                subprocess.run([validator, '--target-env', 'vulkan1.1', str(spv)], check=True, timeout=30)
                code = spv.read_bytes()
            app = v.VkApplicationInfo(pApplicationName='Stereo synthetic test', apiVersion=v.VK_MAKE_VERSION(1, 1, 0))
            self.instance = v.vkCreateInstance(v.VkInstanceCreateInfo(pApplicationInfo=app), None)
            self.resources.callback(v.vkDestroyInstance, self.instance, None)
            self.physical = next(p for p in v.vkEnumeratePhysicalDevices(self.instance) if any(q.queueFlags & v.VK_QUEUE_COMPUTE_BIT for q in v.vkGetPhysicalDeviceQueueFamilyProperties(p)))
            self.family = next(i for i, q in enumerate(v.vkGetPhysicalDeviceQueueFamilyProperties(self.physical)) if q.queueFlags & v.VK_QUEUE_COMPUTE_BIT)
            print('Vulkan device:', v.vkGetPhysicalDeviceProperties(self.physical).deviceName)
            qi = v.VkDeviceQueueCreateInfo(queueFamilyIndex=self.family, queueCount=1, pQueuePriorities=[1.0])
            self.device = v.vkCreateDevice(self.physical, v.VkDeviceCreateInfo(queueCreateInfoCount=1, pQueueCreateInfos=[qi]), None)
            d = self.device
            self.resources.callback(v.vkDestroyDevice, d, None)
            self.queue = v.vkGetDeviceQueue(d, self.family, 0)
            self.memory = v.vkGetPhysicalDeviceMemoryProperties(self.physical)
            self.pool = v.vkCreateCommandPool(d, v.VkCommandPoolCreateInfo(queueFamilyIndex=self.family), None)
            self.resources.callback(v.vkDestroyCommandPool, d, self.pool, None)
            types = [v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, v.VK_DESCRIPTOR_TYPE_STORAGE_IMAGE]
            bindings = [v.VkDescriptorSetLayoutBinding(binding=i, descriptorType=t, descriptorCount=1, stageFlags=v.VK_SHADER_STAGE_COMPUTE_BIT) for i, t in enumerate(types)]
            self.dsl = v.vkCreateDescriptorSetLayout(d, v.VkDescriptorSetLayoutCreateInfo(bindingCount=3, pBindings=bindings), None)
            self.resources.callback(v.vkDestroyDescriptorSetLayout, d, self.dsl, None)
            push = v.VkPushConstantRange(stageFlags=v.VK_SHADER_STAGE_COMPUTE_BIT, offset=0, size=64)
            self.layout = v.vkCreatePipelineLayout(d, v.VkPipelineLayoutCreateInfo(setLayoutCount=1, pSetLayouts=[self.dsl], pushConstantRangeCount=1, pPushConstantRanges=[push]), None)
            self.resources.callback(v.vkDestroyPipelineLayout, d, self.layout, None)
            shader = v.vkCreateShaderModule(d, v.VkShaderModuleCreateInfo(codeSize=len(code), pCode=code), None)
            try:
                stage = v.VkPipelineShaderStageCreateInfo(stage=v.VK_SHADER_STAGE_COMPUTE_BIT, module=shader, pName='main')
                self.pipeline = v.vkCreateComputePipelines(d, v.VK_NULL_HANDLE, 1, [v.VkComputePipelineCreateInfo(stage=stage, layout=self.layout)], None)[0]
            finally:
                v.vkDestroyShaderModule(d, shader, None)
            self.resources.callback(v.vkDestroyPipeline, d, self.pipeline, None)
            self.sampler = v.vkCreateSampler(d, v.VkSamplerCreateInfo(magFilter=v.VK_FILTER_NEAREST, minFilter=v.VK_FILTER_NEAREST, mipmapMode=v.VK_SAMPLER_MIPMAP_MODE_NEAREST, addressModeU=v.VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, addressModeV=v.VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, addressModeW=v.VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, maxLod=0), None)
            self.resources.callback(v.vkDestroySampler, d, self.sampler, None)
        except BaseException:
            self.close()
            raise

    def close(self):
        self.resources.close()

    def allocate(self, requirements, flags):
        index = next(i for i in range(self.memory.memoryTypeCount) if requirements.memoryTypeBits & (1 << i) and self.memory.memoryTypes[i].propertyFlags & flags == flags)
        return v.vkAllocateMemory(self.device, v.VkMemoryAllocateInfo(allocationSize=requirements.size, memoryTypeIndex=index), None)

    def buffer(self, stack, size):
        d = self.device
        b = v.vkCreateBuffer(d, v.VkBufferCreateInfo(size=size, usage=v.VK_BUFFER_USAGE_TRANSFER_SRC_BIT | v.VK_BUFFER_USAGE_TRANSFER_DST_BIT, sharingMode=v.VK_SHARING_MODE_EXCLUSIVE), None)
        # Destroy the object before freeing bound memory, including on setup failure.
        mem = None
        def cleanup():
            v.vkDestroyBuffer(d, b, None)
            if mem is not None:
                v.vkFreeMemory(d, mem, None)
        stack.callback(cleanup)
        mem = self.allocate(v.vkGetBufferMemoryRequirements(d, b), v.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | v.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        v.vkBindBufferMemory(d, b, mem, 0)
        return b, mem

    def image(self, stack, w, h, fmt, usage):
        d = self.device
        image = v.vkCreateImage(d, v.VkImageCreateInfo(imageType=v.VK_IMAGE_TYPE_2D, format=fmt, extent=v.VkExtent3D(width=w, height=h, depth=1), mipLevels=1, arrayLayers=1, samples=v.VK_SAMPLE_COUNT_1_BIT, tiling=v.VK_IMAGE_TILING_OPTIMAL, usage=usage, sharingMode=v.VK_SHARING_MODE_EXCLUSIVE, initialLayout=v.VK_IMAGE_LAYOUT_UNDEFINED), None)
        mem = view = None
        def cleanup():
            if view is not None:
                v.vkDestroyImageView(d, view, None)
            v.vkDestroyImage(d, image, None)
            if mem is not None:
                v.vkFreeMemory(d, mem, None)
        stack.callback(cleanup)
        mem = self.allocate(v.vkGetImageMemoryRequirements(d, image), v.VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        v.vkBindImageMemory(d, image, mem, 0)
        aspect = v.VK_IMAGE_ASPECT_DEPTH_BIT if fmt == v.VK_FORMAT_D32_SFLOAT else v.VK_IMAGE_ASPECT_COLOR_BIT
        sub = v.VkImageSubresourceRange(aspectMask=aspect, levelCount=1, layerCount=1)
        view = v.vkCreateImageView(d, v.VkImageViewCreateInfo(image=image, viewType=v.VK_IMAGE_VIEW_TYPE_2D, format=fmt, subresourceRange=sub), None)
        return image, view

    def render(self, colors, depths, width, height, crop, output, projection, params, source_srgb=False, depth_shape=None, native_blit=False):
        ow, oh, mode, swap = output
        dw, dh = depth_shape or (width, height)
        if not (1 <= width <= 96 and 1 <= height <= 96 and 1 <= ow <= 96 and 1 <= oh <= 96):
            raise ValueError('Fixture dimensions must be between 1 and 96')
        if not (1 <= dw <= 96 and 1 <= dh <= 96):
            raise ValueError('Depth dimensions must be between 1 and 96')
        if len(colors) != width * height or len(depths) != dw * dh:
            raise ValueError('Incorrect input dimensions')
        d = self.device
        with contextlib.ExitStack() as stack:
            color_data = (bytes(round(max(0, min(1, c)) * 255) for pixel in colors for c in pixel)
                          if source_srgb else b''.join(struct.pack('<4e', *pixel) for pixel in colors))
            depth_data = struct.pack('<%df' % len(depths), *depths)
            out_size = ow * oh * 8
            cb, cm = self.buffer(stack, len(color_data))
            db, dm = self.buffer(stack, len(depth_data))
            ob, om = self.buffer(stack, out_size)
            for mem, data in ((cm, color_data), (dm, depth_data)):
                ptr = v.vkMapMemory(d, mem, 0, len(data), 0)
                try:
                    ptr[:] = data
                finally:
                    v.vkUnmapMemory(d, mem)
            ci, cv = self.image(stack, width, height,
                                v.VK_FORMAT_R8G8B8A8_SRGB if source_srgb else v.VK_FORMAT_R16G16B16A16_SFLOAT,
                                v.VK_IMAGE_USAGE_TRANSFER_DST_BIT | v.VK_IMAGE_USAGE_TRANSFER_SRC_BIT | v.VK_IMAGE_USAGE_SAMPLED_BIT)
            di, dv = self.image(stack, dw, dh, v.VK_FORMAT_D32_SFLOAT, v.VK_IMAGE_USAGE_TRANSFER_DST_BIT | v.VK_IMAGE_USAGE_SAMPLED_BIT)
            oi, ov = self.image(stack, ow, oh, v.VK_FORMAT_R16G16B16A16_SFLOAT,
                                v.VK_IMAGE_USAGE_STORAGE_BIT | v.VK_IMAGE_USAGE_TRANSFER_SRC_BIT | v.VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            sizes = [v.VkDescriptorPoolSize(type=v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptorCount=2), v.VkDescriptorPoolSize(type=v.VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, descriptorCount=1)]
            pool = v.vkCreateDescriptorPool(d, v.VkDescriptorPoolCreateInfo(maxSets=1, poolSizeCount=2, pPoolSizes=sizes), None)
            stack.callback(v.vkDestroyDescriptorPool, d, pool, None)
            ds = v.vkAllocateDescriptorSets(d, v.VkDescriptorSetAllocateInfo(descriptorPool=pool, descriptorSetCount=1, pSetLayouts=[self.dsl]))[0]
            infos = [v.VkDescriptorImageInfo(sampler=self.sampler, imageView=cv, imageLayout=v.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), v.VkDescriptorImageInfo(sampler=self.sampler, imageView=dv, imageLayout=v.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), v.VkDescriptorImageInfo(imageView=ov, imageLayout=v.VK_IMAGE_LAYOUT_GENERAL)]
            types = [v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, v.VK_DESCRIPTOR_TYPE_STORAGE_IMAGE]
            writes = [v.VkWriteDescriptorSet(dstSet=ds, dstBinding=i, descriptorCount=1, descriptorType=t, pImageInfo=[infos[i]]) for i, t in enumerate(types)]
            v.vkUpdateDescriptorSets(d, len(writes), writes, 0, None)
            cmd = v.vkAllocateCommandBuffers(d, v.VkCommandBufferAllocateInfo(commandPool=self.pool, level=v.VK_COMMAND_BUFFER_LEVEL_PRIMARY, commandBufferCount=1))[0]
            stack.callback(v.vkFreeCommandBuffers, d, self.pool, 1, [cmd])
            v.vkBeginCommandBuffer(cmd, v.VkCommandBufferBeginInfo(flags=v.VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))
            def barrier(image, aspect, old, new, src, dst, srcstage, dststage):
                sub = v.VkImageSubresourceRange(aspectMask=aspect, levelCount=1, layerCount=1)
                b = v.VkImageMemoryBarrier(srcAccessMask=src, dstAccessMask=dst, oldLayout=old, newLayout=new, srcQueueFamilyIndex=v.VK_QUEUE_FAMILY_IGNORED, dstQueueFamilyIndex=v.VK_QUEUE_FAMILY_IGNORED, image=image, subresourceRange=sub)
                v.vkCmdPipelineBarrier(cmd, srcstage, dststage, 0, 0, None, 0, None, 1, [b])
            def region(w, h, aspect):
                return v.VkBufferImageCopy(imageSubresource=v.VkImageSubresourceLayers(aspectMask=aspect, layerCount=1), imageExtent=v.VkExtent3D(width=w, height=h, depth=1))
            for image, aspect, buffer, iw, ih in ((ci, v.VK_IMAGE_ASPECT_COLOR_BIT, cb, width, height),
                                                 (di, v.VK_IMAGE_ASPECT_DEPTH_BIT, db, dw, dh)):
                barrier(image, aspect, v.VK_IMAGE_LAYOUT_UNDEFINED, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, v.VK_ACCESS_TRANSFER_WRITE_BIT, v.VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT)
                v.vkCmdCopyBufferToImage(cmd, buffer, image, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, [region(iw, ih, aspect)])
                barrier(image, aspect, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, v.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, v.VK_ACCESS_TRANSFER_WRITE_BIT, v.VK_ACCESS_SHADER_READ_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT, v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
            barrier(oi, v.VK_IMAGE_ASPECT_COLOR_BIT, v.VK_IMAGE_LAYOUT_UNDEFINED, v.VK_IMAGE_LAYOUT_GENERAL, 0, v.VK_ACCESS_SHADER_WRITE_BIT, v.VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
            if native_blit:
                barrier(ci, v.VK_IMAGE_ASPECT_COLOR_BIT, v.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        v.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v.VK_ACCESS_TRANSFER_WRITE_BIT,
                        v.VK_ACCESS_TRANSFER_READ_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT)
                barrier(oi, v.VK_IMAGE_ASPECT_COLOR_BIT, v.VK_IMAGE_LAYOUT_GENERAL,
                        v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, v.VK_ACCESS_TRANSFER_WRITE_BIT,
                        v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT)
                region_blit = v.VkImageBlit(
                    srcSubresource=v.VkImageSubresourceLayers(aspectMask=v.VK_IMAGE_ASPECT_COLOR_BIT, layerCount=1),
                    srcOffsets=[v.VkOffset3D(x=crop[0], y=crop[1], z=0), v.VkOffset3D(x=crop[0]+crop[2], y=crop[1]+crop[3], z=1)],
                    dstSubresource=v.VkImageSubresourceLayers(aspectMask=v.VK_IMAGE_ASPECT_COLOR_BIT, layerCount=1),
                    dstOffsets=[v.VkOffset3D(x=0, y=0, z=0), v.VkOffset3D(x=ow, y=oh, z=1)])
                v.vkCmdBlitImage(cmd, ci, v.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, oi,
                                 v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, [region_blit], v.VK_FILTER_NEAREST)
                barrier(oi, v.VK_IMAGE_ASPECT_COLOR_BIT, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        v.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v.VK_ACCESS_TRANSFER_WRITE_BIT,
                        v.VK_ACCESS_TRANSFER_READ_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT)
            else:
                v.vkCmdBindPipeline(cmd, v.VK_PIPELINE_BIND_POINT_COMPUTE, self.pipeline)
                v.vkCmdBindDescriptorSets(cmd, v.VK_PIPELINE_BIND_POINT_COMPUTE, self.layout, 0, 1, [ds], 0, None)
                push = struct.pack('<8i8f', *crop, *output, *projection, *params)
                v.vkCmdPushConstants(cmd, self.layout, v.VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, v.ffi.from_buffer(push))
                v.vkCmdDispatch(cmd, (ow + 7) // 8, (oh + 7) // 8, 1)
                barrier(oi, v.VK_IMAGE_ASPECT_COLOR_BIT, v.VK_IMAGE_LAYOUT_GENERAL, v.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v.VK_ACCESS_SHADER_WRITE_BIT, v.VK_ACCESS_TRANSFER_READ_BIT, v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT)
            v.vkCmdCopyImageToBuffer(cmd, oi, v.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob, 1, [region(ow, oh, v.VK_IMAGE_ASPECT_COLOR_BIT)])
            v.vkEndCommandBuffer(cmd)
            v.vkQueueSubmit(self.queue, 1, [v.VkSubmitInfo(commandBufferCount=1, pCommandBuffers=[cmd])], v.VK_NULL_HANDLE)
            v.vkQueueWaitIdle(self.queue)
            ptr = v.vkMapMemory(d, om, 0, out_size, 0)
            try:
                data = bytes(ptr)
            finally:
                v.vkUnmapMemory(d, om)
            return list(struct.iter_unpack('<4e', data))
