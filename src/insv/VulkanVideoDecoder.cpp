// SPDX-License-Identifier: MIT OR Apache-2.0
#include "VulkanVideoDecoder.h"

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/version.h>
}

namespace insv {

namespace {

// get_format: take AV_PIX_FMT_VULKAN and build a hw_frames_ctx that keeps the
// NV12 image as a single 2-plane image (default) with SAMPLED usage, so the
// compute stitch can sample it through a ycbcr-conversion sampler.
enum AVPixelFormat pickVulkan(AVCodecContext* ctx, const enum AVPixelFormat* fmts)
{
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p != AV_PIX_FMT_VULKAN)
            continue;
        if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, AV_PIX_FMT_VULKAN,
                                             &ctx->hw_frames_ctx) < 0)
            return AV_PIX_FMT_NONE;
        auto* fc = reinterpret_cast<AVHWFramesContext*>(ctx->hw_frames_ctx->data);
        auto* vfc = reinterpret_cast<AVVulkanFramesContext*>(fc->hwctx);
        vfc->usage = static_cast<VkImageUsageFlagBits>(vfc->usage | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (av_hwframe_ctx_init(ctx->hw_frames_ctx) < 0)
            return AV_PIX_FMT_NONE;
        return AV_PIX_FMT_VULKAN;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

VulkanVideoDecoder::~VulkanVideoDecoder() { close(); }

void VulkanVideoDecoder::close()
{
    m_demux.close(); // the decoders hold their own device references
    if (m_hwDevice)
        av_buffer_unref(&m_hwDevice);
}

// The process-wide Vulkan hw device (see the header). Created on first use
// and kept for the process lifetime; returns a new reference, or nullptr.
AVBufferRef* sharedVulkanDevice()
{
    static std::mutex mx;
    static AVBufferRef* device = nullptr;
    std::lock_guard<std::mutex> lk(mx);
    if (!device && av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) < 0)
        device = nullptr;
    return device ? av_buffer_ref(device) : nullptr;
}

namespace {

// Whether the device has a video decode queue for `codec`. FFmpeg opens the
// Vulkan hwaccel on any Vulkan device and only fails frame by frame ("Device
// does not support the VK_KHR_video_decode_queue extension", a grey viewport:
// llvmpipe, other vendors' or older GPUs / drivers), so check up front and let
// the caller fall back to CPU decode.
bool CanDecode(const AVBufferRef* hwDevice, AVCodecID codec)
{
    VkVideoCodecOperationFlagsKHR op = 0;
    switch (codec) {
    case AV_CODEC_ID_HEVC:
        op = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
        break;
    case AV_CODEC_ID_H264:
        op = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        break;
    case AV_CODEC_ID_AV1:
        op = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
        break;
    default:
        return false;
    }
    const auto* dctx = reinterpret_cast<const AVHWDeviceContext*>(hwDevice->data);
    const auto* vk = static_cast<const AVVulkanDeviceContext*>(dctx->hwctx);
    for (int i = 0; i < vk->nb_qf; ++i)
        if ((vk->qf[i].flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && (vk->qf[i].video_caps & op))
            return true;
    return false;
}

} // namespace

bool VulkanVideoDecoder::open(const std::string& path, std::string& err)
{
    close();
    m_hwDevice = sharedVulkanDevice();
    if (!m_hwDevice) {
        err = "no Vulkan hw device (FFmpeg built without Vulkan, or no driver support)";
        return false;
    }
    AVBufferRef* hw = m_hwDevice;
    const bool ok = m_demux.open(
        path,
        [hw](const AVStream* st, const AVCodec* dec, AVCodecContext* ctx, std::string& e) {
            if (!CanDecode(hw, st->codecpar->codec_id)) {
                e = std::string("the GPU has no Vulkan video decode for ") + dec->name;
                return false;
            }
            ctx->hw_device_ctx = av_buffer_ref(hw);
            ctx->get_format = pickVulkan;
            return true;
        },
        err);
    if (ok && m_demux.lensCount() < 2)
        err = "expected 2 video streams, found " + std::to_string(m_demux.lensCount());
    if (!ok || m_demux.lensCount() < 2) {
        close();
        return false;
    }
    return true;
}

void VulkanVideoDecoder::deviceHandles(VkInstance& inst, VkPhysicalDevice& phys, VkDevice& dev,
                                       uint32_t& computeQueueFamily,
                                       VkDeviceQueueCreateFlags& queueFlags) const
{
    auto* dctx = reinterpret_cast<AVHWDeviceContext*>(m_hwDevice->data);
    auto* vk = reinterpret_cast<AVVulkanDeviceContext*>(dctx->hwctx);
    inst = vk->inst;
    phys = vk->phys_dev;
    dev = vk->act_dev;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 32, 100)
    queueFlags = vk->queue_flags;
#else
    queueFlags = 0; // older FFmpeg creates its queues without flags
#endif
    computeQueueFamily = UINT32_MAX;
    for (int i = 0; i < vk->nb_qf; ++i) {
        if (vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
            computeQueueFamily = vk->qf[i].idx;
            break;
        }
    }
    if (computeQueueFamily == UINT32_MAX)
        for (int i = 0; i < vk->nb_qf; ++i)
            if (vk->qf[i].flags & VK_QUEUE_TRANSFER_BIT) {
                computeQueueFamily = vk->qf[i].idx;
                break;
            }
}

