#include "VulkanFisheyeStitcherImpl.h"

#include "EmbeddedShaders.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// The stitcher's device (its own, or the decoder's adopted one), its
// pipelines, and the images and buffers they use, made as they are needed
// and freed with it.

namespace render {

std::atomic<uint64_t> LensRingCore::nextId{1};

bool VulkanFisheyeStitcher::Impl::ensureFlat(VkDeviceSize bytes, std::string& err)
{
    if (flatBuf && bytes <= flatBytes)
        return true;
    idleBeforeFree();
    if (flatMap) { vkUnmapMemory(device, flatMem); flatMap = nullptr; }
    if (flatBuf) vkDestroyBuffer(device, flatBuf, nullptr);
    if (flatMem) vkFreeMemory(device, flatMem, nullptr);
    flatBuf = VK_NULL_HANDLE;
    flatMem = VK_NULL_HANDLE;
    flatBytes = 0;
    if (!makeReadbackBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, flatBuf, flatMem,
                            &flatMap)) {
        err = "flat output buffer allocation failed";
        return false;
    }
    flatBytes = bytes;
    return true;
}

bool VulkanFisheyeStitcher::Impl::makeReadbackBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                     VkBuffer& buf, VkDeviceMemory& mem, void** map)
{
    const VkMemoryPropertyFlags coherent =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (makeBuffer(size, usage, coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, buf, mem, map))
        return true;
    if (buf) {
        vkDestroyBuffer(device, buf, nullptr);
        buf = VK_NULL_HANDLE;
    }
    return makeBuffer(size, usage, coherent, buf, mem, map);
}

bool VulkanFisheyeStitcher::Impl::makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                             VkMemoryPropertyFlags mprops, VkBuffer& buf,
                                             VkDeviceMemory& mem, void** map)
{
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buf, &req);
    uint32_t type = 0;
    if (!findMemoryType(req.memoryTypeBits, mprops, type))
        return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(device, buf, mem, 0);
    if (map)
        vkMapMemory(device, mem, 0, size, 0, map);
    return true;
}

void VulkanFisheyeStitcher::Impl::destroyResources()
{
    for (int i = 0; i < 2; ++i) {
        if (fisheyeView[i]) vkDestroyImageView(device, fisheyeView[i], nullptr);
        if (fisheye[i]) vkDestroyImage(device, fisheye[i], nullptr);
        if (fisheyeMem[i]) vkFreeMemory(device, fisheyeMem[i], nullptr);
        fisheyeView[i] = VK_NULL_HANDLE;
        fisheye[i] = VK_NULL_HANDLE;
        fisheyeMem[i] = VK_NULL_HANDLE;
    }
    auto killBuf = [&](VkBuffer& b, VkDeviceMemory& m, void*& mp) {
        if (mp) { vkUnmapMemory(device, m); mp = nullptr; }
        if (b) vkDestroyBuffer(device, b, nullptr);
        if (m) vkFreeMemory(device, m, nullptr);
        b = VK_NULL_HANDLE;
        m = VK_NULL_HANDLE;
    };
    killBuf(staging, stagingMem, stagingMap);
    killBuf(ubo, uboMem, uboMap);
    killBuf(outBuf, outMem, outMap);
    killBuf(gainBuf, gainMem, gainMap);
    killBuf(seamBuf, seamMem, seamMap);
    killBuf(flatBuf, flatMem, flatMap);
    fw = fh = ew = eh = 0;
    flatBytes = 0;
}

