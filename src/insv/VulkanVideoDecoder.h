// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "LensDemuxer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// GPU-resident sequential decode of an .insv file's two fisheye HEVC streams
// via FFmpeg's Vulkan hwaccel. Frames never leave VRAM: readNextPair() hands
// back two AVFrame*s carrying AVVkFrame (NV12, one 2-plane image each), which a
// Vulkan compute stitch samples directly.
//
// FFmpeg creates and owns the VkInstance/VkDevice; deviceHandles() exposes them
// so the compute stitch can run on the same device. One device serves every
// decoder in the process: creating one costs ~0.3 s, and on the NVIDIA driver
// it stalls other threads' presents meanwhile, so opening a clip mid-playback
// must not create another. Since decoders on several threads share it, a
// submission to it from outside FFmpeg must hold the queue's lock
// (queueLock()).
//
// Frames are allocated CONCURRENT across FFmpeg's queue families (verified
// via the since-removed tools/vkdecode_spike), so no queue-family ownership
// transfer is needed.
//
// The demux, decoders and seeking are LensDemuxer's, shared with InsvDemux;
// this adds the Vulkan hwaccel set-up and the frame locking.
//
// Part of insv_demux (needs FFmpeg + Vulkan). Not thread-safe.

struct AVBufferRef;
struct AVFrame;

namespace insv {

using VulkanVideoStreamInfo = InsvStreamInfo;

// A new reference to the process-wide Vulkan hw device every decoder (and
// the adopted compute stitch) runs on, or nullptr when it can't be created.
// Exported for the export path: a *_vulkan encoder on the same device can
// consume flat frames with no host round-trip. Caller unrefs.
AVBufferRef* sharedVulkanDevice();

// One decoded fisheye frame's Vulkan handles, pulled from its AVVkFrame under
// the frame lock (see lockFrame). Consumed by render::VulkanFisheyeStitcher.
struct VulkanLensFrame {
    VkImage image = VK_NULL_HANDLE;   // 2-plane: G8_B8R8_2PLANE_420_UNORM (NV12), or
                                      // G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 (P010) for 10-bit
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSemaphore semaphore = VK_NULL_HANDLE; // timeline
    uint64_t semWaitValue = 0;              // wait on this before reading
    int width = 0;
    int height = 0;
    VkImageUsageFlags usage = 0; // the frames pool's (plane copies need TRANSFER_SRC)
};

// What the consumer left the image as, written back into the AVVkFrame by
// unlockFrame so FFmpeg's next use of it syncs correctly.
struct VulkanLensFrameResult {
    uint64_t semSignalValue = 0; // the value the consumer signalled
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

class VulkanVideoDecoder {
public:
    VulkanVideoDecoder() = default;
    ~VulkanVideoDecoder();
    VulkanVideoDecoder(const VulkanVideoDecoder&) = delete;
    VulkanVideoDecoder& operator=(const VulkanVideoDecoder&) = delete;

    // Creates the Vulkan hw device and per-lens decoders. Returns false with
    // `err` set if Vulkan video decode is unavailable (caller falls back).
    bool open(const std::string& path, std::string& err);

    int lensCount() const { return m_demux.lensCount(); }
    const VulkanVideoStreamInfo& lens(int i) const { return m_demux.lens(i); }

    // FFmpeg's Vulkan device, for the compute stitch to adopt. `queueFlags`
    // are the flags FFmpeg created its queues with
    // (AVVulkanDeviceContext::queue_flags, FFmpeg 9+: INTERNALLY_SYNCHRONIZED
    // when the driver has VK_KHR_internally_synchronized_queues, which makes
    // vkGetDeviceQueue return NULL; retrieve with vkGetDeviceQueue2). 0 on
    // older FFmpeg. Valid after open().
    void deviceHandles(VkInstance& inst, VkPhysicalDevice& phys, VkDevice& dev,
                       uint32_t& computeQueueFamily, VkDeviceQueueCreateFlags& queueFlags) const;

    // The VkFormat decoded frames will carry (from the frames context's
    // format[0]), learned by peeking at the first decoded frame of lens 0.
    // UNDEFINED if no frame can be decoded yet. The first frame stays queued
    // for a normal read -- nothing is consumed.
    VkFormat frameFormat();

    // Frame-exact: the next readNextPair returns the frame at `timestampMs`.
    // A target a little ahead, with no keyframe since the last pair read,
    // decodes forward from there instead of from the keyframe again.
    bool seekTo(int64_t timestampMs, std::string& err);
    // See InsvDemux::nearestKeyframeMs.
    int64_t nearestKeyframeMs(int64_t timestampMs) const;
    // See insv::KeyframesAroundMs.
    void keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const;

    // Next lens-0 / lens-1 frame pair in play order (AVFrame* carrying
    // AVVkFrame). The caller owns them -- av_frame_free() both when done, after
    // unlockFrame(). Returns false with `err` empty at EOS, `err` set on error.
    bool readNextPair(AVFrame*& out0, AVFrame*& out1, int64_t* ptsMs, std::string& err);

    // Take the frame lock and read its current Vulkan state into `lf`. Must be
    // paired with unlockFrame(). Call just before command submission.
    void lockFrame(AVFrame* f, VulkanLensFrame& lf);
    // A reference to the Vulkan hw device that keeps it (and its VkDevice)
    // alive past this decoder -- for GPU objects created on the device that
    // may outlive it (lens ring slots still shown by the viewport).
    std::shared_ptr<void> deviceKeepAlive() const;
    // FFmpeg's lock on queue (`family`, `index`) of the shared device -- the
    // one it takes around its own submissions. Hold it around any
    // vkQueueSubmit / vkQueueWaitIdle on that queue. The functions keep the
    // device alive. Valid after open().
    struct QueueLock {
        std::function<void()> lock, unlock;
    };
    QueueLock queueLock(uint32_t family, uint32_t index) const;
    // Write the post-submission state back and release the frame lock.
    void unlockFrame(AVFrame* f, const VulkanLensFrameResult& r);
    // av_frame_free() wrapper so callers need no FFmpeg headers.
    void freeFrame(AVFrame*& f);

private:
    void close();

    LensDemuxer m_demux;
    AVBufferRef* m_hwDevice = nullptr;
};

} // namespace insv
