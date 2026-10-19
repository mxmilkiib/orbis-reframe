#include "ReframeFlatten.h"

#include "ReframeRenderer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <dlfcn.h>

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace exportpipe {

namespace {

std::string avErr(const std::string& what, int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return what + ": " + buf;
}

struct Encoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVStream* stream = nullptr;
    SwsContext* toNv12 = nullptr;
    AVFrame* nv12 = nullptr;
    // Main 10 output (FlattenConfig::ten_bit): the encoder's surfaces are
    // P010, so CPU-side frames get widened by `toP010` into `p010` before
    // the send / upload.
    bool tenBit = false;
    SwsContext* toP010 = nullptr;
    AVFrame* p010 = nullptr;
    AVPacket* pkt = nullptr;
    // Hardware-surface encoders (Vulkan video, VAAPI): nv12 is uploaded into
    // `hw` before it's sent to the encoder. All nullptr when the encoder
    // takes CPU frames as they are (NVENC).
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrames = nullptr;
    AVFrame* hw = nullptr;
    // True when hwDevice is FlattenConfig::vulkan_device -- frames the
    // caller's GPU pipeline fills on that device can go straight to the
    // encoder (acquireVulkanFrame / addVulkanFrame).
    bool sharedDevice = false;
    // Optional AAC audio stream (FlattenConfig::audio_sample_rate > 0).
    AVCodecContext* actx = nullptr;
    AVStream* astream = nullptr;
    AVFrame* aframe = nullptr;
};

void closeEncoder(Encoder& e)
{
    if (e.aframe)
        av_frame_free(&e.aframe);
    if (e.actx)
        avcodec_free_context(&e.actx);
    if (e.hw)
        av_frame_free(&e.hw);
    if (e.hwFrames)
        av_buffer_unref(&e.hwFrames);
    if (e.hwDevice)
        av_buffer_unref(&e.hwDevice);
    if (e.toNv12) {
        sws_freeContext(e.toNv12);
        e.toNv12 = nullptr;
    }
    if (e.toP010) {
        sws_freeContext(e.toP010);
        e.toP010 = nullptr;
    }
    if (e.p010)
        av_frame_free(&e.p010);
    if (e.nv12)
        av_frame_free(&e.nv12);
    if (e.pkt)
        av_packet_free(&e.pkt);
    if (e.ctx)
        avcodec_free_context(&e.ctx);
    if (e.fmt) {
        if (!(e.fmt->oformat->flags & AVFMT_NOFILE) && e.fmt->pb)
            avio_closep(&e.fmt->pb);
        avformat_free_context(e.fmt);
        e.fmt = nullptr;
    }
}

// FFmpeg ships the *_nvenc encoders whether or not the NVIDIA driver is
// installed; the driver library is dlopen'ed when the encoder is opened, so
// look for it first: its absence is what the "install libnvidia-encode1"
// hint is for.
bool nvencLibraryPresent()
{
    void* h = dlopen("libnvidia-encode.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (h)
        dlclose(h);
    return h != nullptr;
}

// The backends openEncoder() tries, in order: NVENC, then VAAPI (the
// on-by-default route in Mesa; Vulkan video encode is still opt-in on RADV
// and off on recent Intel GPUs), then Vulkan video. ORBIS_ENCODER
// ("nvenc" / "vaapi" / "vulkan") restricts the choice to one backend; any
// other value is reported once and ignored.
struct Backends {
    const char* names[2][3] = {};  // [h265][i]
    int count = 0;
    const char* only = nullptr;    // ORBIS_ENCODER as accepted, else nullptr
};

const Backends& backends()
{
    static const Backends b = [] {
        static const char* const all[2][3] = {{"h264_nvenc", "h264_vaapi", "h264_vulkan"},
                                              {"hevc_nvenc", "hevc_vaapi", "hevc_vulkan"}};
        static const char* const kinds[3] = {"nvenc", "vaapi", "vulkan"};
        Backends out;
        const char* env = std::getenv("ORBIS_ENCODER");
        for (int k = 0; k < 3; ++k)
            if (env && !std::strcmp(env, kinds[k]))
                out.only = kinds[k];
        if (env && *env && !out.only)
            std::fprintf(stderr, "ORBIS_ENCODER=%s isn't nvenc, vaapi or vulkan; trying all\n",
                         env);
        for (int k = 0; k < 3; ++k) {
            if (out.only && std::strcmp(out.only, kinds[k]))
                continue;
            out.names[0][out.count] = all[0][k];
            out.names[1][out.count] = all[1][k];
            ++out.count;
        }
        return out;
    }();
    return b;
}

AVHWDeviceType deviceFor(const char* encName)
{
    if (std::strstr(encName, "_vulkan"))
        return AV_HWDEVICE_TYPE_VULKAN;
    if (std::strstr(encName, "_vaapi"))
        return AV_HWDEVICE_TYPE_VAAPI;
    return AV_HWDEVICE_TYPE_NONE;
}

// Whether `hwDevice` has a video encode queue for `h265 ? HEVC : H264` --
// FlattenConfig::vulkan_device comes from the decode path, which only needs
// a decode queue, so a device could decode yet not encode.
bool vulkanCanEncode(const AVBufferRef* hwDevice, bool h265)
{
    const VkVideoCodecOperationFlagsKHR op = h265
        ? static_cast<VkVideoCodecOperationFlagsKHR>(VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR)
        : static_cast<VkVideoCodecOperationFlagsKHR>(VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR);
    const auto* dctx = reinterpret_cast<const AVHWDeviceContext*>(hwDevice->data);
    const auto* vk = static_cast<const AVVulkanDeviceContext*>(dctx->hwctx);
    for (int i = 0; i < vk->nb_qf; ++i)
        if ((vk->qf[i].flags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) && (vk->qf[i].video_caps & op))
            return true;
    return false;
}

// The Vulkan device's vendor: NVIDIA cards should keep steering at NVENC --
// FFmpeg's Vulkan encoders are slower and lower quality there, and silently
// picking them would hide the "install libnvidia-encode1" hint.
bool vulkanDeviceIsNvidia(AVBufferRef* device)
{
    auto* dctx = reinterpret_cast<AVHWDeviceContext*>(device->data);
    auto* vk = reinterpret_cast<AVVulkanDeviceContext*>(dctx->hwctx);
    if (!vk->get_proc_addr)
        return false;
    auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        vk->get_proc_addr(vk->inst, "vkGetPhysicalDeviceProperties"));
    if (!getProps)
        return false;
    VkPhysicalDeviceProperties props{};
    getProps(vk->phys_dev, &props);
    return props.vendorID == 0x10de;
}

// Whether `encName` can run on this machine: the encoder exists in this
// FFmpeg, its driver is there (NVENC's library; a device for the
// hardware-surface encoders), and a throwaway avcodec_open2 at 1080p
// succeeds. Finding the library or creating the device alone doesn't prove
// the GPU can encode (an NVIDIA driver / library mismatch, Intel ANV and
// RADV without Vulkan encode, the decode-only nvidia-vaapi-driver): the
// open is what makes the probe match what export will actually pick. On
// success a hardware-surface encoder's device is handed back in
// `deviceOut` (caller unrefs) for the export to run on; on failure `why`
// says what was missing.
bool encoderUsable(const char* encName, AVBufferRef** deviceOut, std::string& why)
{
    const AVCodec* enc = avcodec_find_encoder_by_name(encName);
    if (!enc) {
        why = "not in this FFmpeg build";
        return false;
    }
    const AVHWDeviceType dev = deviceFor(encName);
    AVBufferRef* hw = nullptr;
    if (dev == AV_HWDEVICE_TYPE_NONE) {
        if (!nvencLibraryPresent()) {
            why = "libnvidia-encode.so.1 not found";
            return false;
        }
    } else {
        const int ret = av_hwdevice_ctx_create(&hw, dev, nullptr, nullptr, 0);
        if (ret < 0) {
            why = avErr(dev == AV_HWDEVICE_TYPE_VULKAN ? "no Vulkan device" : "no VAAPI device",
                        ret);
            return false;
        }
        if (dev == AV_HWDEVICE_TYPE_VULKAN && vulkanDeviceIsNvidia(hw)) {
            av_buffer_unref(&hw);
            why = "skipped on NVIDIA (NVENC is used there)";
            return false;
        }
    }
    // Throwaway open at export-like settings. NVENC takes NV12 as it is; the
    // hardware-surface encoders need a frames context at open (Vulkan video
    // outright, VAAPI to bind the surfaces).
    AVCodecContext* ctx = avcodec_alloc_context3(enc);
    AVBufferRef* frames = nullptr;
    int ret = AVERROR(ENOMEM);
    if (ctx) {
        ctx->width = 1920;
        ctx->height = 1080;
        ctx->time_base = {1, 30};
        ctx->framerate = {30, 1};
        if (!hw) {
            ctx->pix_fmt = AV_PIX_FMT_NV12;
        } else {
            ctx->pix_fmt = dev == AV_HWDEVICE_TYPE_VULKAN ? AV_PIX_FMT_VULKAN : AV_PIX_FMT_VAAPI;
            ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
            ctx->hw_device_ctx = av_buffer_ref(hw);
            frames = av_hwframe_ctx_alloc(hw);
            if (frames) {
                auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
                fc->format = ctx->pix_fmt;
                fc->sw_format = AV_PIX_FMT_NV12;
                fc->width = ctx->width;
                fc->height = ctx->height;
                ret = av_hwframe_ctx_init(frames);
                if (ret == 0)
                    ctx->hw_frames_ctx = av_buffer_ref(frames);
            }
        }
        if (!hw || ctx->hw_frames_ctx)
            ret = avcodec_open2(ctx, enc, nullptr);
    }
    avcodec_free_context(&ctx);
    av_buffer_unref(&frames);
    if (ret < 0) {
        av_buffer_unref(&hw);
        why = avErr("open failed", ret);
        return false;
    }
    if (deviceOut)
        *deviceOut = hw;
    else
        av_buffer_unref(&hw);
    return true;
}

// One run's probe, per codec: the backends tried in order up to the first
// usable one, why each before it isn't, and the picked one's device, kept
// so the export runs on it instead of creating another (and on AMD / Intel
// opening a second throwaway encoder). Backends after the picked one are
// only probed if its real open fails (openEncoder). The device lives as
// long as the process.
struct Probe {
    const char* picked = nullptr;
    AVBufferRef* device = nullptr;  // the picked hardware-surface encoder's
    std::string report;             // "hevc_nvenc: …; hevc_vaapi: …"
};

const Probe& probe(bool h265)
{
    static Probe probes[2];
    static std::once_flag once;
    std::call_once(once, [] {
        const Backends& b = backends();
        for (int codec = 0; codec < 2; ++codec) {
            Probe& pr = probes[codec];
            for (int i = 0; i < b.count && !pr.picked; ++i) {
                const char* name = b.names[codec][i];
                std::string why;
                if (encoderUsable(name, &pr.device, why))
                    pr.picked = name;
                else
                    pr.report += (pr.report.empty() ? "" : "; ") + std::string(name) + ": " + why;
            }
            core::log::Info("export encoder %s: %s%s%s\n", codec ? "H.265" : "H.264",
                            pr.picked ? pr.picked : "none",
                            pr.report.empty() ? "" : " -- not usable: ", pr.report.c_str());
        }
    });
    return probes[h265];
}

} // namespace