VkImageView VulkanFisheyeStitcher::Impl::nv12ViewFor(VkImage img, VkFormat fmt)
{
    // The ycbcr conversion is for one format; a view of another is invalid.
    if (fmt != lensFormat)
        return VK_NULL_HANDLE;
    auto it = nv12Views.find(img);
    if (it != nv12Views.end())
        return it->second;
    // The decoder's images carry STORAGE (and video) usage, which the
    // 2-plane format can't back in a view: sample-only views. The ycbcr
    // conversion (made for the frame format) is on the immutable sampler AND
    // chained here when it matches the image's format -- a mismatched
    // conversion would make the view invalid.
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkSamplerYcbcrConversionInfo yci{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    yci.pNext = &usage;
    yci.conversion = ycbcr;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = fmt == lensFormat ? static_cast<const void*>(&yci)
                                 : static_cast<const void*>(&usage);
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    nv12Views.emplace(img, view);
    return view;
}

bool VulkanFisheyeStitcher::Impl::ensureUbo(std::string& err)
{
    if (ubo)
        return true;
    const VkMemoryPropertyFlags hostVis =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!makeBuffer(sizeof(ShaderParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostVis, ubo,
                    uboMem, &uboMap) ||
        !makeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    hostVis, gainBuf, gainMem, &gainMap) ||
        !makeReadbackBuffer(kSeamBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, seamBuf, seamMem,
                            &seamMap)) {
        err = "uniform / gain / seam buffer allocation failed";
        return false;
    }
    VkDescriptorBufferInfo uboInfo{ubo, 0, sizeof(ShaderParams)};
    VkDescriptorBufferInfo gainInfo{gainBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo seamInfo{seamBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[6]{};
    int n = 0;
    for (VkDescriptorSet set : {descSet, vwSet}) {
        if (!set)
            continue;
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = set;
        w[n].dstBinding = 0;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[n].pBufferInfo = &uboInfo;
        ++n;
        w[n] = w[n - 1];
        w[n].dstBinding = 4;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[n].pBufferInfo = &gainInfo;
        ++n;
        w[n] = w[n - 1];
        w[n].dstBinding = 5;
        w[n].pBufferInfo = &seamInfo;
        ++n;
    }
    vkUpdateDescriptorSets(device, n, w, 0, nullptr);
    return true;
}

bool VulkanFisheyeStitcher::Impl::ensureOutput(int new_, int neh, std::string& err)
{
    if (!ensureUbo(err))
        return false;
    if (new_ == ew && neh == eh && outBuf)
        return true;
    idleBeforeFree();
    if (outMap) { vkUnmapMemory(device, outMem); outMap = nullptr; }
    if (outBuf) vkDestroyBuffer(device, outBuf, nullptr);
    if (outMem) vkFreeMemory(device, outMem, nullptr);
    outBuf = VK_NULL_HANDLE;
    outMem = VK_NULL_HANDLE;
    if (!makeReadbackBuffer(static_cast<VkDeviceSize>(new_) * neh * 4,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, outBuf, outMem, &outMap)) {
        err = "host-visible buffer allocation failed";
        return false;
    }
    VkDescriptorBufferInfo outInfo{outBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = descSet;
    w.dstBinding = 3;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &outInfo;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    ew = new_;
    eh = neh;
    return true;
}

bool VulkanFisheyeStitcher::Impl::makeExportImage(VkFormat fmt, int w, int h, VkImage& img,
                                                  VkDeviceMemory& mem, uint64_t& size, int& fd,
                                                  std::string& err)
{
    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.pNext = &emi;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ii, nullptr, &img) != VK_SUCCESS) {
        err = "vkCreateImage (exportable) failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img, &req);
    uint32_t type = 0;
    if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
        err = "no device-local memory for an exportable image";
        return false;
    }
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = img;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &exp;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) {
        err = "vkAllocateMemory (exportable) failed";
        return false;
    }
    vkBindImageMemory(device, img, mem, 0);
    VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gfi.memory = mem;
    gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (getMemoryFd(device, &gfi, &fd) != VK_SUCCESS) {
        err = "vkGetMemoryFdKHR failed";
        return false;
    }
    size = req.size;
    return true;
}

