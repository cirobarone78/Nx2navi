"""Minimal Vulkan compute fixture for ambient_occlusion_gpu.py."""
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
        compiler = os.environ.get('GLSLC') or shutil.which('glslc')
        validator = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
        if not compiler or not validator:
            raise RuntimeError('Set GLSLC and SPIRV_VAL to the shader tool paths')
        # Keep compiled shaders in the ignored build directory, never in source.
        build = ROOT / 'build'
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build) as tmp:
            spv = pathlib.Path(tmp) / 'ao.spv'
            shader_source = pathlib.Path(os.environ.get('AO_SHADER', ROOT / 'runtime/shaders/ambient_occlusion.comp'))
            # Match tools/build_shaders.py, including optimisation and its Vulkan target.
            subprocess.run([compiler, '-O', '--target-env=vulkan1.1', '-fshader-stage=compute',
                            str(shader_source), '-o', str(spv)], check=True, timeout=30)
            subprocess.run([validator, '--target-env', 'vulkan1.1', str(spv)], check=True, timeout=30)
            code = spv.read_bytes()
        app = v.VkApplicationInfo(pApplicationName='AO synthetic test', apiVersion=v.VK_MAKE_VERSION(1, 1, 0))
        self.instance = v.vkCreateInstance(v.VkInstanceCreateInfo(pApplicationInfo=app), None)
        physicals = v.vkEnumeratePhysicalDevices(self.instance)
        self.physical = next(p for p in physicals if any(q.queueFlags & v.VK_QUEUE_COMPUTE_BIT for q in v.vkGetPhysicalDeviceQueueFamilyProperties(p)))
        self.family = next(i for i,q in enumerate(v.vkGetPhysicalDeviceQueueFamilyProperties(self.physical)) if q.queueFlags & v.VK_QUEUE_COMPUTE_BIT)
        print('Vulkan device:', v.vkGetPhysicalDeviceProperties(self.physical).deviceName)
        qi = v.VkDeviceQueueCreateInfo(queueFamilyIndex=self.family, queueCount=1, pQueuePriorities=[1.0])
        self.device = v.vkCreateDevice(self.physical, v.VkDeviceCreateInfo(queueCreateInfoCount=1, pQueueCreateInfos=[qi]), None)
        self.queue = v.vkGetDeviceQueue(self.device, self.family, 0)
        self.memory = v.vkGetPhysicalDeviceMemoryProperties(self.physical)
        self.pool = v.vkCreateCommandPool(self.device, v.VkCommandPoolCreateInfo(queueFamilyIndex=self.family), None)
        bindings = [v.VkDescriptorSetLayoutBinding(binding=0, descriptorType=v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptorCount=1, stageFlags=v.VK_SHADER_STAGE_COMPUTE_BIT),
                    v.VkDescriptorSetLayoutBinding(binding=1, descriptorType=v.VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, descriptorCount=1, stageFlags=v.VK_SHADER_STAGE_COMPUTE_BIT)]
        self.dsl = v.vkCreateDescriptorSetLayout(self.device, v.VkDescriptorSetLayoutCreateInfo(bindingCount=2, pBindings=bindings), None)
        push = v.VkPushConstantRange(stageFlags=v.VK_SHADER_STAGE_COMPUTE_BIT, offset=0, size=64)
        self.layout = v.vkCreatePipelineLayout(self.device, v.VkPipelineLayoutCreateInfo(setLayoutCount=1, pSetLayouts=[self.dsl], pushConstantRangeCount=1, pPushConstantRanges=[push]), None)
        shader = v.vkCreateShaderModule(self.device, v.VkShaderModuleCreateInfo(codeSize=len(code), pCode=code), None)
        stage = v.VkPipelineShaderStageCreateInfo(stage=v.VK_SHADER_STAGE_COMPUTE_BIT, module=shader, pName='main')
        self.pipeline = v.vkCreateComputePipelines(self.device, v.VK_NULL_HANDLE, 1, [v.VkComputePipelineCreateInfo(stage=stage, layout=self.layout)], None)[0]
        v.vkDestroyShaderModule(self.device, shader, None)
        self.sampler = v.vkCreateSampler(self.device, v.VkSamplerCreateInfo(magFilter=v.VK_FILTER_NEAREST, minFilter=v.VK_FILTER_NEAREST, mipmapMode=v.VK_SAMPLER_MIPMAP_MODE_NEAREST, addressModeU=v.VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, addressModeV=v.VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, addressModeW=v.VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, maxLod=0), None)

    def close(self):
        v.vkDestroySampler(self.device, self.sampler, None)
        v.vkDestroyPipeline(self.device, self.pipeline, None)
        v.vkDestroyPipelineLayout(self.device, self.layout, None)
        v.vkDestroyDescriptorSetLayout(self.device, self.dsl, None)
        v.vkDestroyCommandPool(self.device, self.pool, None)
        v.vkDestroyDevice(self.device, None)
        v.vkDestroyInstance(self.instance, None)

    def allocate(self, requirements, flags):
        index = next(i for i in range(self.memory.memoryTypeCount) if requirements.memoryTypeBits & (1 << i) and self.memory.memoryTypes[i].propertyFlags & flags == flags)
        return v.vkAllocateMemory(self.device, v.VkMemoryAllocateInfo(allocationSize=requirements.size, memoryTypeIndex=index), None)

    def buffer(self, size):
        b = v.vkCreateBuffer(self.device, v.VkBufferCreateInfo(size=size, usage=v.VK_BUFFER_USAGE_TRANSFER_SRC_BIT | v.VK_BUFFER_USAGE_TRANSFER_DST_BIT, sharingMode=v.VK_SHARING_MODE_EXCLUSIVE), None)
        mem = self.allocate(v.vkGetBufferMemoryRequirements(self.device, b), v.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | v.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        v.vkBindBufferMemory(self.device, b, mem, 0)
        return b, mem

    def image(self, w, h, fmt, usage):
        image = v.vkCreateImage(self.device, v.VkImageCreateInfo(imageType=v.VK_IMAGE_TYPE_2D, format=fmt, extent=v.VkExtent3D(width=w, height=h, depth=1), mipLevels=1, arrayLayers=1, samples=v.VK_SAMPLE_COUNT_1_BIT, tiling=v.VK_IMAGE_TILING_OPTIMAL, usage=usage, sharingMode=v.VK_SHARING_MODE_EXCLUSIVE, initialLayout=v.VK_IMAGE_LAYOUT_UNDEFINED), None)
        mem = self.allocate(v.vkGetImageMemoryRequirements(self.device, image), v.VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        v.vkBindImageMemory(self.device, image, mem, 0)
        aspect = v.VK_IMAGE_ASPECT_DEPTH_BIT if fmt == v.VK_FORMAT_D32_SFLOAT else v.VK_IMAGE_ASPECT_COLOR_BIT
        sub = v.VkImageSubresourceRange(aspectMask=aspect, levelCount=1, layerCount=1)
        view = v.vkCreateImageView(self.device, v.VkImageViewCreateInfo(image=image, viewType=v.VK_IMAGE_VIEW_TYPE_2D, format=fmt, subresourceRange=sub), None)
        return image, mem, view

    def render(self, values, w, h, dw, dh, mode, strength, radius, quality=2, offset_x=0.0):
        d = self.device
        depth_bytes = struct.pack('<%df' % len(values), *values)
        colour_bytes = struct.pack('<4e', 1, 1, 1, 1) * (w*h)
        db, dm = self.buffer(len(depth_bytes))
        cb, cm = self.buffer(len(colour_bytes))
        for mem, data in ((dm, depth_bytes), (cm, colour_bytes)):
            ptr = v.vkMapMemory(d, mem, 0, len(data), 0)
            ptr[:] = data
            v.vkUnmapMemory(d, mem)
        di, dim, div = self.image(dw, dh, v.VK_FORMAT_D32_SFLOAT, v.VK_IMAGE_USAGE_TRANSFER_DST_BIT | v.VK_IMAGE_USAGE_SAMPLED_BIT)
        ci, cim, civ = self.image(w, h, v.VK_FORMAT_R16G16B16A16_SFLOAT, v.VK_IMAGE_USAGE_TRANSFER_DST_BIT | v.VK_IMAGE_USAGE_TRANSFER_SRC_BIT | v.VK_IMAGE_USAGE_STORAGE_BIT)
        sizes = [v.VkDescriptorPoolSize(type=v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptorCount=1), v.VkDescriptorPoolSize(type=v.VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, descriptorCount=1)]
        pool = v.vkCreateDescriptorPool(d, v.VkDescriptorPoolCreateInfo(maxSets=1, poolSizeCount=2, pPoolSizes=sizes), None)
        ds = v.vkAllocateDescriptorSets(d, v.VkDescriptorSetAllocateInfo(descriptorPool=pool, descriptorSetCount=1, pSetLayouts=[self.dsl]))[0]
        infos = [v.VkDescriptorImageInfo(sampler=self.sampler, imageView=div, imageLayout=v.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), v.VkDescriptorImageInfo(imageView=civ, imageLayout=v.VK_IMAGE_LAYOUT_GENERAL)]
        writes = [v.VkWriteDescriptorSet(dstSet=ds, dstBinding=i, descriptorCount=1, descriptorType=t, pImageInfo=[infos[i]]) for i,t in enumerate((v.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, v.VK_DESCRIPTOR_TYPE_STORAGE_IMAGE))]
        v.vkUpdateDescriptorSets(d, len(writes), writes, 0, None)
        cmd = v.vkAllocateCommandBuffers(d, v.VkCommandBufferAllocateInfo(commandPool=self.pool, level=v.VK_COMMAND_BUFFER_LEVEL_PRIMARY, commandBufferCount=1))[0]
        v.vkBeginCommandBuffer(cmd, v.VkCommandBufferBeginInfo(flags=v.VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))
        def barrier(image, old, new, src, dst, srcstage, dststage):
            aspect = v.VK_IMAGE_ASPECT_DEPTH_BIT if image == di else v.VK_IMAGE_ASPECT_COLOR_BIT
            sub = v.VkImageSubresourceRange(aspectMask=aspect, levelCount=1, layerCount=1)
            b = v.VkImageMemoryBarrier(srcAccessMask=src, dstAccessMask=dst, oldLayout=old, newLayout=new, srcQueueFamilyIndex=v.VK_QUEUE_FAMILY_IGNORED, dstQueueFamilyIndex=v.VK_QUEUE_FAMILY_IGNORED, image=image, subresourceRange=sub)
            v.vkCmdPipelineBarrier(cmd, srcstage, dststage, 0, 0, None, 0, None, 1, [b])
        def region(width, height, depth=False):
            aspect = v.VK_IMAGE_ASPECT_DEPTH_BIT if depth else v.VK_IMAGE_ASPECT_COLOR_BIT
            return v.VkBufferImageCopy(imageSubresource=v.VkImageSubresourceLayers(aspectMask=aspect, layerCount=1), imageExtent=v.VkExtent3D(width=width, height=height, depth=1))
        for image in (di, ci):
            barrier(image, v.VK_IMAGE_LAYOUT_UNDEFINED, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, v.VK_ACCESS_TRANSFER_WRITE_BIT, v.VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT)
        v.vkCmdCopyBufferToImage(cmd, db, di, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, [region(dw, dh, True)])
        v.vkCmdCopyBufferToImage(cmd, cb, ci, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, [region(w, h)])
        barrier(di, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, v.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, v.VK_ACCESS_TRANSFER_WRITE_BIT, v.VK_ACCESS_SHADER_READ_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT, v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
        barrier(ci, v.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, v.VK_IMAGE_LAYOUT_GENERAL, v.VK_ACCESS_TRANSFER_WRITE_BIT, v.VK_ACCESS_SHADER_READ_BIT | v.VK_ACCESS_SHADER_WRITE_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT, v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
        v.vkCmdBindPipeline(cmd, v.VK_PIPELINE_BIND_POINT_COMPUTE, self.pipeline)
        v.vkCmdBindDescriptorSets(cmd, v.VK_PIPELINE_BIND_POINT_COMPUTE, self.layout, 0, 1, [ds], 0, None)
        push = struct.pack('<16f', 1,1,offset_x,0, 1.002002002,2.002002002,-1,0, w,h,1/w,1/h, mode,strength,radius,quality)
        v.vkCmdPushConstants(cmd, self.layout, v.VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, v.ffi.from_buffer(push))
        v.vkCmdDispatch(cmd, (w+7)//8, (h+7)//8, 1)
        barrier(ci, v.VK_IMAGE_LAYOUT_GENERAL, v.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v.VK_ACCESS_SHADER_WRITE_BIT, v.VK_ACCESS_TRANSFER_READ_BIT, v.VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, v.VK_PIPELINE_STAGE_TRANSFER_BIT)
        v.vkCmdCopyImageToBuffer(cmd, ci, v.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cb, 1, [region(w, h)])
        v.vkEndCommandBuffer(cmd)
        v.vkQueueSubmit(self.queue, 1, [v.VkSubmitInfo(commandBufferCount=1, pCommandBuffers=[cmd])], v.VK_NULL_HANDLE)
        v.vkQueueWaitIdle(self.queue)
        ptr = v.vkMapMemory(d, cm, 0, len(colour_bytes), 0)
        output = bytes(ptr)
        v.vkUnmapMemory(d, cm)
        v.vkFreeCommandBuffers(d, self.pool, 1, [cmd])
        v.vkDestroyDescriptorPool(d, pool, None)
        for image, memory, view in ((di, dim, div), (ci, cim, civ)):
            v.vkDestroyImageView(d, view, None)
            v.vkDestroyImage(d, image, None)
            v.vkFreeMemory(d, memory, None)
        for buffer, memory in ((db, dm), (cb, cm)):
            v.vkDestroyBuffer(d, buffer, None)
            v.vkFreeMemory(d, memory, None)
        pixels = list(struct.iter_unpack('<4e', output))
        assert all(p[0] == p[1] == p[2] and p[3] == 1.0 for p in pixels), 'RGB/alpha corruption'
        return [p[0] for p in pixels]
