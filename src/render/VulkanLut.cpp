#include "VulkanLut.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace render {

namespace {

// IEEE half from float, round to nearest even; LUT values are finite.
uint16_t toHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = int32_t((x >> 23) & 0xffu) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (exp >= 31)
        return uint16_t(sign | 0x7c00u); // too large: infinity
    if (exp <= 0) {
        if (exp < -10)
            return uint16_t(sign); // too small: zero
        mant |= 0x800000u;
        const uint32_t shift = uint32_t(14 - exp);
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u)))
            ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u)))
        ++h; // may carry into the exponent, which is still right
    return uint16_t(sign | h);
}

constexpr VkFormat kLutFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // linear filtering is required

} // namespace

bool VulkanLutStore::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want,
                                    uint32_t& out) const
{
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (m_memProps.memoryTypes[i].propertyFlags & want) == want) {
            out = i;
            return true;
        }
    }
    return false;
}

bool VulkanLutStore::init(const Device& dev, VkShaderStageFlags stages, std::string& err)
{
    destroy();
    m_dev = dev;
    vkGetPhysicalDeviceMemoryProperties(dev.phys, &m_memProps);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(dev.device, &si, nullptr, &m_sampler) != VK_SUCCESS) {
        err = "vkCreateSampler (LUT) failed";
        return false;
    }
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = stages;
    b.pImmutableSamplers = &m_sampler;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &m_layout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout (LUT) failed";
        destroy();
        return false;
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(kMaxLuts + 1)};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = uint32_t(kMaxLuts + 1);
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev.device, &pi, nullptr, &m_pool) != VK_SUCCESS) {
        err = "vkCreateDescriptorPool (LUT) failed";
        destroy();
        return false;
    }
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ci.queueFamilyIndex = dev.queueFamily;
    if (vkCreateCommandPool(dev.device, &ci, nullptr, &m_cmdPool) != VK_SUCCESS) {
        err = "vkCreateCommandPool (LUT) failed";
        destroy();
        return false;
    }
    // The identity, so a set is always there to bind.
    color::Lut3D identity;
    identity.size = 2;
    for (int bl = 0; bl < 2; ++bl)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r)
                identity.rgb.insert(identity.rgb.end(), {float(r), float(g), float(bl)});
    if (!upload(identity, m_identity, err)) {
        destroy();
        return false;
    }
    return true;
}

void VulkanLutStore::free(Entry& e)
{
    VkDevice dev = m_dev.device;
    if (e.set && m_pool)
        vkFreeDescriptorSets(dev, m_pool, 1, &e.set);
    if (e.view)
        vkDestroyImageView(dev, e.view, nullptr);
    if (e.image)
        vkDestroyImage(dev, e.image, nullptr);
    if (e.mem)
        vkFreeMemory(dev, e.mem, nullptr);
    e = Entry{};
}

void VulkanLutStore::destroy()
{
    if (!m_dev.device)
        return;
    for (Entry& e : m_luts)
        free(e);
    m_luts.clear();
    free(m_identity);
    VkDevice dev = m_dev.device;
    if (m_cmdPool)
        vkDestroyCommandPool(dev, m_cmdPool, nullptr);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_layout)
        vkDestroyDescriptorSetLayout(dev, m_layout, nullptr);
    if (m_sampler)
        vkDestroySampler(dev, m_sampler, nullptr);
    m_cmdPool = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_dev = Device{};
}