LensRingCore::Slot* VulkanFisheyeStitcher::Impl::acquireLensSlot(int lensW, int lensH, std::string& err)
{
    if (!ring) {
        ring = std::make_shared<LensRingCore>();
        ring->device = device;
        ring->keepAlive = keepAlive;
    }
    std::unique_lock<std::mutex> lk(ring->mx);
    const auto deadline = std::chrono::steady_clock::now() + kLensSlotWait;
    for (;;) {
        for (auto& sl : ring->slots)
            if (!sl->busy && sl->pub.lensW == lensW && sl->pub.lensH == lensH &&
                sl->pub.bitDepth == (deepLens() ? 10 : 8)) {
                sl->busy = true;
                return sl.get();
            }
        const size_t cap = std::chrono::steady_clock::now() < deadline ? kLensSlotsSoft
                                                                       : kLensSlotsHard;
        if (ring->slots.size() < cap)
            break;
        if (ring->slots.size() >= kLensSlotsHard &&
            std::chrono::steady_clock::now() >= deadline) {
            err = "lens ring exhausted (every slot held)";
            return nullptr;
        }
        ring->cv.wait_until(lk, deadline);
    }
    // Drop free slots of another size first (a source's lens size doesn't
    // change, but be tidy).
    for (auto it = ring->slots.begin(); it != ring->slots.end();) {
        if (!(*it)->busy && ((*it)->pub.lensW != lensW || (*it)->pub.lensH != lensH ||
                             (*it)->pub.bitDepth != (deepLens() ? 10 : 8))) {
            ring->destroySlot(**it);
            it = ring->slots.erase(it);
        } else {
            ++it;
        }
    }
    auto sl = std::make_unique<LensRingCore::Slot>();
    sl->pub.lensW = lensW;
    sl->pub.lensH = lensH;
    sl->pub.bitDepth = deepLens() ? 10 : 8;
    sl->pub.id = LensRingCore::nextId.fetch_add(1);
    std::memcpy(sl->pub.deviceUuid, deviceUuid, sizeof deviceUuid);
    for (int i = 0; i < LensSlot::kImages; ++i) {
        const bool uv = (i & 1) != 0;
        // A plane copy needs the plane's size: 16-bit samples for P010.
        const VkFormat fmt = deepLens() ? (uv ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16_UNORM)
                                        : (uv ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM);
        if (!makeExportImage(fmt,
                             uv ? lensW / 2 : lensW, uv ? lensH / 2 : lensH, sl->img[i],
                             sl->mem[i], sl->pub.memSize[i], sl->pub.fd[i], err)) {
            ring->destroySlot(*sl);
            return nullptr;
        }
    }
    sl->busy = true;
    ring->slots.push_back(std::move(sl));
    return ring->slots.back().get();
}