void probeHwEncodersInBackground()
{
    std::thread([] { probe(false); }).detach();
}

bool hwEncoderAvailable()
{
    return probe(false).picked || probe(true).picked;
}

const char* hwEncoderName(bool h265)
{
    return probe(h265).picked;
}

std::string hwEncoderReport(bool h265)
{
    const Probe& pr = probe(h265);
    std::string r = pr.report;
    if (backends().only)
        r += (r.empty() ? "" : "; ") + std::string("ORBIS_ENCODER=") + backends().only;
    return r;
}

namespace {

// One candidate of openEncoder()'s loop: configure a codec context for
// `encName` -- already probed usable, on `e.hwDevice` for the
// hardware-surface encoders -- at the export's settings and try
// avcodec_open2(). False leaves `err` saying why; openEncoder() cleans the
// codec-side bits out of `e` so the next name can be tried.
bool tryEncoder(const char* encName, const FlattenConfig& p, Encoder& e, std::string& err)
{
    const AVCodec* enc = avcodec_find_encoder_by_name(encName);
    const AVHWDeviceType deviceType = deviceFor(encName);
    // HEVC Main 10 when asked (FlattenConfig::ten_bit): P010 input frames.
    // An encoder or device without it is retried at 8-bit instead of failing.
    e.tenBit = p.ten_bit && p.h265;
    for (;;) {
        const AVPixelFormat swFmt = e.tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        e.ctx = avcodec_alloc_context3(enc);
        e.ctx->width = p.out_w;
        e.ctx->height = p.out_h;
        const AVRational tb = av_d2q(1.0 / p.fps, 1000000);
        e.ctx->time_base = tb;
        e.ctx->framerate = av_inv_q(tb);
        if (deviceType == AV_HWDEVICE_TYPE_NONE) {
            e.ctx->pix_fmt = swFmt;
        } else {
            // *_vulkan / *_vaapi take hardware surfaces only; the CPU frames
            // are uploaded into `e.hw` before each send (drainEncoder).
            // sw_pix_fmt tells the encoder what those surfaces carry.
            e.ctx->pix_fmt = deviceType == AV_HWDEVICE_TYPE_VULKAN ? AV_PIX_FMT_VULKAN
                                                                 : AV_PIX_FMT_VAAPI;
            e.ctx->sw_pix_fmt = swFmt;
            e.ctx->hw_device_ctx = av_buffer_ref(e.hwDevice);
            e.hwFrames = av_hwframe_ctx_alloc(e.hwDevice);
            if (!e.hwFrames) {
                err = "cannot allocate hardware frames context";
                return false;
            }
            auto* fc = reinterpret_cast<AVHWFramesContext*>(e.hwFrames->data);
            fc->format = e.ctx->pix_fmt;
            fc->sw_format = swFmt;
            fc->width = p.out_w;
            fc->height = p.out_h;
            if (av_hwframe_ctx_init(e.hwFrames) < 0) {
                if (!e.tenBit) {
                    err = "init hardware frames context";
                    return false;
                }
                av_buffer_unref(&e.hwFrames);
                avcodec_free_context(&e.ctx);
                e.tenBit = false;
                continue;
            }
            // Vulkan video requires the frames context at open (VAAPI would
            // derive one from the device); the uploaded frames come out of it.
            e.ctx->hw_frames_ctx = av_buffer_ref(e.hwFrames);
            e.hw = av_frame_alloc();
            if (!e.hw) {
                err = "cannot allocate hardware frame";
                return false;
            }
        }
        if (e.tenBit)
            e.ctx->profile = AV_PROFILE_HEVC_MAIN_10;
        // BT.709 limited range, matching reframe_reproject.comp and the swscale
        // setup below; tagged so players don't have to guess.
        e.ctx->color_range = AVCOL_RANGE_MPEG;
        e.ctx->colorspace = AVCOL_SPC_BT709;
        e.ctx->color_primaries = AVCOL_PRI_BT709;
        e.ctx->color_trc = AVCOL_TRC_BT709;
        e.ctx->bit_rate = p.bitrate_kbps * 1000;
        // VBR peak headroom for busy scenes; the average stays at bit_rate.
        e.ctx->rc_max_rate = e.ctx->bit_rate * 3 / 2;
        e.ctx->gop_size = std::max(1, static_cast<int>(std::lround(p.fps)) * 2);
        if (e.fmt->oformat->flags & AVFMT_GLOBALHEADER)
            e.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (deviceType == AV_HWDEVICE_TYPE_NONE) {
            av_opt_set(e.ctx->priv_data, "preset", "p5", 0);
            av_opt_set(e.ctx->priv_data, "tune", "hq", 0);
            av_opt_set(e.ctx->priv_data, "rc", "vbr", 0);
        } else if (deviceType == AV_HWDEVICE_TYPE_VULKAN) {
            av_opt_set(e.ctx->priv_data, "rc_mode", "vbr", 0);
            av_opt_set(e.ctx->priv_data, "tune", "hq", 0);
            av_opt_set(e.ctx->priv_data, "usage", "transcode", 0);
            av_opt_set(e.ctx->priv_data, "content", "camera", 0);
        } else if (deviceType == AV_HWDEVICE_TYPE_VAAPI) {
            av_opt_set(e.ctx->priv_data, "rc_mode", "VBR", 0);
        }

        int ret = avcodec_open2(e.ctx, enc, nullptr);
        if (ret >= 0)
            break;
        if (!e.tenBit) {
            // The probe passed at 1080p but the real open failed (e.g. a size
            // the backend can't encode): openEncoder() drops this candidate's
            // state and tries the next name.
            err = avErr("open failed", ret);
            return false;
        }
        if (e.hw)
            av_frame_free(&e.hw);
        if (e.hwFrames)
            av_buffer_unref(&e.hwFrames);
        avcodec_free_context(&e.ctx);
        e.tenBit = false;
    }
    return true;
}

bool openEncoder(const FlattenConfig& p, Encoder& e, std::string& err)
{
    avformat_alloc_output_context2(&e.fmt, nullptr, nullptr, p.out_path.c_str());
    if (!e.fmt) {
        err = "cannot infer output container from " + p.out_path;
        return false;
    }

    // The probe's pick first, on its device; the backends after it only if
    // the real open fails (a size the 1080p probe didn't catch), probed here.
    const Probe& pr = probe(p.h265);
    const Backends& b = backends();
    const char* encName = nullptr;
    std::string why = pr.report;
    for (int i = 0; i < b.count && pr.picked && !encName; ++i) {
        const char* name = b.names[p.h265][i];
        const bool isPick = name == pr.picked;
        if (!isPick && why.find(std::string(name) + ":") != std::string::npos)
            continue;  // the probe already found it unusable
        err.clear();
        if (isPick) {
            if (pr.device)
                e.hwDevice = av_buffer_ref(pr.device);
        } else if (!encoderUsable(name, &e.hwDevice, err)) {
            why += (why.empty() ? "" : "; ") + std::string(name) + ": " + err;
            continue;
        }
        // A *_vulkan encoder runs on FlattenConfig::vulkan_device when that
        // device has an encode queue, so the caller's GPU-filled frames can
        // go straight to the encoder (acquireVulkanFrame / addVulkanFrame).
        if (e.hwDevice && p.vulkan_device && std::strstr(name, "_vulkan") &&
            vulkanCanEncode(p.vulkan_device, p.h265)) {
            av_buffer_unref(&e.hwDevice);
            e.hwDevice = av_buffer_ref(p.vulkan_device);
            e.sharedDevice = true;
        }
        if (tryEncoder(name, p, e, err)) {
            encName = name;
            break;
        }
        // Drop whatever the failed candidate left behind.
        if (e.hw)
            av_frame_free(&e.hw);
        if (e.hwFrames)
            av_buffer_unref(&e.hwFrames);
        if (e.ctx)
            avcodec_free_context(&e.ctx);
        if (e.hwDevice)
            av_buffer_unref(&e.hwDevice);
        e.sharedDevice = false;
        why += (why.empty() ? "" : "; ") + std::string(name) + ": " + err;
    }
    if (!encName) {
        if (b.only)
            why += (why.empty() ? "" : "; ") + std::string("ORBIS_ENCODER=") + b.only;
        err = "no usable hardware encoder" + (why.empty() ? std::string() : " (" + why + ")");
        return false;
    }
    int ret;
    e.stream = avformat_new_stream(e.fmt, nullptr);
    avcodec_parameters_from_context(e.stream->codecpar, e.ctx);
    e.stream->time_base = e.ctx->time_base;

    if (p.audio_sample_rate > 0) {
        const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aenc) {
            err = "no AAC encoder in this FFmpeg build";
            return false;
        }
        e.actx = avcodec_alloc_context3(aenc);
        e.actx->sample_fmt = AV_SAMPLE_FMT_FLTP; // FFmpeg's native aac encoder's only format
        e.actx->sample_rate = p.audio_sample_rate;
        av_channel_layout_default(&e.actx->ch_layout, p.audio_channels);
        e.actx->bit_rate = p.audio_bitrate_kbps * 1000;
        e.actx->time_base = AVRational{1, p.audio_sample_rate};
        if (e.fmt->oformat->flags & AVFMT_GLOBALHEADER)
            e.actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((ret = avcodec_open2(e.actx, aenc, nullptr)) < 0) {
            err = avErr("open AAC encoder", ret);
            return false;
        }
        e.astream = avformat_new_stream(e.fmt, nullptr);
        avcodec_parameters_from_context(e.astream->codecpar, e.actx);
        e.astream->time_base = e.actx->time_base;

        e.aframe = av_frame_alloc();
        e.aframe->format = e.actx->sample_fmt;
        e.aframe->sample_rate = e.actx->sample_rate;
        av_channel_layout_copy(&e.aframe->ch_layout, &e.actx->ch_layout);
        e.aframe->nb_samples = e.actx->frame_size > 0 ? e.actx->frame_size : 1024;
        if ((ret = av_frame_get_buffer(e.aframe, 0)) < 0) {
            err = avErr("allocate audio frame", ret);
            return false;
        }
    }

