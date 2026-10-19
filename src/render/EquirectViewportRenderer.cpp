// Before any Qt header (EquirectViewportRenderer.h has the reason).
#include <vulkan/vulkan.h>

#include "EquirectViewportRenderer.h"

#include "../core/Log.h"

#include <QMutexLocker>
#include <QVersionNumber>

#include <unistd.h> // dup / close: imported memory FDs

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// The renderer's pipelines and the images and buffers they draw from: made
// in initResources, remade when a frame's size changes, freed in
// releaseResources.

namespace {

constexpr const char *kExternalMemoryFdExt = "VK_KHR_external_memory_fd";

} // namespace

VkCommandBuffer EquirectViewportRenderer::beginOneTimeCommands() const
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_window->graphicsCommandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_window->device(), &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

void EquirectViewportRenderer::endOneTimeCommands(VkCommandBuffer cmd) const
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkQueue queue = m_window->graphicsQueue();
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    // A one-off upload (new frame arriving), not per-render-frame -- a full
    // wait-idle is simple and correct; revisit if/when uploads need to
    // happen every rendered frame for real playback.
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(m_window->device(), m_window->graphicsCommandPool(), 1, &cmd);
}

VkShaderModule EquirectViewportRenderer::loadShader(render::shaders::Spirv spirv) const
{
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.bytes;
    info.pCode = spirv.code;

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_window->device(), &info, nullptr, &module) != VK_SUCCESS) {
        qFatal("Failed to create shader module");
    }
    return module;
}

void EquirectViewportRenderer::createSampler()
{
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // U wraps around the equirect seam; V does not (poles are the top/bottom
    // edges of the texture, not a wraparound).
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    vkCreateSampler(m_window->device(), &info, nullptr, &m_sampler);
}

void EquirectViewportRenderer::createDescriptorSet()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VkResult r = vkCreateDescriptorSetLayout(m_window->device(), &layoutInfo, nullptr,
                                             &m_descriptorSetLayout);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[viewport] vkCreateDescriptorSetLayout failed (VkResult %d); the "
                             "equirect preview and insets won't show\n", r);
        m_descriptorSetLayout = VK_NULL_HANDLE;
        return;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    r = vkCreateDescriptorPool(m_window->device(), &poolInfo, nullptr, &m_descriptorPool);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[viewport] vkCreateDescriptorPool failed (VkResult %d); the "
                             "equirect preview won't show\n", r);
        m_descriptorPool = VK_NULL_HANDLE;
        return;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    r = vkAllocateDescriptorSets(m_window->device(), &allocInfo, &m_descriptorSet);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[viewport] vkAllocateDescriptorSets failed (VkResult %d); the "
                             "equirect preview won't show\n", r);
        m_descriptorSet = VK_NULL_HANDLE;
    }
}