bool VulkanFisheyeStitcher::Impl::ensureResources(int nfw, int nfh, int new_, int neh, std::string& err)
{
    if (!ensureOutput(new_, neh, err))
        return false;
    if (nfw == fw && nfh == fh && fisheye[0])
        return true;
    idleBeforeFree();
    for (int i = 0; i < 2; ++i) {
        if (fisheyeView[i]) vkDestroyImageView(device, fisheyeView[i], nullptr);
        if (fisheye[i]) vkDestroyImage(device, fisheye[i], nullptr);
        if (fisheyeMem[i]) vkFreeMemory(device, fisheyeMem[i], nullptr);
        fisheyeView[i] = VK_NULL_HANDLE;
        fisheye[i] = VK_NULL_HANDLE;
        fisheyeMem[i] = VK_NULL_HANDLE;
    }
    if (stagingMap) { vkUnmapMemory(device, stagingMem); stagingMap = nullptr; }
    if (staging) vkDestroyBuffer(device, staging, nullptr);
    if (stagingMem) vkFreeMemory(device, stagingMem, nullptr);
    staging = VK_NULL_HANDLE;
    stagingMem = VK_NULL_HANDLE;

    const VkDeviceSize onePix = static_cast<VkDeviceSize>(nfw) * nfh;
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {static_cast<uint32_t>(nfw), static_cast<uint32_t>(nfh), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ii, nullptr, &fisheye[i]) != VK_SUCCESS) {
            err = "vkCreateImage failed";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, fisheye[i], &req);
        uint32_t type = 0;
        if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
            err = "no device-local memory for fisheye image";
            return false;
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &fisheyeMem[i]) != VK_SUCCESS) {
            err = "vkAllocateMemory (fisheye) failed";
            return false;
        }
        vkBindImageMemory(device, fisheye[i], fisheyeMem[i], 0);

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = fisheye[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &fisheyeView[i]) != VK_SUCCESS) {
            err = "vkCreateImageView failed";
            return false;
        }
    }

    const VkMemoryPropertyFlags hostVis =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!makeBuffer(onePix * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVis, staging,
                    stagingMem, &stagingMap)) {
        err = "staging buffer allocation failed";
        return false;
    }

    // Point descriptor bindings 1 & 2 at the fixed upload images.
    VkDescriptorImageInfo img0{sampler, fisheyeView[0],
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo img1{sampler, fisheyeView[1],
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = descSet;
    w[0].dstBinding = 1;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &img0;
    w[1] = w[0];
    w[1].dstBinding = 2;
    w[1].pImageInfo = &img1;
    vkUpdateDescriptorSets(device, 2, w, 0, nullptr);

    fw = nfw;
    fh = nfh;
    return true;
}

VulkanFisheyeStitcher::VulkanFisheyeStitcher() : d(new Impl) {}

VulkanFisheyeStitcher::~VulkanFisheyeStitcher()
{
    if (d && d->device) {
        // On an adopted device, idle only our queue -- FFmpeg may still be
        // using the device on another thread.
        if (d->adopted) {
            if (d->lockQueue)
                d->lockQueue();
            vkQueueWaitIdle(d->queue);
            if (d->unlockQueue)
                d->unlockQueue();
        } else
            vkDeviceWaitIdle(d->device);
        d->destroyResources();
        d->ring.reset(); // freed now, or by the last LensFrame still holding a slot
        for (auto& [img, view] : d->nv12Views)
            vkDestroyImageView(d->device, view, nullptr);
        d->nv12Views.clear();
        d->luts.destroy();
        if (d->pipeline) vkDestroyPipeline(d->device, d->pipeline, nullptr);
        if (d->pipeLayout) vkDestroyPipelineLayout(d->device, d->pipeLayout, nullptr);
        if (d->descPool) vkDestroyDescriptorPool(d->device, d->descPool, nullptr);
        if (d->setLayout) vkDestroyDescriptorSetLayout(d->device, d->setLayout, nullptr);
        if (d->vwPipeline) vkDestroyPipeline(d->device, d->vwPipeline, nullptr);
        if (d->vwPipeLayout) vkDestroyPipelineLayout(d->device, d->vwPipeLayout, nullptr);
        if (d->rfPipeline) vkDestroyPipeline(d->device, d->rfPipeline, nullptr);
        if (d->rfPipeLayout) vkDestroyPipelineLayout(d->device, d->rfPipeLayout, nullptr);
        if (d->rfDescPool) vkDestroyDescriptorPool(d->device, d->rfDescPool, nullptr);
        if (d->rfSetLayout) vkDestroyDescriptorSetLayout(d->device, d->rfSetLayout, nullptr);
        if (d->sampler) vkDestroySampler(d->device, d->sampler, nullptr);
        if (d->ycbcr) vkDestroySamplerYcbcrConversion(d->device, d->ycbcr, nullptr);
        if (d->tsPool) vkDestroyQueryPool(d->device, d->tsPool, nullptr);
        if (d->fence) vkDestroyFence(d->device, d->fence, nullptr);
        if (d->cmdPool) vkDestroyCommandPool(d->device, d->cmdPool, nullptr);
        if (!d->adopted)
            vkDestroyDevice(d->device, nullptr);
    }
    if (d && d->instance && !d->adopted)
        vkDestroyInstance(d->instance, nullptr);
    delete d;
}

bool VulkanFisheyeStitcher::init(std::string& err)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "orbis-reframe-stitch";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char* valLayer = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("INSV_VK_VALIDATE")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &valLayer;
    }
    if (vkCreateInstance(&ici, nullptr, &d->instance) != VK_SUCCESS) {
        err = "vkCreateInstance failed";
        return false;
    }

    uint32_t nphys = 0;
    vkEnumeratePhysicalDevices(d->instance, &nphys, nullptr);
    if (nphys == 0) {
        err = "no Vulkan physical devices";
        return false;
    }
    std::vector<VkPhysicalDevice> devs(nphys);
    vkEnumeratePhysicalDevices(d->instance, &nphys, devs.data());
    d->phys = devs[0];
    for (VkPhysicalDevice pd : devs) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            d->phys = pd;
            break;
        }
    }
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memProps);

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nqf, qfs.data());
    bool found = false;
    for (uint32_t i = 0; i < nqf; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            d->queueFamily = i;
            found = true;
            break;
        }
    }
    if (!found) {
        err = "no compute queue family";
        return false;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d->queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(d->phys, &dci, nullptr, &d->device) != VK_SUCCESS) {
        err = "vkCreateDevice failed";
        return false;
    }
    vkGetDeviceQueue(d->device, d->queueFamily, 0, &d->queue);

    return bringUp(false, err);
}