    if (!(e.fmt->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&e.fmt->pb, p.out_path.c_str(), AVIO_FLAG_WRITE)) < 0) {
            err = avErr("open output " + p.out_path, ret);
            return false;
        }
    }
    if ((ret = avformat_write_header(e.fmt, nullptr)) < 0) {
        err = avErr("write output header", ret);
        return false;
    }

    e.nv12 = av_frame_alloc();
    e.nv12->format = AV_PIX_FMT_NV12;
    e.nv12->width = p.out_w;
    e.nv12->height = p.out_h;
    av_frame_get_buffer(e.nv12, 0);
    e.pkt = av_packet_alloc();
    e.toNv12 = sws_getContext(p.out_w, p.out_h, AV_PIX_FMT_RGBA, p.out_w, p.out_h, AV_PIX_FMT_NV12,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!e.toNv12) {
        err = "cannot create RGBA->NV12 scaler";
        return false;
    }
    // swscale's RGB->YUV default is BT.601; make it BT.709 limited range.
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(e.toNv12, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
    if (e.tenBit) {
        // CPU-side frames stay NV12 (the frame hook's format); widen to the
        // encoder's P010 at send time.
        e.p010 = av_frame_alloc();
        e.p010->format = AV_PIX_FMT_P010LE;
        e.p010->width = p.out_w;
        e.p010->height = p.out_h;
        av_frame_get_buffer(e.p010, 0);
        e.toP010 = sws_getContext(p.out_w, p.out_h, AV_PIX_FMT_NV12, p.out_w, p.out_h,
                                  AV_PIX_FMT_P010LE, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!e.toP010) {
            err = "cannot create NV12->P010 scaler";
            return false;
        }
        sws_setColorspaceDetails(e.toP010, bt709, 0, bt709, 0, 0, 1 << 16, 1 << 16);
    }
    return true;
}

