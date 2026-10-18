#pragma once

#include <FisheyeProjection.h> // insv::StitchGpuParams
#include <SeamDepth.h>         // insv::SeamCosts

#include "LensFrame.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Vulkan compute port of insv::FisheyeStitcher::renderEquirect.
//
// Two ways to use it:
//   * init()      -- creates its own compute-only VkInstance/VkDevice; stitch()
//                    takes CPU-decoded RGB fisheye frames.
//   * initAdopt() -- adopts an existing VkDevice (FFmpeg's Vulkan video
//                    decoder); stitchVk() takes decoder-owned NV12 images and
//                    never touches host memory for the inputs (stage 2).
//
// Not thread-safe -- drive it from one thread (the NativeStitchSource decoder
// thread).

namespace render {

// Handles for one decoder-owned (FFmpeg Vulkan) fisheye frame, extracted under
// the frame lock. Mirrors insv::VulkanLensFrame (kept separate so the two libs'
// headers stay independent).
struct VulkanLensInput {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSemaphore semaphore = VK_NULL_HANDLE; // timeline
    uint64_t semWaitValue = 0;
    int width = 0;
    int height = 0;
    VkImageUsageFlags usage = 0; // pool image usage; fillLensFrameVk needs TRANSFER_SRC
};

// What stitchVk() left the image as -- write back into the AVVkFrame.
struct VulkanLensOutcome {
    uint64_t semSignalValue = 0;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Where the time went in the last stitchVk() / stitchAndReframeVk() call
// (tools/playback_bench). GPU times come from timestamp queries and are < 0
// when the queue can't write timestamps.
struct StitchTiming {
    double stitchGpuMs = -1.0;  // measure + stitch passes
    double reframeGpuMs = -1.0; // reproject pass (stitchAndReframeVk only)
    double fenceWaitMs = 0.0;   // submit -> fence signalled (decode wait + GPU work)
    double copyMs = 0.0;        // memcpy out of the host-visible output buffer
};

class VulkanFisheyeStitcher {
public:
    VulkanFisheyeStitcher();
    ~VulkanFisheyeStitcher();

    VulkanFisheyeStitcher(const VulkanFisheyeStitcher&) = delete;
    VulkanFisheyeStitcher& operator=(const VulkanFisheyeStitcher&) = delete;

    // Own compute-only device; use with stitch(). False (err set) if no usable
    // Vulkan device is present.
    bool init(std::string& err);

    // Adopt FFmpeg's Vulkan device; builds the NV12/ycbcr pipeline; use with
    // stitchVk(). `queueFlags` is AVVulkanDeviceContext::queue_flags -- the
    // flags FFmpeg created its queues with (PROTECTED on some FFmpeg/driver
    // pairs; those queues need vkGetDeviceQueue2, vkGetDeviceQueue returns
    // NULL for them). `frameFormat` is the VkFormat the decoder's frames will
    // carry (e.g. VulkanVideoDecoder::frameFormat()); UNDEFINED means NV12.
    // False (err set) if the device can't do ycbcr conversion or the NV12
    // shader is missing -- caller should fall back to init()+stitch().
    bool initAdopt(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                   uint32_t computeQueueFamily, VkDeviceQueueCreateFlags queueFlags,
                   VkFormat frameFormat, std::string& err);

    bool ready() const { return m_ready; }

    // Whether stitchAndReframeVk() is usable (the reproject compute pipeline
    // built, reframe_reproject.comp). Independent of ready().
    bool reframeReady() const { return m_reframeReady; }

    // stage 1: reproject an RGB24 fisheye pair (params.fisheyeW x fisheyeH each)
    // into an eqW x eqH equirect (outRgba resized to eqW*eqH*4, opaque alpha).
    bool stitch(const uint8_t* rgb0, const uint8_t* rgb1, const insv::StitchGpuParams& params,
                int eqW, int eqH, std::vector<uint8_t>& outRgba, std::string& err);

    // stage 2: reproject a decoder-owned NV12 fisheye pair straight from VRAM.
    // Waits on each frame's timeline semaphore at semWaitValue and signals it
    // forward; *r0 / *r1 get the signalled value + left layout for the caller
    // to write back into the AVVkFrame. Blocking.
    bool stitchVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                  const insv::StitchGpuParams& params, int eqW, int eqH,
                  std::vector<uint8_t>& outRgba, VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                  std::string& err);