VkPipeline EquirectViewportRenderer::makePipeline(render::shaders::Spirv frag, VkPipelineLayout layout,
                                                  bool blend)
{
    VkShaderModule vertShader = loadShader(render::shaders::equirectVert());
    VkShaderModule fragShader = loadShader(frag);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = m_window->sampleCountFlagBits();

    // QVulkanWindow's default render pass has a depth-stencil attachment; the
    // fullscreen triangle neither tests nor writes it, but the pipeline must
    // still say so (VUID-VkGraphicsPipelineCreateInfo-renderPass-09028).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    static const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = m_window->defaultRenderPass();

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(m_window->device(), VK_NULL_HANDLE, 1,
                                                             &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(m_window->device(), vertShader, nullptr);
    vkDestroyShaderModule(m_window->device(), fragShader, nullptr);
    return r == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

void EquirectViewportRenderer::createPipeline()
{
    if (m_descriptorSetLayout == VK_NULL_HANDLE || !m_luts.ready())
        return; // logged by createDescriptorSet / initResources; no equirect preview
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const VkDescriptorSetLayout sets[2] = {m_descriptorSetLayout, m_luts.layout()};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = sets;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    vkCreatePipelineLayout(m_window->device(), &layoutInfo, nullptr, &m_pipelineLayout);

    m_pipeline = makePipeline(render::shaders::equirectFrag(),
                              m_pipelineLayout);
    if (m_pipeline == VK_NULL_HANDLE)
        qFatal("Failed to create equirect graphics pipeline");
}

void EquirectViewportRenderer::createLensResources()
{
    m_lensCapable = false;
    m_window->setLensFramesSupported(false);
    QVulkanInstance *inst = m_window->vulkanInstance();
    VkDevice device = m_window->device();

    if (!m_luts.ready())
        return; // logged by initResources
    // Needs Vulkan 1.1 (external memory / dedicated allocation structs) and
    // FD import, which MainWindow asks QVulkanWindow to enable.
    if (inst->apiVersion() < QVersionNumber(1, 1) ||
        !m_window->supportedDeviceExtensions().contains(QByteArray(kExternalMemoryFdExt))) {
        std::fprintf(stderr, "[viewport] no external-memory FD import; playback stays on the "
                             "equirect preview\n");
        return;
    }
    auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        inst->getInstanceProcAddr("vkGetPhysicalDeviceProperties2"));
    if (!getProps2)
        return;
    VkPhysicalDeviceIDProperties idp{};
    idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &idp;
    getProps2(m_window->physicalDevice(), &p2);
    std::memcpy(m_deviceUuid, idp.deviceUUID, sizeof m_deviceUuid);
    vkGetPhysicalDeviceMemoryProperties(m_window->physicalDevice(), &m_memProps);

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(device, &si, nullptr, &m_lensSampler) != VK_SUCCESS)
        return;

    VkDescriptorSetLayoutBinding b[5]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 1; i <= 4; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 5;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &m_lensSetLayout) != VK_SUCCESS)
        return;

    const uint32_t frames = static_cast<uint32_t>(std::max(1, m_window->concurrentFrameCount()));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frames},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * frames}};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = frames;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &m_lensPool) != VK_SUCCESS)
        return;
    m_lensSets.assign(frames, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(frames, m_lensSetLayout);
    VkDescriptorSetAllocateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsi.descriptorPool = m_lensPool;
    dsi.descriptorSetCount = frames;
    dsi.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &dsi, m_lensSets.data()) != VK_SUCCESS)
        return;

    m_lensUbo.assign(frames, VK_NULL_HANDLE);
    m_lensUboMem.assign(frames, VK_NULL_HANDLE);
    m_lensUboMap.assign(frames, nullptr);
    for (uint32_t i = 0; i < frames; ++i) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = render::LensFrame::kParamsBytes;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &m_lensUbo[i]) != VK_SUCCESS)
            return;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, m_lensUbo[i], &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = m_window->hostVisibleMemoryIndex();
        if (vkAllocateMemory(device, &ai, nullptr, &m_lensUboMem[i]) != VK_SUCCESS)
            return;
        vkBindBufferMemory(device, m_lensUbo[i], m_lensUboMem[i], 0);
        vkMapMemory(device, m_lensUboMem[i], 0, bi.size, 0, &m_lensUboMap[i]);
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LensPushConstants)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const VkDescriptorSetLayout sets[2] = {m_lensSetLayout, m_luts.layout()};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &pli, nullptr, &m_lensPipelineLayout) != VK_SUCCESS)
        return;
    m_lensPipeline = makePipeline(render::shaders::fisheyeViewFrag(),
                                  m_lensPipelineLayout);
    if (m_lensPipeline == VK_NULL_HANDLE)
        return;

    m_lensCapable = true;
    m_window->setLensFramesSupported(true);
}

void EquirectViewportRenderer::destroyImport(LensImport &imp)
{
    VkDevice device = m_window->device();
    for (int i = 0; i < render::LensSlot::kImages; ++i) {
        if (imp.view[i])
            vkDestroyImageView(device, imp.view[i], nullptr);
        if (imp.img[i])
            vkDestroyImage(device, imp.img[i], nullptr);
        if (imp.mem[i])
            vkFreeMemory(device, imp.mem[i], nullptr);
        imp.view[i] = VK_NULL_HANDLE;
        imp.img[i] = VK_NULL_HANDLE;
        imp.mem[i] = VK_NULL_HANDLE;
    }
}