// Send `frame` (nullptr => flush) to `ctx` and mux every packet it yields
// into `stream`. Shared by the video and the audio stream.
bool drainTo(Encoder& e, AVCodecContext* ctx, AVStream* stream, AVFrame* frame, std::string& err)
{
    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        err = avErr("avcodec_send_frame", ret);
        return false;
    }
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, e.pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            err = avErr("avcodec_receive_packet", ret);
            return false;
        }
        av_packet_rescale_ts(e.pkt, ctx->time_base, stream->time_base);
        e.pkt->stream_index = stream->index;
        av_interleaved_write_frame(e.fmt, e.pkt);
        av_packet_unref(e.pkt);
    }
    return true;
}

// Send `frame` (nullptr => flush) to the video encoder. For Main 10 the
// CPU-side NV12 is widened into the P010 staging frame first, and when the
// encoder wants hardware surfaces it is uploaded into `e.hw`.
bool drainEncoder(Encoder& e, AVFrame* frame, std::string& err)
{
    if (frame && e.toP010) {
        int ret = sws_scale(e.toP010, reinterpret_cast<const uint8_t* const*>(frame->data),
                            frame->linesize, 0, frame->height, e.p010->data, e.p010->linesize);
        if (ret < 0) {
            err = avErr("NV12->P010 scale", ret);
            return false;
        }
        av_frame_copy_props(e.p010, frame);
        frame = e.p010;
    }
    if (frame && e.hw) {
        av_frame_unref(e.hw);
        int ret = av_hwframe_get_buffer(e.hwFrames, e.hw, 0);
        if (ret < 0) {
            err = avErr("allocate hardware frame", ret);
            return false;
        }
        if ((ret = av_hwframe_transfer_data(e.hw, frame, 0)) < 0) {
            err = avErr("upload frame to the GPU", ret);
            return false;
        }
        av_frame_copy_props(e.hw, frame);
        frame = e.hw;
    }
    return drainTo(e, e.ctx, e.stream, frame, err);
}

} // namespace