    // stage 2 + export: stitch the NV12 fisheye pair to an eqW x eqH equirect
    // (kept GPU-resident), then reproject it to an outW x outH rectilinear
    // frame against `pose` ({yaw,pitch,roll,fovY} radians; nullptr => forward
    // view), emitting tightly-packed BT.709 full-range NV12 into `outNv12`
    // (Y plane outW*outH, then interleaved UV outW*outH/2). outW must be a
    // multiple of 4 and outH a multiple of 2. No CPU reproject / swscale.
    // Requires reframeReady(). Same semaphore handshake as stitchVk().
    // `stab` (row-major 3x3, nullptr => identity) is the frame's gyro
    // de-rotation in the sphere frame, applied after `pose`. `colorGrade`
    // ({exposureEv, contrast%, saturation%, temperature%}, nullptr => neutral)
    // is the Colour panel grade, applied to the equirect sample before it's
    // reprojected.
    bool stitchAndReframeVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                            const insv::StitchGpuParams& params, int eqW, int eqH, int outW, int outH,
                            const float pose[4], const float stab[9],
                            const float colorGrade[4], std::vector<uint8_t>& outNv12,
                            VulkanLensOutcome* r0, VulkanLensOutcome* r1, std::string& err);

    // Depth-aware seam: measure the seam cost volume of a decoder-owned NV12
    // pair (pass 2 of the stitch shaders -- per azimuth bin and candidate
    // distance, the NCC of the two lenses) into `out`, in its own submission,
    // so the table solved from it can go into this same frame's stitch. Same
    // semaphore handshake as stitchVk(): pass r0 / r1's value and layout on as
    // the next call's semWaitValue / layout. Blocking.
    bool measureSeamVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                       const insv::StitchGpuParams& params, insv::SeamCosts& out,
                       VulkanLensOutcome* r0, VulkanLensOutcome* r1, std::string& err);

    // Whether renderViewVk() is usable (the fused view pipeline built -- needs
    // initAdopt(); fisheye_view_nv12.comp).
    bool viewReady() const { return m_viewReady; }

    // NV12 and P010 are both tightly packed BT.709 limited 4:2:0; P010 keeps
    // 16 bits per sample with the 10-bit code in the high bits.
    enum class ViewFormat { Nv12, Rgba, P010 };

    // A caller-owned NV12/P010 image the fused view fills on-device instead of
    // reading back to the host: the fields of FFmpeg's AVVkFrame. `fmt` is the
    // pixel format the fused pass writes. `nbImages` is 1 for a multi-plane
    // image (the usual case, the two planes addressed by aspect) or 2 for
    // separate Y / CbCr images. `layout` and `access` are the image's current
    // state; both are updated. `sem` / `semValue` are its timeline semaphore
    // and value: the submission waits on `sem` at `semValue`, signals
    // `semValue` + 1, and `semValue` is bumped. Same-device only -- the work
    // runs on the adopted device.
    struct FlatImageTarget {
        ViewFormat fmt = ViewFormat::Nv12;
        int nbImages = 0;
        VkImage img[2] = {};
        VkImageLayout layout[2] = {};
        VkAccessFlagBits2 access[2] = {};
        VkSemaphore sem[2] = {};
        uint64_t semValue[2] = {};
    };

    // Fused render: the outW x outH rectilinear view straight from the
    // NV12 fisheye pair -- no equirect. Each output pixel's view ray (`pose`,
    // then `stab`, as stitchAndReframeVk) samples the lenses directly, so it
    // is one resample instead of two and costs output pixels, not sphere
    // pixels. The seam's photometric gains are measured over the same grid as
    // a measureW x measureH equirect stitch would use, so they match that
    // path. `fmt` Nv12 = tightly-packed BT.709 limited NV12 (outW*outH*3/2
    // bytes); Rgba = RGBA8 (outW*outH*4). outW must be a multiple of 4, outH
    // of 2. Same semaphore handshake as stitchVk(). Requires viewReady().
    bool renderViewVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                      const insv::StitchGpuParams& params, int measureW, int measureH, int outW,
                      int outH, const float pose[4], const float stab[9],
                      const float colorGrade[4], ViewFormat fmt, std::vector<uint8_t>& out,
                      VulkanLensOutcome* r0, VulkanLensOutcome* r1, std::string& err);

    // As renderViewVk (one view), but the packed NV12/P010 goes into `dst`'s
    // image(s) on-device -- no host read-back. For feeding a *_vulkan
    // encoder's frame pool directly. `dst.fmt` picks the packing (the
    // encoder frame pool's software format: NV12 or P010LE). Requires
    // viewReady().
    bool renderViewVkToImage(const VulkanLensInput& f0, const VulkanLensInput& f1,
                             const insv::StitchGpuParams& params, int measureW,
                             int measureH, int outW, int outH, const float pose[4],
                             const float stab[9], const float colorGrade[4],
                             FlatImageTarget& dst, VulkanLensOutcome* r0,
                             VulkanLensOutcome* r1, std::string& err);

    // Several RGBA views of one pair in one submission, into one
    // canvasW x canvasH RGBA8 buffer (rows canvasW pixels long), cleared to
    // `clearRgba` (R in the low byte) first. Each view: its pose as above, its
    // rectangle in the canvas (x and w multiples of 4, h of 2); the aspect is
    // the rectangle's. Same stab for all, no colour grade. For the privacy
    // blur's plate scan: the detector's tiles and the quad tracker's views.
    struct ViewRect {
        float pose[4] = {0.0f, 0.0f, 0.0f, 1.309f};
        int x = 0, y = 0, w = 0, h = 0;
    };
    bool renderViewsVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                       const insv::StitchGpuParams& params, int measureW, int measureH,
                       int canvasW, int canvasH, const std::vector<ViewRect>& views,
                       const float stab[9], uint32_t clearRgba, std::vector<uint8_t>& out,
                       VulkanLensOutcome* r0, VulkanLensOutcome* r1, std::string& err);

    // Whether fillLensFrameVk() is usable -- the fused view pipeline (its
    // measure pass) plus exportable memory on this device.
    bool lensExportReady() const;

    // The adopted device's lock on the compute queue this stitcher submits to
    // (insv::VulkanVideoDecoder::queueLock): the device is shared by other
    // threads' decoders. Set it right after initAdopt().
    void setQueueLock(std::function<void()> lock, std::function<void()> unlock);

    // Keeps the adopted device alive for as long as any LensFrame handed out
    // still references a ring slot (slots are freed on this device when the
    // last reference goes, which can be after this stitcher is gone). Set it
    // before the first fillLensFrameVk(): the decoder's hw device reference.
    void setDeviceKeepAlive(std::shared_ptr<void> keepAlive);

    // Hand the decoded NV12 pair to another Vulkan device. Copies both lenses'
    // Y / CbCr planes into a free slot of this stitcher's exportable ring
    // (render::LensFrame), measures the seam's photometric gains over a
    // measureW x measureH equirect grid (as the stitch does), and fills `out`
    // with the slot, this frame's packed stitch params and the gains. Blocks
    // until the GPU is done (and, if every slot is held, until one frees up --
    // the ring grows a little rather than deadlock). Same semaphore handshake
    // as stitchVk(). Requires lensExportReady() and a decoder image with
    // TRANSFER_SRC usage.
    bool fillLensFrameVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                         const insv::StitchGpuParams& params, int measureW, int measureH,
                         LensFrame& out, VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                         std::string& err);

    // Free the ring slots nobody holds (a source going idle).
    void trimLensRing();

    const StitchTiming& lastTiming() const;

private:
    bool bringUp(bool nv12Mode, VkFormat frameFormat, std::string& err);
    bool buildReframePipeline(std::string& err);
    bool buildViewPipeline(std::string& err);
    // renderViewVk / renderViewsVk: `views` into one buffer (canvas 0 x 0 =
    // the single view, NV12 or RGBA as `fmt`); `clear` fills it first.
    bool renderViewsImpl(const VulkanLensInput& f0, const VulkanLensInput& f1,
                         const insv::StitchGpuParams& params, int measureW, int measureH,
                         int canvasW, int canvasH, const std::vector<ViewRect>& views,
                         const float stab[9], const float colorGrade[4], ViewFormat fmt,
                         const uint32_t* clear, std::vector<uint8_t>& out,
                         FlatImageTarget* img, VulkanLensOutcome* r0,
                         VulkanLensOutcome* r1, std::string& err);

    struct Impl;
    Impl* d = nullptr;
    bool m_ready = false;
    bool m_reframeReady = false;
    bool m_viewReady = false;
};

} // namespace render