void EquirectViewportRenderer::destroyLensResources()
{
    VkDevice device = m_window->device();
    for (auto &[id, imp] : m_imports)
        destroyImport(imp);
    m_imports.clear();
    m_retired.clear(); // the device is idle: every frame using them is done
    for (size_t i = 0; i < m_lensUbo.size(); ++i) {
        if (m_lensUboMap[i])
            vkUnmapMemory(device, m_lensUboMem[i]);
        if (m_lensUbo[i])
            vkDestroyBuffer(device, m_lensUbo[i], nullptr);
        if (m_lensUboMem[i])
            vkFreeMemory(device, m_lensUboMem[i], nullptr);
    }
    m_lensUbo.clear();
    m_lensUboMem.clear();
    m_lensUboMap.clear();
    m_lensSets.clear();
    if (m_lensPipeline)
        vkDestroyPipeline(device, m_lensPipeline, nullptr);
    if (m_lensPipelineLayout)
        vkDestroyPipelineLayout(device, m_lensPipelineLayout, nullptr);
    if (m_lensPool)
        vkDestroyDescriptorPool(device, m_lensPool, nullptr);
    if (m_lensSetLayout)
        vkDestroyDescriptorSetLayout(device, m_lensSetLayout, nullptr);
    if (m_lensSampler)
        vkDestroySampler(device, m_lensSampler, nullptr);
    m_lensPipeline = VK_NULL_HANDLE;
    m_lensPipelineLayout = VK_NULL_HANDLE;
    m_lensPool = VK_NULL_HANDLE;
    m_lensSetLayout = VK_NULL_HANDLE;
    m_lensSampler = VK_NULL_HANDLE;
    m_lensCapable = false;
    m_window->setLensFramesSupported(false);
}

void EquirectViewportRenderer::ensureTextureResources(uint32_t width, uint32_t height)
{
    if (width == m_texWidth && height == m_texHeight && m_textureImage != VK_NULL_HANDLE)
        return;

    VkDevice device = m_window->device();

    // Only reached on the first upload and on a frame-size change (e.g. a clip
    // stitched at a different preview resolution) -- never the per-frame path.
    // Make sure no in-flight frame is still reading the old image or writing an
    // old staging buffer before tearing them down.
    if (m_textureImage != VK_NULL_HANDLE || !m_stagingBuffers.empty())
        vkDeviceWaitIdle(device);
    destroyTextureResources();

    // --- Device-local sampled image + view ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &imageInfo, nullptr, &m_textureImage);

        VkMemoryRequirements memReq{};
        vkGetImageMemoryRequirements(device, m_textureImage, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = m_window->deviceLocalMemoryIndex();
        vkAllocateMemory(device, &allocInfo, nullptr, &m_textureMemory);
        vkBindImageMemory(device, m_textureImage, m_textureMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_textureImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &viewInfo, nullptr, &m_textureView);
    }

    // --- Host-visible staging ring (one buffer per frame in flight) ---
    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(width) * height * 4;
    const int frames = std::max(1, m_window->concurrentFrameCount());
    m_stagingBuffers.assign(frames, VK_NULL_HANDLE);
    m_stagingMemory.assign(frames, VK_NULL_HANDLE);
    m_stagingMapped.assign(frames, nullptr);
    for (int i = 0; i < frames; ++i) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufferInfo, nullptr, &m_stagingBuffers[i]);

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(device, m_stagingBuffers[i], &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = m_window->hostVisibleMemoryIndex();
        vkAllocateMemory(device, &allocInfo, nullptr, &m_stagingMemory[i]);
        vkBindBufferMemory(device, m_stagingBuffers[i], m_stagingMemory[i], 0);
        vkMapMemory(device, m_stagingMemory[i], 0, bufferSize, 0, &m_stagingMapped[i]);
    }

    // Point the persistent descriptor set at the new view. Safe without a
    // wait here: the only paths that reach this recreated the resources after
    // a vkDeviceWaitIdle (above) or on the very first upload, so no pending
    // command buffer references the old descriptor contents.
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_textureView;
    imageInfo.sampler = m_sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    if (m_descriptorSet != VK_NULL_HANDLE)
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    m_texWidth = width;
    m_texHeight = height;
    m_textureUndefined = true;
}