// --- FlattenWriter ---------------------------------------------------------

struct FlattenWriter::Impl {
    FlattenConfig cfg;
    Encoder enc;
    bool opened = false;
    int64_t written = 0; // shared across every input -> continuous output PTS
    std::vector<uint8_t> flatRgba; // reused across inputs (output-sized)
    // Audio: interleaved PCM not yet a whole AAC frame, and the running
    // sample count (fed, and encoded -> the next audio frame's PTS).
    std::vector<float> audioPending;
    int64_t audioFed = 0;
    int64_t audioEncoded = 0;
    FrameHook hook;

    // Run the hook on the NV12 frame about to be encoded.
    bool runHook(std::string& err)
    {
        if (!hook)
            return true;
        return hook(written, enc.nv12->data[0], enc.nv12->linesize[0], enc.nv12->data[1],
                    enc.nv12->linesize[1], err);
    }

    // Encode `frames` frames from the front of audioPending (<= one AAC frame).
    bool encodeAudioFrame(int frames, std::string& err);
};

bool FlattenWriter::Impl::encodeAudioFrame(int frames, std::string& err)
{
    AVFrame* f = enc.aframe;
    av_frame_make_writable(f);
    const int ch = cfg.audio_channels;
    f->nb_samples = frames;
    for (int c = 0; c < ch; ++c) {
        float* plane = reinterpret_cast<float*>(f->data[c]);
        for (int i = 0; i < frames; ++i)
            plane[i] = audioPending[static_cast<size_t>(i) * ch + c];
    }
    f->pts = audioEncoded;
    audioEncoded += frames;
    audioPending.erase(audioPending.begin(), audioPending.begin() + static_cast<ptrdiff_t>(frames) * ch);
    return drainTo(enc, enc.actx, enc.astream, f, err);
}