bool VulkanFisheyeStitcher::initAdopt(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                                      uint32_t computeQueueFamily,
                                      VkDeviceQueueCreateFlags queueFlags, std::string& err,
                                      VkFormat lensFormat)
{
    if (lensFormat != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
        lensFormat != VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16) {
        err = "unsupported lens picture format " + std::to_string(int(lensFormat));
        return false;
    }
    // The stitch samples the decoder's pictures through a ycbcr conversion,
    // filtered, with midpoint chroma.
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys, lensFormat, &fp);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT |
        VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        err = lensFormat == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
                  ? "the device can't sample 8-bit (NV12) pictures through a ycbcr conversion"
                  : "the device can't sample 10-bit (P010) pictures through a ycbcr conversion";
        return false;
    }
    d->lensFormat = lensFormat;
    d->adopted = true;
    d->instance = inst;
    d->phys = phys;
    d->device = dev;
    d->queueFamily = computeQueueFamily;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memProps);
    if (queueFlags) {
        // FFmpeg created its queues with these flags (INTERNALLY_SYNCHRONIZED
        // on FFmpeg 9+ with drivers that offer it) -- vkGetDeviceQueue would
        // return NULL.
        VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
        qi.flags = queueFlags;
        qi.queueFamilyIndex = d->queueFamily;
        qi.queueIndex = 0;
        vkGetDeviceQueue2(d->device, &qi, &d->queue);
    } else {
        vkGetDeviceQueue(d->device, d->queueFamily, 0, &d->queue);
    }
    if (!d->queue) {
        err = "no compute queue on FFmpeg's device";
        return false;
    }
    // Present when FFmpeg enabled VK_KHR_external_memory_fd (it does, for its
    // own interop); null => no lens export, playback stays on the equirect.
    d->getMemoryFd =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
    {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        std::memcpy(d->deviceUuid, idp.deviceUUID, sizeof d->deviceUuid);
    }
    return bringUp(true, err);
}