void EquirectViewportRenderer::destroyTextureResources()
{
    VkDevice device = m_window->device();

    for (size_t i = 0; i < m_stagingBuffers.size(); ++i) {
        if (m_stagingMapped[i])
            vkUnmapMemory(device, m_stagingMemory[i]);
        if (m_stagingBuffers[i] != VK_NULL_HANDLE)
            vkDestroyBuffer(device, m_stagingBuffers[i], nullptr);
        if (m_stagingMemory[i] != VK_NULL_HANDLE)
            vkFreeMemory(device, m_stagingMemory[i], nullptr);
    }
    m_stagingBuffers.clear();
    m_stagingMemory.clear();
    m_stagingMapped.clear();

    if (m_textureView != VK_NULL_HANDLE)
        vkDestroyImageView(device, m_textureView, nullptr);
    if (m_textureImage != VK_NULL_HANDLE)
        vkDestroyImage(device, m_textureImage, nullptr);
    if (m_textureMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, m_textureMemory, nullptr);
    m_textureView = VK_NULL_HANDLE;
    m_textureImage = VK_NULL_HANDLE;
    m_textureMemory = VK_NULL_HANDLE;
    m_texWidth = 0;
    m_texHeight = 0;
    m_textureUndefined = true;
}

void EquirectViewportRenderer::uploadTextureImmediate(int width, int height,
                                                      const std::vector<uint8_t> &rgba)
{
    ensureTextureResources(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    std::memcpy(m_stagingMapped[0], rgba.data(), rgba.size());

    VkCommandBuffer cmd = beginOneTimeCommands();
    recordTextureCopy(cmd, 0);
    endOneTimeCommands(cmd); // waits for the queue to go idle -- acceptable on
                             // this one-off path (initResources only)
}

void EquirectViewportRenderer::createInsetResources()
{
    VkDevice device = m_window->device();
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(device, &si, nullptr, &m_insetSampler) != VK_SUCCESS)
        m_insetSampler = VK_NULL_HANDLE;

    // One combined image sampler per slot -- the same set layout as the
    // equirect texture's. Without the sets there's no inset pipeline, and
    // startNextFrame draws no insets.
    constexpr uint32_t n = EquirectViewportWindow::kMaxInsets;
    VkDescriptorSet sets[n]{};
    bool setsReady = false;
    if (m_insetSampler != VK_NULL_HANDLE && m_descriptorSetLayout != VK_NULL_HANDLE) {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n};
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = n;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        VkResult r = vkCreateDescriptorPool(device, &dpi, nullptr, &m_insetPool);
        if (r == VK_SUCCESS) {
            VkDescriptorSetLayout layouts[n];
            std::fill(layouts, layouts + n, m_descriptorSetLayout);
            VkDescriptorSetAllocateInfo dsi{};
            dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsi.descriptorPool = m_insetPool;
            dsi.descriptorSetCount = n;
            dsi.pSetLayouts = layouts;
            r = vkAllocateDescriptorSets(device, &dsi, sets);
            if (r == VK_SUCCESS)
                setsReady = true;
            else
                std::fill(sets, sets + n, VK_NULL_HANDLE);
        } else {
            m_insetPool = VK_NULL_HANDLE;
        }
        if (!setsReady)
            std::fprintf(stderr, "[viewport] inset descriptor sets failed (VkResult %d); insets "
                                 "won't show\n", r);
    } else if (m_insetSampler == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[viewport] inset sampler failed; insets won't show\n");
    }
    for (uint32_t i = 0; i < n; ++i) {
        m_insetSlots[i].set = sets[i];
        m_insetSlots[i].key = 0;
    }

    if (setsReady && m_luts.ready()) {
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(InsetPushConstants)};
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout sets[2] = {m_descriptorSetLayout, m_luts.layout()};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(device, &pli, nullptr, &m_insetPipelineLayout);
        m_insetPipeline = makePipeline(render::shaders::insetFrag(),
                                       m_insetPipelineLayout, /*blend=*/true);
        if (m_insetPipeline == VK_NULL_HANDLE)
            std::fprintf(stderr, "[viewport] inset pipeline failed; insets won't show\n");
    }

    VkPushConstantRange mpcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MarkerPushConstants)};
    VkPipelineLayoutCreateInfo mpli{};
    mpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    mpli.pushConstantRangeCount = 1;
    mpli.pPushConstantRanges = &mpcr;
    vkCreatePipelineLayout(device, &mpli, nullptr, &m_markerPipelineLayout);
    m_markerPipeline = makePipeline(render::shaders::markerFrag(),
                                    m_markerPipelineLayout, /*blend=*/true);
    if (m_markerPipeline == VK_NULL_HANDLE)
        std::fprintf(stderr, "[viewport] marker pipeline failed; the tracking box won't show\n");

    VkPushConstantRange rpcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RegionPushConstants)};
    VkPipelineLayoutCreateInfo rpli{};
    rpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    rpli.pushConstantRangeCount = 1;
    rpli.pPushConstantRanges = &rpcr;
    vkCreatePipelineLayout(device, &rpli, nullptr, &m_regionPipelineLayout);
    m_regionPipeline = makePipeline(render::shaders::regionFrag(),
                                    m_regionPipelineLayout, /*blend=*/true);
    if (m_regionPipeline == VK_NULL_HANDLE)
        std::fprintf(stderr, "[viewport] region pipeline failed; the blur regions won't show\n");
}

