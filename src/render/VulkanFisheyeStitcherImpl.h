#pragma once

// VulkanFisheyeStitcher's insides, shared by its two source files: the
// per-frame passes (VulkanFisheyeStitcher.cpp) and the device, pipelines and
// resources they run on (VulkanFisheyeStitcherSetup.cpp). Private: only
// those two include it.

#include "VulkanFisheyeStitcher.h"
#include "VulkanLut.h"

#include <SeamDepth.h> // insv::SeamCosts, the cost pass's candidates / patch

#include <vulkan/vulkan.h>

#include <unistd.h> // close() -- exported memory FDs

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace render {

// std140 layout matching fisheye_stitch.comp's `Params` uniform block.
struct ShaderParams {
    float w2l0[3][4];
    float w2l1[3][4];
    float outRot[3][4];
    float cxcy[4];
    float seam[4];
    float edge[4];  // edgeStartFrac, edgeEndFrac, photometric, gammaMode
    int32_t dims[4];
    float meiA[4];  // xi0, xi1, fx0, fx1
    float meiB[4];  // fy0, fy1, -, accessory
    float kd0[4];   // lens 0 radial k1..k4
    float kd1[4];   // lens 1 radial k1..k4
    float rs[4];    // capture timing: active, useColumns, -, -
    float rsRot[2 * insv::StitchGpuParams::kRsTaps][4]; // per lens, rotation vectors (stitch world frame), -
    float pd0[4];   // lens 0 tangential p1, p2, thin-prism s1, s2
    float pd1[4];   // lens 1 likewise
    float sd[4];    // thin-prism s3, s4 of lens 0, then of lens 1
    float accA[4];  // accessory poly c0..c3
    float accB[4];  // accessory poly c4, c5, bare poly c0, c1
    float accC[4];  // bare poly c2..c5
    float seamA[4]; // depth-aware seam: axis xyz, active
    float seamU[4]; // seam-plane u xyz, half baseline (m)
    float seamV[4]; // seam-plane v xyz, ramp past the band (rad)
    float seamC[4]; // cost pass: q min, q max (1/m), patch elevation half-range (rad), -
    float seamQ[insv::kSeamDepthBins]; // vec4[kSeamDepthBins / 4]: per-bin inverse distance
    float blur0[4]; // privacy blur: count, -, -, -
    float blurQ[insv::kMaxBlurQuads * 6][4]; // per quad, see fisheye_stitch_common.glsl
};
static_assert(sizeof(ShaderParams) == 7344, "ShaderParams must match the std140 UBO");
static_assert(sizeof(ShaderParams) == LensFrame::kParamsBytes, "LensFrame::params carries ShaderParams");

// Push constants for reframe_reproject.comp. 4 ints + 5 floats, then a mat3
// (16-byte aligned in std430 -> 12 bytes pad) for the stabilization matrix.
struct ReframePush {
    int32_t eqW;
    int32_t eqH;
    int32_t outW;
    int32_t outH;
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    float _pad[3];
    float stab[12];      // 3 columns, each a vec3 padded to 16 bytes (col-major)
    float colorGrade[4]; // exposureEv, contrast%, saturation%, temperature%
    float colorLut[4];   // VulkanLutStore::pushValues
};
static_assert(sizeof(ReframePush) == 128, "ReframePush must match the compute push_constant block");

// Push constants for fisheye_view_nv12.comp: 4 ints + 5 floats, pad, then the
// stab mat3 as padded columns and the grade -- the same tail as ReframePush.
struct ViewPush {
    int32_t pass;
    int32_t outW;
    int32_t outH;
    int32_t fmt; // 0 = NV12, 1 = RGBA
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    int32_t ox;     // RGBA: the view's origin in the canvas (ox % 4 == 0) ...
    int32_t oy;
    int32_t stride; // ... and the canvas row length; 0 = outW
    float stab[12];
    float colorGrade[4];
    float colorLut[4];
};
static_assert(sizeof(ViewPush) == 128, "ViewPush must match the compute push_constant block");

// The exportable lens ring (render::LensFrame). Shared-owned: the stitcher
// holds it, and so does every LensSlot handle given out (its deleter marks the
// slot free), so slots -- and the device they live on, via keepAlive -- outlast
// the stitcher while a viewport still shows one.
struct LensRingCore {
    struct Slot {
        LensSlot pub;
        VkImage img[LensSlot::kImages] = {};
        VkDeviceMemory mem[LensSlot::kImages] = {};
        bool busy = false;
        uint64_t fill = 0;
    };

    VkDevice device = VK_NULL_HANDLE;
    std::shared_ptr<void> keepAlive; // the adopted device's owner
    std::mutex mx;
    std::condition_variable cv;
    std::vector<std::unique_ptr<Slot>> slots; // guarded by mx

    static std::atomic<uint64_t> nextId;

    ~LensRingCore()
    {
        for (auto& sl : slots)
            destroySlot(*sl);
    }

    void destroySlot(Slot& sl)
    {
        for (int i = 0; i < LensSlot::kImages; ++i) {
            if (sl.pub.fd[i] >= 0)
                ::close(sl.pub.fd[i]);
            if (sl.img[i])
                vkDestroyImage(device, sl.img[i], nullptr);
            if (sl.mem[i])
                vkFreeMemory(device, sl.mem[i], nullptr);
            sl.pub.fd[i] = -1;
            sl.img[i] = VK_NULL_HANDLE;
            sl.mem[i] = VK_NULL_HANDLE;
        }
    }
};

// Ring sizing: enough for the playback queue plus what a viewport holds (the
// shown frame and its frames in flight). Past kLensSlotsSoft a fill waits for
// a slot to come back; past kLensSlotsHard it fails instead of growing.
constexpr size_t kLensSlotsSoft = 10;
constexpr size_t kLensSlotsHard = 16;
constexpr auto kLensSlotWait = std::chrono::milliseconds(250);

struct VulkanFisheyeStitcher::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // Timestamp queries: 0 before the stitch, 1 after it, 2 after the reproject.
    VkQueryPool tsPool = VK_NULL_HANDLE; // null if the queue has no timestamps
    double tsPeriodNs = 0.0;
    StitchTiming timing;

    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion ycbcr = VK_NULL_HANDLE; // nv12 mode only
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // The grade's LUTs (set 1 of the reproject and view pipelines).
    VulkanLutStore luts;

    bool adopted = false; // device is FFmpeg's -- don't destroy it
    // Adopted: FFmpeg's lock on `queue` -- the device is shared by every
    // decoder in the process, on other threads (setQueueLock).
    std::function<void()> lockQueue, unlockQueue;

    VkResult submit(const VkSubmitInfo& si)
    {
        if (lockQueue)
            lockQueue();
        const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
        if (unlockQueue)
            unlockQueue();
        return r;
    }
    // Before freeing resources our submissions use. Each submission is waited
    // on (its fence) before the call that made it returns, so on the shared,
    // adopted device there is nothing to wait for -- and a vkDeviceWaitIdle
    // there would race other threads' queue use.
    void idleBeforeFree()
    {
        if (!adopted)
            vkDeviceWaitIdle(device);
    }
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr; // exportable ring
    uint8_t deviceUuid[16] = {};
    std::shared_ptr<void> keepAlive;
    std::shared_ptr<LensRingCore> ring;
    bool nv12 = false;    // stitchVk path: sample decoder NV12 images via ycbcr
    // The decoder's 2-plane format, the ycbcr conversion's: NV12, or P010
    // for 10-bit (initAdopt). Every image it hands over must have it.
    VkFormat lensFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    bool deepLens() const { return lensFormat != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; }
    // Cached ycbcr image views over the decoder's pooled images (nv12 mode).
    std::map<VkImage, VkImageView> nv12Views;

    // Dimension-dependent resources.
    int fw = 0, fh = 0, ew = 0, eh = 0;
    VkImage fisheye[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory fisheyeMem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView fisheyeView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer staging = VK_NULL_HANDLE, ubo = VK_NULL_HANDLE, outBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE, uboMem = VK_NULL_HANDLE, outMem = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
    void* uboMap = nullptr;
    void* outMap = nullptr;
    VkBuffer gainBuf = VK_NULL_HANDLE;      // 3 uints: photometric sum0, sum1, count
    VkDeviceMemory gainMem = VK_NULL_HANDLE;
    void* gainMap = nullptr;
    // Depth-aware seam cost volume (binding 5): (NCC, texture) per bin x candidate.
    static constexpr VkDeviceSize kSeamBufBytes =
        sizeof(float) * 2 * insv::kSeamDepthBins * insv::kSeamDepthCands;
    VkBuffer seamBuf = VK_NULL_HANDLE;
    VkDeviceMemory seamMem = VK_NULL_HANDLE;
    void* seamMap = nullptr;

    // Reproject pass (stitchAndReframeVk): equirect OutBuf -> rectilinear NV12.
    VkDescriptorSetLayout rfSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool rfDescPool = VK_NULL_HANDLE;
    VkDescriptorSet rfSet = VK_NULL_HANDLE;
    VkPipelineLayout rfPipeLayout = VK_NULL_HANDLE;
    VkPipeline rfPipeline = VK_NULL_HANDLE;
    VkBuffer flatBuf = VK_NULL_HANDLE; // host-visible flat output (NV12 or RGBA)
    VkDeviceMemory flatMem = VK_NULL_HANDLE;
    void* flatMap = nullptr;
    VkDeviceSize flatBytes = 0; // size backing flatBuf

    // Fused fisheye -> view pass (renderViewVk): the stitch's set layout, its
    // own set (binding 3 = flatBuf) and push constants.
    VkDescriptorSet vwSet = VK_NULL_HANDLE;
    VkPipelineLayout vwPipeLayout = VK_NULL_HANDLE;
    VkPipeline vwPipeline = VK_NULL_HANDLE;

    // (Re)allocate the host-visible flat output buffer to hold `bytes`. Grows
    // only, so switching NV12 <-> RGBA or sizes doesn't thrash.
    bool ensureFlat(VkDeviceSize bytes, std::string& err);

    bool findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out) const
    {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want) {
                out = i;
                return true;
            }
        }
        return false;
    }

    // A buffer the CPU reads back every frame: host-cached if the device has
    // it. Plain HOST_VISIBLE|COHERENT system memory is write-combined, and CPU
    // reads from it run at ~0.3 GB/s (tools/playback_bench: 100 ms to copy a
    // 3840x1920 RGBA frame, vs 1 ms for the stitch itself).
    bool makeReadbackBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                            VkDeviceMemory& mem, void** map);

    bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mprops,
                    VkBuffer& buf, VkDeviceMemory& mem, void** map);

    void destroyResources();

    void tsReset()
    {
        if (tsPool)
            vkCmdResetQueryPool(cmd, tsPool, 0, 3);
    }
    void tsWrite(VkPipelineStageFlagBits stage, uint32_t query)
    {
        if (tsPool)
            vkCmdWriteTimestamp(cmd, stage, tsPool, query);
    }
    // After the fence: fill timing.stitchGpuMs (and reframeGpuMs if `reframe`).
    void tsRead(bool reframe)
    {
        timing.stitchGpuMs = timing.reframeGpuMs = -1.0;
        if (!tsPool)
            return;
        uint64_t t[3] = {};
        const uint32_t n = reframe ? 3 : 2;
        if (vkGetQueryPoolResults(device, tsPool, 0, n, sizeof t, t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        timing.stitchGpuMs = (t[1] - t[0]) * tsPeriodNs * 1e-6;
        if (reframe)
            timing.reframeGpuMs = (t[2] - t[1]) * tsPeriodNs * 1e-6;
    }

    // fill + measure pass + stitch pass, into the already-begun `cmd`.
    void recordStitchPasses(int eqW, int eqH);

    // nv12 mode: a cached ycbcr-conversion view over one of the decoder's
    // pooled 2-plane images.
    VkImageView nv12ViewFor(VkImage img, VkFormat fmt);

    // UBO + gain buffer + seam cost buffer, created once, and bound (0, 4
    // and 5) in both the stitch set and the view set.
    bool ensureUbo(std::string& err);

    // The equirect output SSBO (stitch set binding 3), plus ensureUbo(). Both
    // modes.
    bool ensureOutput(int new_, int neh, std::string& err);

    // One exportable image with its own dedicated memory, and the memory's FD.
    bool makeExportImage(VkFormat fmt, int w, int h, VkImage& img, VkDeviceMemory& mem,
                         uint64_t& size, int& fd, std::string& err);

    // A free ring slot for lensW x lensH lenses, marked busy -- reusing one,
    // allocating a new one, or waiting for one to come back.
    LensRingCore::Slot* acquireLensSlot(int lensW, int lensH, std::string& err);

    bool ensureResources(int nfw, int nfh, int new_, int neh, std::string& err);
};

} // namespace render