bool VulkanFisheyeStitcher::bringUp(bool nv12Mode, std::string& err)
{
    d->nv12 = nv12Mode;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d->queueFamily;
    if (vkCreateCommandPool(d->device, &pci, nullptr, &d->cmdPool) != VK_SUCCESS) {
        err = "vkCreateCommandPool failed";
        return false;
    }

    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = d->cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(d->device, &cbi, &d->cmd);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(d->device, &fci, nullptr, &d->fence);

    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nq, qf.data());
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d->phys, &props);
        if (d->queueFamily < nq && qf[d->queueFamily].timestampValidBits > 0 &&
            props.limits.timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 3;
            if (vkCreateQueryPool(d->device, &qpi, nullptr, &d->tsPool) == VK_SUCCESS)
                d->tsPeriodNs = props.limits.timestampPeriod;
        }
    }

    // The ycbcr conversion carries the decoded frames' real format (G10X6...
    // for the X6's Main-10 streams, G8_B8R8 for 8-bit): the spec needs it to
    // match the image's, and RADV applies the conversion only when it rides
    // on the immutable sampler baked into the set layout -- a sampler handed
    // in per descriptor write is ignored for this.
    VkSamplerYcbcrConversionInfo yci{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    if (nv12Mode) {
        VkSamplerYcbcrConversionCreateInfo ycci{
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
        ycci.format = d->lensFormat; // NV12, or P010 for 10-bit
        ycci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
        ycci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL; // camera HEVC is full range
        ycci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        ycci.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ycci.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ycci.chromaFilter = VK_FILTER_LINEAR;
        if (vkCreateSamplerYcbcrConversion(d->device, &ycci, nullptr, &d->ycbcr) != VK_SUCCESS) {
            err = "vkCreateSamplerYcbcrConversion failed (device lacks samplerYcbcrConversion)";
            return false;
        }
        yci.conversion = d->ycbcr;
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    if (nv12Mode)
        si.pNext = &yci;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    if (vkCreateSampler(d->device, &si, nullptr, &d->sampler) != VK_SUCCESS) {
        err = "vkCreateSampler failed";
        return false;
    }

    // Bindings 1 & 2 use an immutable sampler in nv12 mode (the ycbcr
    // conversion must be baked into the layout for RADV to apply it).
    const VkSampler immutable[2] = {d->sampler, d->sampler};
    VkDescriptorSetLayoutBinding b[6]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
            nv12Mode ? &immutable[0] : nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
            nv12Mode ? &immutable[1] : nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 6;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->device, &sli, nullptr, &d->setLayout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    // A sampler with a ycbcr conversion can take more than one descriptor
    // of the pool; the driver says how many (NV12: 1 on NVIDIA, 2 on RADV
    // and llvmpipe).
    uint32_t samplerDescriptors = 1;
    if (nv12Mode) {
        VkPhysicalDeviceImageFormatInfo2 fi{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        fi.format = d->lensFormat;
        fi.type = VK_IMAGE_TYPE_2D;
        fi.tiling = VK_IMAGE_TILING_OPTIMAL;
        fi.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkSamplerYcbcrConversionImageFormatProperties yp{
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &yp};
        if (vkGetPhysicalDeviceImageFormatProperties2(d->phys, &fi, &fp) == VK_SUCCESS)
            samplerDescriptors = std::max(1u, yp.combinedImageSamplerDescriptorCount);
    }

    // Two sets of this layout: the stitch's, and the fused view pass's.
    VkDescriptorPoolSize ps[3]{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * samplerDescriptors},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 4;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->device, &dpi, nullptr, &d->descPool) != VK_SUCCESS) {
        err = "vkCreateDescriptorPool failed";
        return false;
    }

    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = d->descPool;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &d->setLayout;
    const VkResult ar = vkAllocateDescriptorSets(d->device, &dsi, &d->descSet);
    if (ar != VK_SUCCESS) {
        err = "vkAllocateDescriptorSets (stitch) failed, VkResult " + std::to_string(ar);
        return false;
    }

    const shaders::Spirv spv =
        nv12Mode ? shaders::fisheyeStitchNv12Comp() : shaders::fisheyeStitchComp();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.bytes;
    smi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed";
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4}; // int pass
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d->setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(d->device, &pli, nullptr, &d->pipeLayout);

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = d->pipeLayout;
    const VkResult pr =
        vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d->pipeline);
    vkDestroyShaderModule(d->device, module, nullptr);
    if (pr != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }

    // The grade's LUTs, for the reproject and view passes below. The queue
    // lock is set after initAdopt(), so look it up at each upload.
    VulkanLutStore::Device lutDev;
    lutDev.phys = d->phys;
    lutDev.device = d->device;
    lutDev.queue = d->queue;
    lutDev.queueFamily = d->queueFamily;
    lutDev.lockQueue = [impl = d] {
        if (impl->lockQueue)
            impl->lockQueue();
    };
    lutDev.unlockQueue = [impl = d] {
        if (impl->unlockQueue)
            impl->unlockQueue();
    };
    lutDev.waitIdle = [impl = d] { impl->idleBeforeFree(); };
    if (!d->luts.init(lutDev, VK_SHADER_STAGE_COMPUTE_BIT, err))
        return false;

    // The export reproject pass is optional -- a build without the SPV, or a
    // pipeline-creation failure, just leaves reframeReady() false and callers
    // fall back to the CPU reproject.
    std::string rferr;
    if (!buildReframePipeline(rferr))
        std::fprintf(stderr, "[VulkanFisheyeStitcher] reframe reproject pass unavailable (%s); "
                             "export will reproject on the CPU\n",
                     rferr.c_str());
    // Likewise the fused fisheye -> view pass (NV12 inputs only).
    if (nv12Mode) {
        std::string vwerr;
        if (!buildViewPipeline(vwerr))
            std::fprintf(stderr, "[VulkanFisheyeStitcher] fused view pass unavailable (%s)\n",
                         vwerr.c_str());
    }

    m_ready = true;
    return true;
}