FlattenWriter::FlattenWriter() : d(std::make_unique<Impl>()) {}

FlattenWriter::~FlattenWriter()
{
    if (d)
        closeEncoder(d->enc);
}

bool FlattenWriter::open(const FlattenConfig& cfg, std::string& err)
{
    d->cfg = cfg;
    if (!openEncoder(d->cfg, d->enc, err)) {
        closeEncoder(d->enc);
        return false;
    }
    d->flatRgba.assign(static_cast<size_t>(cfg.out_w) * cfg.out_h * 4, 0);
    d->opened = true;
    return true;
}

bool FlattenWriter::encodeFrame(const uint8_t* eqRgba, int eqW, int eqH, const ReframeView& view,
                                std::string& err)
{
    RenderRectilinear(eqRgba, eqW, eqH, d->flatRgba.data(), d->cfg.out_w, d->cfg.out_h, view);

    av_frame_make_writable(d->enc.nv12);
    const uint8_t* src[1] = {d->flatRgba.data()};
    int srcStride[1] = {d->cfg.out_w * 4};
    sws_scale(d->enc.toNv12, src, srcStride, 0, d->cfg.out_h, d->enc.nv12->data,
              d->enc.nv12->linesize);
    if (!d->runHook(err))
        return false;
    d->enc.nv12->pts = d->written;
    if (!drainEncoder(d->enc, d->enc.nv12, err))
        return false;
    ++d->written;
    return true;
}