bool VulkanLutStore::upload(const color::Lut3D& lut, Entry& e, std::string& err)
{
    VkDevice dev = m_dev.device;
    const uint32_t n = uint32_t(lut.size);
    const size_t entries = size_t(n) * n * n;
    const VkDeviceSize bytes = entries * 4 * sizeof(uint16_t);

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.format = kLutFormat;
    ii.extent = {n, n, n};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &e.image) != VK_SUCCESS) {
        err = "vkCreateImage (LUT) failed";
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, e.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        ai.memoryTypeIndex) ||
        vkAllocateMemory(dev, &ai, nullptr, &e.mem) != VK_SUCCESS ||
        vkBindImageMemory(dev, e.image, e.mem, 0) != VK_SUCCESS) {
        err = "no device memory for the LUT";
        free(e);
        return false;
    }

    // Staging: RGBA16F, red fastest -- the .cube order is the texel order.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    auto cleanup = [&] {
        if (fence)
            vkDestroyFence(dev, fence, nullptr);
        if (cmd)
            vkFreeCommandBuffers(dev, m_cmdPool, 1, &cmd);
        if (staging)
            vkDestroyBuffer(dev, staging, nullptr);
        if (stagingMem)
            vkFreeMemory(dev, stagingMem, nullptr);
    };
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    void* map = nullptr;
    bool ok = vkCreateBuffer(dev, &bi, nullptr, &staging) == VK_SUCCESS;
    if (ok) {
        vkGetBufferMemoryRequirements(dev, staging, &req);
        ai.allocationSize = req.size;
        ok = findMemoryType(req.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            ai.memoryTypeIndex) &&
             vkAllocateMemory(dev, &ai, nullptr, &stagingMem) == VK_SUCCESS &&
             vkBindBufferMemory(dev, staging, stagingMem, 0) == VK_SUCCESS &&
             vkMapMemory(dev, stagingMem, 0, bytes, 0, &map) == VK_SUCCESS;
    }
    if (!ok) {
        err = "no staging memory for the LUT";
        cleanup();
        free(e);
        return false;
    }
    auto* px = static_cast<uint16_t*>(map);
    const uint16_t one = toHalf(1.0f);
    for (size_t i = 0; i < entries; ++i) {
        px[i * 4 + 0] = toHalf(lut.rgb[i * 3 + 0]);
        px[i * 4 + 1] = toHalf(lut.rgb[i * 3 + 1]);
        px[i * 4 + 2] = toHalf(lut.rgb[i * 3 + 2]);
        px[i * 4 + 3] = one;
    }
    vkUnmapMemory(dev, stagingMem);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = m_cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS ||
        vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS) {
        err = "can't record the LUT upload";
        cleanup();
        free(e);
        return false;
    }
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbi);
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = e.image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {n, n, n};
    vkCmdCopyBufferToImage(cmd, staging, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    if (m_dev.lockQueue)
        m_dev.lockQueue();
    VkResult r = vkQueueSubmit(m_dev.queue, 1, &sub, fence);
    if (m_dev.unlockQueue)
        m_dev.unlockQueue();
    if (r == VK_SUCCESS)
        r = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    cleanup();
    if (r != VK_SUCCESS) {
        err = "the LUT upload failed, VkResult " + std::to_string(r);
        free(e);
        return false;
    }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = e.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = kLutFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = m_pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_layout;
    if (vkCreateImageView(dev, &vi, nullptr, &e.view) != VK_SUCCESS ||
        vkAllocateDescriptorSets(dev, &dai, &e.set) != VK_SUCCESS) {
        err = "can't make the LUT's descriptor";
        free(e);
        return false;
    }
    VkDescriptorImageInfo di{m_sampler, e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = e.set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &di;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return true;
}

VkDescriptorSet VulkanLutStore::setFor(const std::shared_ptr<const color::Lut3D>& lut,
                                       std::string* err)
{
    if (!lut)
        return m_identity.set;
    ++m_clock;
    for (Entry& e : m_luts) {
        if (e.lut == lut) {
            e.used = m_clock;
            return e.set;
        }
    }
    if (m_luts.size() >= kMaxLuts) {
        auto lru = std::min_element(m_luts.begin(), m_luts.end(),
                                    [](const Entry& a, const Entry& b) { return a.used < b.used; });
        if (m_dev.waitIdle)
            m_dev.waitIdle();
        free(*lru);
        m_luts.erase(lru);
    }
    Entry e;
    std::string why;
    if (!upload(*lut, e, why)) {
        if (err)
            *err = why;
        return m_identity.set;
    }
    e.lut = lut;
    e.used = m_clock;
    m_luts.push_back(std::move(e));
    return m_luts.back().set;
}

void VulkanLutStore::pushValues(const color::Lut3D* lut, float mix, float out[4])
{
    out[0] = lut ? std::clamp(mix, 0.0f, 1.0f) : 0.0f;
    out[1] = lut ? lut->domainMin : 0.0f;
    out[2] = lut ? lut->domainMax : 1.0f;
    out[3] = 0.0f;
}

} // namespace render