bool VulkanFisheyeStitcher::buildReframePipeline(std::string& err)
{

    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->device, &sli, nullptr, &d->rfSetLayout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout (reframe) failed";
        return false;
    }

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d->device, &dpi, nullptr, &d->rfDescPool) != VK_SUCCESS) {
        err = "vkCreateDescriptorPool (reframe) failed";
        return false;
    }
    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = d->rfDescPool;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &d->rfSetLayout;
    if (vkAllocateDescriptorSets(d->device, &dsi, &d->rfSet) != VK_SUCCESS) {
        err = "vkAllocateDescriptorSets (reframe) failed";
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ReframePush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout sets[2] = {d->rfSetLayout, d->luts.layout()};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d->device, &pli, nullptr, &d->rfPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout (reframe) failed";
        return false;
    }

    const shaders::Spirv spv = shaders::reframeReprojectComp();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.bytes;
    smi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule (reframe) failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = d->rfPipeLayout;
    const VkResult pr =
        vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d->rfPipeline);
    vkDestroyShaderModule(d->device, module, nullptr);
    if (pr != VK_SUCCESS) {
        err = "vkCreateComputePipelines (reframe) failed";
        return false;
    }

    m_reframeReady = true;
    return true;
}

bool VulkanFisheyeStitcher::buildViewPipeline(std::string& err)
{
    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = d->descPool;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &d->setLayout;
    const VkResult ar = vkAllocateDescriptorSets(d->device, &dsi, &d->vwSet);
    if (ar != VK_SUCCESS) {
        err = "vkAllocateDescriptorSets (view) failed, VkResult " + std::to_string(ar);
        return false;
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ViewPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout sets[2] = {d->setLayout, d->luts.layout()};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d->device, &pli, nullptr, &d->vwPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout (view) failed";
        return false;
    }

    const shaders::Spirv spv = shaders::fisheyeViewNv12Comp();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.bytes;
    smi.pCode = spv.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d->device, &smi, nullptr, &module) != VK_SUCCESS) {
        err = "vkCreateShaderModule (view) failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = d->vwPipeLayout;
    const VkResult pr =
        vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d->vwPipeline);
    vkDestroyShaderModule(d->device, module, nullptr);
    if (pr != VK_SUCCESS) {
        err = "vkCreateComputePipelines (view) failed";
        return false;
    }
    m_viewReady = true;
    return true;
}

bool VulkanFisheyeStitcher::lensExportReady() const
{
    return m_ready && d->nv12 && m_viewReady && d->getMemoryFd;
}

void VulkanFisheyeStitcher::setQueueLock(std::function<void()> lock, std::function<void()> unlock)
{
    d->lockQueue = std::move(lock);
    d->unlockQueue = std::move(unlock);
}

void VulkanFisheyeStitcher::setDeviceKeepAlive(std::shared_ptr<void> keepAlive)
{
    d->keepAlive = std::move(keepAlive);
    if (d->ring)
        d->ring->keepAlive = d->keepAlive;
}

void VulkanFisheyeStitcher::trimLensRing()
{
    if (!d->ring)
        return;
    std::lock_guard<std::mutex> lk(d->ring->mx);
    auto& slots = d->ring->slots;
    for (auto it = slots.begin(); it != slots.end();) {
        if (!(*it)->busy) {
            d->ring->destroySlot(**it);
            it = slots.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace render
