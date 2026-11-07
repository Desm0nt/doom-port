// [nonadreno] ASTC 4x4 HDR GPU self-test, the Vulkan side (docs/BUGS_AND_FIXES.md 2.37). Included once, by android/vulkan_present.cpp,
// which runs it right after vkCreateDevice and before the overlay variant is mounted (the RomFS is mounted later, by dm_main).
// Plan and verdict: xbg_astc_hdr.h. It creates its own objects (an image of the engine's format and shape class: 2D,
// VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK, CUBE_COMPATIBLE, a cube array view and a 2D array view; NEAREST and LINEAR samplers), uploads
// the test blocks with vkCmdCopyBufferToImage as the engine uploads overlay blocks, runs xbg_astc_hdr_selftest.comp, reads the
// results back and destroys everything. One submission, waited with a fence (2 s timeout).
#pragma once
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>
#include "xbg_astc_hdr.h"
#include "xbg_astc_hdr_selftest_spv.h"

struct XbgAhsRun {
    int ran;                 // 1 = the GPU test ran (verdict valid); 0 = not run (why)
    char why[160];
    XbgAhsVerdict verdict;
    unsigned queries, blocks;
    double ms;
};

static inline XbgAhsRun xbg_astc_hdr_selftest_run(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t family, bool cube_array_feature) {
    XbgAhsRun out; memset(&out, 0, sizeof out);
    timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    const VkFormat fmt = VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK;
    {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, fmt, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp.optimalTilingFeatures & need) != need) { snprintf(out.why, sizeof out.why, "ASTC_4x4_SFLOAT optimal features %#x lack sampled / transfer-dst", unsigned(fp.optimalTilingFeatures)); return out; }
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) { snprintf(out.why, sizeof out.why, "ASTC_4x4_SFLOAT is not linearly filterable (features %#x)", unsigned(fp.optimalTilingFeatures)); return out; }
    }
    // the shader samples a samplerCubeArray (the engine's probe array view): without the feature there is nothing comparable to test
    if (!cube_array_feature) { snprintf(out.why, sizeof out.why, "no imageCubeArray feature"); return out; }
    XbgAhsPlan plan;
    xbg_astc_hdr_plan(plan, xbg_astc_hdr_tests, XBG_ASTC_HDR_TEST_BLOCKS, true);
    out.queries = unsigned(plan.queries.size()); out.blocks = unsigned(plan.blocks.size() / 16);

    VkImage image = VK_NULL_HANDLE; VkDeviceMemory image_mem = VK_NULL_HANDLE, host_mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE; VkImageView view_cube = VK_NULL_HANDLE, view_flat = VK_NULL_HANDLE;
    VkSampler s_near = VK_NULL_HANDLE, s_lin = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
    VkShaderModule sm = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; VkDescriptorPool dp = VK_NULL_HANDLE; VkCommandPool cp = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    auto cleanup = [&] {
        if (fence) vkDestroyFence(dev, fence, nullptr);
        if (cp) vkDestroyCommandPool(dev, cp, nullptr);
        if (dp) vkDestroyDescriptorPool(dev, dp, nullptr);
        if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
        if (sm) vkDestroyShaderModule(dev, sm, nullptr);
        if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
        if (s_lin) vkDestroySampler(dev, s_lin, nullptr);
        if (s_near) vkDestroySampler(dev, s_near, nullptr);
        if (view_flat) vkDestroyImageView(dev, view_flat, nullptr);
        if (view_cube) vkDestroyImageView(dev, view_cube, nullptr);
        if (buf) vkDestroyBuffer(dev, buf, nullptr);
        if (host_mem) vkFreeMemory(dev, host_mem, nullptr);
        if (image) vkDestroyImage(dev, image, nullptr);
        if (image_mem) vkFreeMemory(dev, image_mem, nullptr);
    };
    auto fail = [&](const char *what, VkResult r) { snprintf(out.why, sizeof out.why, "%s failed: VkResult %d", what, int(r)); cleanup(); return out; };
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    auto mem_type = [&](uint32_t bits, VkMemoryPropertyFlags want) -> int {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1; };
    VkResult r;
    // the image: same create flags as the engine's probe array (2D, cube compatible, sampled + transfer dst, optimal tiling)
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {XBG_AHS_SIZE, XBG_AHS_SIZE, 1};
    ci.mipLevels = XBG_AHS_LEVELS; ci.arrayLayers = XBG_AHS_LAYERS; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((r = vkCreateImage(dev, &ci, nullptr, &image))) return fail("vkCreateImage", r);
    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, image, &req);
    int mt = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (mt < 0) mt = mem_type(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = req.size; ma.memoryTypeIndex = uint32_t(mt);
    if ((r = vkAllocateMemory(dev, &ma, nullptr, &image_mem))) return fail("image memory", r);
    if ((r = vkBindImageMemory(dev, image, image_mem, 0))) return fail("vkBindImageMemory", r);
    // one host-visible buffer: blocks | queries | results
    const size_t off_q = (plan.blocks.size() + 255) & ~size_t(255), off_r = (off_q + plan.queries.size() * 16 + 255) & ~size_t(255);
    const size_t total = off_r + plan.queries.size() * 16;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = total; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if ((r = vkCreateBuffer(dev, &bi, nullptr, &buf))) return fail("vkCreateBuffer", r);
    vkGetBufferMemoryRequirements(dev, buf, &req);
    mt = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { snprintf(out.why, sizeof out.why, "no host-visible coherent memory for the test buffer"); cleanup(); return out; }
    ma.allocationSize = req.size; ma.memoryTypeIndex = uint32_t(mt);
    if ((r = vkAllocateMemory(dev, &ma, nullptr, &host_mem))) return fail("buffer memory", r);
    if ((r = vkBindBufferMemory(dev, buf, host_mem, 0))) return fail("vkBindBufferMemory", r);
    uint8_t *map = nullptr;
    if ((r = vkMapMemory(dev, host_mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(&map)))) return fail("vkMapMemory", r);
    memset(map, 0, total);
    memcpy(map, plan.blocks.data(), plan.blocks.size());
    memcpy(map + off_q, plan.queries.data(), plan.queries.size() * 16);
    for (size_t i = 0; i < plan.queries.size() * 4; i++) { const float unwritten = -12345.0f; memcpy(map + off_r + i * 4, &unwritten, 4); }   // a lost write fails
    // views and samplers
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, XBG_AHS_LEVELS, 0, XBG_AHS_LAYERS};
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if ((r = vkCreateImageView(dev, &vi, nullptr, &view_flat))) return fail("2D array view", r);
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    if ((r = vkCreateImageView(dev, &vi, nullptr, &view_cube))) return fail("cube array view", r);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = float(XBG_AHS_LEVELS);
    if ((r = vkCreateSampler(dev, &si, nullptr, &s_near))) return fail("sampler", r);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if ((r = vkCreateSampler(dev, &si, nullptr, &s_lin))) return fail("sampler", r);
    // pipeline
    VkDescriptorSetLayoutBinding b[5] = {};
    for (uint32_t i = 0; i < 5; i++) { b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 5; dl.pBindings = b;
    if ((r = vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl))) return fail("descriptor set layout", r);
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if ((r = vkCreatePipelineLayout(dev, &pli, nullptr, &pl))) return fail("pipeline layout", r);
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = sizeof xbg_astc_hdr_selftest_cs; smi.pCode = xbg_astc_hdr_selftest_cs;
    if ((r = vkCreateShaderModule(dev, &smi, nullptr, &sm))) return fail("shader module", r);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
    if ((r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe))) return fail("compute pipeline", r);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if ((r = vkCreateDescriptorPool(dev, &dpi, nullptr, &dp))) return fail("descriptor pool", r);
    VkDescriptorSet set; VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
    if ((r = vkAllocateDescriptorSets(dev, &dai, &set))) return fail("descriptor set", r);
    VkDescriptorImageInfo ii[3] = {{s_near, view_cube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {s_near, view_flat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s_lin, view_flat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkDescriptorBufferInfo bq{buf, off_q, plan.queries.size() * 16}, br{buf, off_r, plan.queries.size() * 16};
    VkWriteDescriptorSet w[5] = {};
    for (uint32_t i = 0; i < 5; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = b[i].descriptorType;
        if (i < 3) w[i].pImageInfo = &ii[i]; else w[i].pBufferInfo = i == 3 ? &bq : &br; }
    vkUpdateDescriptorSets(dev, 5, w, 0, nullptr);
    // record: upload (as the engine: buffer-to-image copies of whole blocks per level / layer), barrier, dispatch, barrier to host
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = family;
    if ((r = vkCreateCommandPool(dev, &cpci, nullptr, &cp))) return fail("command pool", r);
    VkCommandBuffer cmd; VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = cp; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(dev, &cai, &cmd))) return fail("command buffer", r);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbi);
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; ib.image = image; ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.subresourceRange = vi.subresourceRange; ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    std::vector<VkBufferImageCopy> copies;
    for (const XbgAhsRegion &g : plan.regions) { VkBufferImageCopy c{}; c.bufferOffset = g.offset; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, g.level, g.layer, 1}; c.imageExtent = {g.width, g.height, 1}; copies.push_back(c); }
    vkCmdCopyBufferToImage(cmd, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
    const uint32_t push[2] = {uint32_t(plan.queries.size()), XBG_AHS_SIZE};
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, push);
    vkCmdDispatch(cmd, (push[0] + 63) / 64, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    if ((r = vkEndCommandBuffer(cmd))) return fail("vkEndCommandBuffer", r);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((r = vkCreateFence(dev, &fci, nullptr, &fence))) return fail("fence", r);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.commandBufferCount = 1; sub.pCommandBuffers = &cmd;
    if ((r = vkQueueSubmit(queue, 1, &sub, fence))) return fail("vkQueueSubmit", r);
    if ((r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull))) {
        if (r == VK_TIMEOUT) vkQueueWaitIdle(queue);   // never leave the submission running past the destroy below
        return fail("vkWaitForFences", r);
    }
    std::vector<float> got(plan.queries.size() * 4);
    memcpy(got.data(), map + off_r, got.size() * 4);
    out.verdict = xbg_astc_hdr_judge(plan, got.data());
    out.ran = 1;
    cleanup();
    timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    out.ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    return out;
}
