#pragma once

#include "ColorLut.h" // color::Lut3D

#include <vulkan/vulkan.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace render {

// The colour LUTs on one Vulkan device, for the shaders that grade
// (color_grade.glsl's `gradeLut`, set 1 binding 0): each color::Lut3D
// uploaded once as a 3D RGBA16F texture with a linear (trilinear) sampler,
// and a descriptor set for it. Used by the viewport (EquirectViewportRenderer)
// and the GPU stitch / export (VulkanFisheyeStitcher), each on its own device.
//
// Every pipeline that grades adds layout() as its set 1 and binds setFor()
// for the clip it draws. With no LUT the set holds a 2x2x2 identity, which
// the shaders skip anyway (lut mix 0): a set is always bound.
class VulkanLutStore {
public:
    struct Device {
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        // Around vkQueueSubmit when the queue is shared with other threads.
        std::function<void()> lockQueue, unlockQueue;
        // Before a LUT's image is freed (evicted): wait until no frame still
        // in flight samples it. Empty when every submission is waited on
        // before the call that made it returns.
        std::function<void()> waitIdle;
    };

    VulkanLutStore() = default;
    VulkanLutStore(const VulkanLutStore&) = delete;
    VulkanLutStore& operator=(const VulkanLutStore&) = delete;
    ~VulkanLutStore() { destroy(); }

    // The set layout, sampler, pool and the identity LUT. `stages`: the
    // shader stages that sample it.
    bool init(const Device& dev, VkShaderStageFlags stages, std::string& err);
    // Free everything; the caller makes sure the GPU is done with it.
    void destroy();
    bool ready() const { return m_layout != VK_NULL_HANDLE; }

    VkDescriptorSetLayout layout() const { return m_layout; }

    // The set for `lut` (nullptr: the identity one), uploading it on first
    // use -- blocking, a few ms for 65^3. Records nothing into a command
    // buffer, so call it before recording. On an upload failure: the identity
    // set, with *err set.
    VkDescriptorSet setFor(const std::shared_ptr<const color::Lut3D>& lut,
                           std::string* err = nullptr);

    // The shaders' `lut` push constant for `lut` at `mix` (0..1):
    // {mix, domainMin, domainMax, 0}; mix 0 when there's no LUT.
    static void pushValues(const color::Lut3D* lut, float mix, float out[4]);

    // How many LUTs stay uploaded; past it the least recently used goes.
    static constexpr size_t kMaxLuts = 8;

private:
    struct Entry {
        std::shared_ptr<const color::Lut3D> lut; // keeps the key's address unique
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t used = 0;
    };

    bool upload(const color::Lut3D& lut, Entry& e, std::string& err);
    void free(Entry& e);
    bool findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out) const;

    Device m_dev;
    VkPhysicalDeviceMemoryProperties m_memProps{};
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    Entry m_identity;
    std::vector<Entry> m_luts;
    uint64_t m_clock = 0;
};

} // namespace render