bool FlattenWriter::addEquirectFrame(const uint8_t* eqRgba, int eqW, int eqH, const float pose[4],
                                     const float stab[9], const color::Grade* colorGrade,
                                     std::string& err)
{
    if (!d->opened) {
        err = "FlattenWriter::addEquirectFrame before open()";
        return false;
    }
    ReframeView view{};
    view.aspect = static_cast<float>(d->cfg.out_w) / static_cast<float>(d->cfg.out_h);
    view.fovY = 1.309f;
    if (pose) {
        view.yaw = pose[0];
        view.pitch = pose[1];
        view.roll = pose[2];
        view.fovY = pose[3];
    }
    if (stab) {
        for (int i = 0; i < 9; ++i)
            view.stab[i] = stab[i];
    }
    if (colorGrade)
        view.colorGrade = *colorGrade;
    return encodeFrame(eqRgba, eqW, eqH, view, err);
}

bool FlattenWriter::addNv12Frame(const uint8_t* nv12, int w, int h, std::string& err)
{
    if (!d->opened) {
        err = "FlattenWriter::addNv12Frame before open()";
        return false;
    }
    if (w != d->cfg.out_w || h != d->cfg.out_h) {
        err = "addNv12Frame size does not match the FlattenConfig output size";
        return false;
    }

    av_frame_make_writable(d->enc.nv12);
    const uint8_t* srcY = nv12;
    const uint8_t* srcUV = nv12 + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y)
        std::memcpy(d->enc.nv12->data[0] + static_cast<ptrdiff_t>(y) * d->enc.nv12->linesize[0],
                    srcY + static_cast<size_t>(y) * w, w);
    for (int y = 0; y < h / 2; ++y)
        std::memcpy(d->enc.nv12->data[1] + static_cast<ptrdiff_t>(y) * d->enc.nv12->linesize[1],
                    srcUV + static_cast<size_t>(y) * w, w);
    if (!d->runHook(err))
        return false;
    d->enc.nv12->pts = d->written;
    if (!drainEncoder(d->enc, d->enc.nv12, err))
        return false;
    ++d->written;
    return true;
}