void EquirectViewportRenderer::destroyInsetSlot(InsetSlot &slot)
{
    VkDevice device = m_window->device();
    for (size_t i = 0; i < slot.staging.size(); ++i) {
        if (slot.stagingMap[i])
            vkUnmapMemory(device, slot.stagingMem[i]);
        vkDestroyBuffer(device, slot.staging[i], nullptr);
        vkFreeMemory(device, slot.stagingMem[i], nullptr);
    }
    slot.staging.clear();
    slot.stagingMem.clear();
    slot.stagingMap.clear();
    if (slot.view)
        vkDestroyImageView(device, slot.view, nullptr);
    if (slot.img)
        vkDestroyImage(device, slot.img, nullptr);
    if (slot.mem)
        vkFreeMemory(device, slot.mem, nullptr);
    slot.view = VK_NULL_HANDLE;
    slot.img = VK_NULL_HANDLE;
    slot.mem = VK_NULL_HANDLE;
    slot.w = slot.h = 0;
    slot.undefined = true;
    slot.key = 0;
}

void EquirectViewportRenderer::destroyInsetResources()
{
    VkDevice device = m_window->device();
    for (InsetSlot &slot : m_insetSlots) {
        destroyInsetSlot(slot);
        slot.set = VK_NULL_HANDLE; // freed with the pool
    }
    if (m_insetPipeline)
        vkDestroyPipeline(device, m_insetPipeline, nullptr);
    if (m_insetPipelineLayout)
        vkDestroyPipelineLayout(device, m_insetPipelineLayout, nullptr);
    if (m_insetPool)
        vkDestroyDescriptorPool(device, m_insetPool, nullptr);
    if (m_markerPipeline)
        vkDestroyPipeline(device, m_markerPipeline, nullptr);
    if (m_markerPipelineLayout)
        vkDestroyPipelineLayout(device, m_markerPipelineLayout, nullptr);
    m_markerPipeline = VK_NULL_HANDLE;
    m_markerPipelineLayout = VK_NULL_HANDLE;
    if (m_regionPipeline)
        vkDestroyPipeline(device, m_regionPipeline, nullptr);
    if (m_regionPipelineLayout)
        vkDestroyPipelineLayout(device, m_regionPipelineLayout, nullptr);
    m_regionPipeline = VK_NULL_HANDLE;
    m_regionPipelineLayout = VK_NULL_HANDLE;
    if (m_insetSampler)
        vkDestroySampler(device, m_insetSampler, nullptr);
    m_insetPipeline = VK_NULL_HANDLE;
    m_insetPipelineLayout = VK_NULL_HANDLE;
    m_insetPool = VK_NULL_HANDLE;
    m_insetSampler = VK_NULL_HANDLE;
}