void VulkanVideoDecoder::keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const
{
    m_demux.keyframesAroundMs(timestampMs, before, after);
}

int64_t VulkanVideoDecoder::nearestKeyframeMs(int64_t timestampMs) const
{
    return m_demux.nearestKeyframeMs(timestampMs);
}

bool VulkanVideoDecoder::seekTo(int64_t timestampMs, std::string& err)
{
    return m_demux.seekTo(timestampMs, err);
}

bool VulkanVideoDecoder::readNextPair(AVFrame*& out0, AVFrame*& out1, int64_t* ptsMs,
                                      std::string& err)
{
    return m_demux.readNextPair(out0, out1, ptsMs, err);
}

void VulkanVideoDecoder::lockFrame(AVFrame* f, VulkanLensFrame& lf)
{
    auto* fc = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
    auto* vfc = reinterpret_cast<AVVulkanFramesContext*>(fc->hwctx);
    auto* vkf = reinterpret_cast<AVVkFrame*>(f->data[0]);
    if (vfc->lock_frame)
        vfc->lock_frame(fc, vkf);
    lf.image = vkf->img[0];
    lf.format = vfc->format[0];
    lf.layout = vkf->layout[0];
    lf.semaphore = vkf->sem[0];
    lf.semWaitValue = vkf->sem_value[0];
    lf.width = f->width;
    lf.height = f->height;
    lf.usage = vfc->usage;
}

void VulkanVideoDecoder::unlockFrame(AVFrame* f, const VulkanLensFrameResult& r)
{
    auto* fc = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
    auto* vfc = reinterpret_cast<AVVulkanFramesContext*>(fc->hwctx);
    auto* vkf = reinterpret_cast<AVVkFrame*>(f->data[0]);
    vkf->sem_value[0] = r.semSignalValue;
    vkf->layout[0] = r.newLayout;
    vkf->access[0] = VK_ACCESS_SHADER_READ_BIT;
    if (vfc->unlock_frame)
        vfc->unlock_frame(fc, vkf);
}

void VulkanVideoDecoder::freeFrame(AVFrame*& f) { av_frame_free(&f); }

VulkanVideoDecoder::QueueLock VulkanVideoDecoder::queueLock(uint32_t family, uint32_t index) const
{
    std::shared_ptr<void> keep = deviceKeepAlive();
    auto* dctx = reinterpret_cast<AVHWDeviceContext*>(m_hwDevice->data);
    auto* vk = reinterpret_cast<AVVulkanDeviceContext*>(dctx->hwctx);
    // Deprecated for VK_KHR_internally_synchronized_queues, which this
    // FFmpeg / driver pair doesn't offer; FFmpeg itself still locks with it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    auto lockFn = vk->lock_queue;
    auto unlockFn = vk->unlock_queue;
#pragma GCC diagnostic pop
    return {[keep, dctx, lockFn, family, index] { lockFn(dctx, family, index); },
            [keep, dctx, unlockFn, family, index] { unlockFn(dctx, family, index); }};
}

std::shared_ptr<void> VulkanVideoDecoder::deviceKeepAlive() const
{
    if (!m_hwDevice)
        return nullptr;
    AVBufferRef* ref = av_buffer_ref(m_hwDevice);
    return std::shared_ptr<void>(ref, [](void* p) {
        auto* r = static_cast<AVBufferRef*>(p);
        av_buffer_unref(&r);
    });
}

} // namespace insv