bool FlattenWriter::vulkanEncodeAvailable() const
{
    return d->opened && d->enc.sharedDevice && !d->hook;
}

// True when the encoder opened for Main 10 (FlattenConfig::ten_bit honored).
bool FlattenWriter::tenBit() const
{
    return d->opened && d->enc.tenBit;
}

AVFrame* FlattenWriter::acquireVulkanFrame()
{
    if (!vulkanEncodeAvailable())
        return nullptr;
    AVFrame* f = av_frame_alloc();
    if (!f || av_hwframe_get_buffer(d->enc.hwFrames, f, 0) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    return f;
}

bool FlattenWriter::addVulkanFrame(AVFrame* frame, std::string& err)
{
    if (!vulkanEncodeAvailable()) {
        err = "addVulkanFrame needs a *_vulkan encoder on the caller's hw device";
        av_frame_free(&frame);
        return false;
    }
    frame->pts = d->written;
    const bool ok = drainTo(d->enc, d->enc.ctx, d->enc.stream, frame, err);
    av_frame_free(&frame);
    if (!ok)
        return false;
    ++d->written;
    return true;
}

bool FlattenWriter::addAudio(const float* interleaved, int64_t frames, std::string& err)
{
    if (!d->opened || !d->enc.actx) {
        err = "FlattenWriter::addAudio without an open audio stream";
        return false;
    }
    if (frames <= 0)
        return true;
    const int ch = d->cfg.audio_channels;
    d->audioPending.insert(d->audioPending.end(), interleaved, interleaved + frames * ch);
    d->audioFed += frames;
    const int frameSize = d->enc.actx->frame_size > 0 ? d->enc.actx->frame_size : 1024;
    while (d->audioPending.size() >= static_cast<size_t>(frameSize) * ch) {
        if (!d->encodeAudioFrame(frameSize, err))
            return false;
    }
    return true;
}

int64_t FlattenWriter::audioFramesWritten() const { return d->audioFed; }

void FlattenWriter::setFrameHook(FrameHook hook) { d->hook = std::move(hook); }

int64_t FlattenWriter::finish(std::string& err)
{
    if (!d->opened) {
        err = "FlattenWriter::finish before open()";
        return -1;
    }
    if (!drainEncoder(d->enc, nullptr, err)) // flush encoder
        return -1;
    if (d->enc.actx) {
        // The tail (< one AAC frame): pad it to a whole frame unless the
        // encoder takes a short last frame (FFmpeg's aac does).
        const int ch = d->cfg.audio_channels;
        int tail = static_cast<int>(d->audioPending.size() / ch);
        if (tail > 0) {
            const int frameSize = d->enc.actx->frame_size > 0 ? d->enc.actx->frame_size : 1024;
            if (!(d->enc.actx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) {
                d->audioPending.resize(static_cast<size_t>(frameSize) * ch, 0.0f);
                tail = frameSize;
            }
            if (!d->encodeAudioFrame(tail, err))
                return -1;
        }
        if (!drainTo(d->enc, d->enc.actx, d->enc.astream, nullptr, err))
            return -1;
    }
    int ret = av_write_trailer(d->enc.fmt);
    if (ret < 0) {
        err = avErr("write output trailer", ret);
        return -1;
    }
    d->opened = false;
    return d->written;
}

} // namespace exportpipe