void EquirectViewportRenderer::ensureInsetSlot(InsetSlot &slot, uint32_t w, uint32_t h)
{
    if (slot.img && slot.w == w && slot.h == h)
        return;
    VkDevice device = m_window->device();
    // A new clip size: rare (per clip, not per frame). Nothing in flight may
    // still read the old image or its descriptor.
    if (slot.img)
        vkDeviceWaitIdle(device);
    const VkDescriptorSet set = slot.set;
    destroyInsetSlot(slot);
    slot.set = set;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device, &ii, nullptr, &slot.img);
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, slot.img, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_window->deviceLocalMemoryIndex();
    vkAllocateMemory(device, &ai, nullptr, &slot.mem);
    vkBindImageMemory(device, slot.img, slot.mem, 0);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = slot.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &slot.view);

    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    const int frames = std::max(1, m_window->concurrentFrameCount());
    slot.staging.assign(frames, VK_NULL_HANDLE);
    slot.stagingMem.assign(frames, VK_NULL_HANDLE);
    slot.stagingMap.assign(frames, nullptr);
    for (int i = 0; i < frames; ++i) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bi, nullptr, &slot.staging[i]);
        VkMemoryRequirements br{};
        vkGetBufferMemoryRequirements(device, slot.staging[i], &br);
        VkMemoryAllocateInfo bai{};
        bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize = br.size;
        bai.memoryTypeIndex = m_window->hostVisibleMemoryIndex();
        vkAllocateMemory(device, &bai, nullptr, &slot.stagingMem[i]);
        vkBindBufferMemory(device, slot.staging[i], slot.stagingMem[i], 0);
        vkMapMemory(device, slot.stagingMem[i], 0, bytes, 0, &slot.stagingMap[i]);
    }

    VkDescriptorImageInfo di{m_insetSampler, slot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = slot.set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
    slot.w = w;
    slot.h = h;
    slot.undefined = true;
}

void EquirectViewportRenderer::initResources()
{
    render::VulkanLutStore::Device lutDev;
    lutDev.phys = m_window->physicalDevice();
    lutDev.device = m_window->device();
    lutDev.queue = m_window->graphicsQueue();
    lutDev.queueFamily = m_window->graphicsQueueFamilyIndex();
    lutDev.waitIdle = [device = lutDev.device] { vkDeviceWaitIdle(device); };
    std::string lutErr;
    if (!m_luts.init(lutDev, VK_SHADER_STAGE_FRAGMENT_BIT, lutErr))
        std::fprintf(stderr, "[viewport] %s; the picture can't be drawn\n", lutErr.c_str());
    createSampler();
    createDescriptorSet();
    createPipeline();
    createLensResources();
    createInsetResources();

    if (!m_lastRgba.empty()) {
        // Resources were rebuilt (e.g. resize on Wayland) -- restore the frame
        // that was on screen rather than blanking to the placeholder.
        uploadTextureImmediate(m_lastWidth, m_lastHeight, m_lastRgba);
    } else {
        // Placeholder so the descriptor set is always valid, even before the
        // first real frame arrives.
        const std::vector<uint8_t> placeholder(4, 128);
        uploadTextureImmediate(1, 1, placeholder);
    }
    // Queued: listeners may open dialogs, which mustn't run inside Qt's
    // Vulkan initialization.
    QMetaObject::invokeMethod(m_window, &EquirectViewportWindow::vulkanReady, Qt::QueuedConnection);
}

void EquirectViewportRenderer::releaseResources()
{
    VkDevice device = m_window->device();

    vkDeviceWaitIdle(device);
    destroyInsetResources();
    destroyLensResources();
    destroyTextureResources();
    m_luts.destroy();

    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
}
