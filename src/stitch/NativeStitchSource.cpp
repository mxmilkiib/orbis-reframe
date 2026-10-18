#include "NativeStitchSource.h"

#include "../core/Log.h"

#include <Calibration.h>           // insv::ParseExtendedCalibrationString
#include <FisheyeProjection.h>     // insv::FisheyeStitcher, insv::StitchParams
#include <Image.h>                 // insv::Image
#include <InsvDemux.h>             // insv::InsvDemux
#include <InsvTrailer.h>           // insv::ReadInsvMakerNotes
#include <VulkanFisheyeStitcher.h> // render::VulkanFisheyeStitcher
#include <VulkanVideoDecoder.h>    // insv::VulkanVideoDecoder (stage 2, GPU decode)

extern "C" {
#include <libavutil/hwcontext_vulkan.h> // AVVkFrame (ReadFlatFrameVk)
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>

namespace stitch {

namespace {

// RGB24 (insv::Image) -> interleaved RGBA (stitch::Frame), opaque alpha.
Frame ToRgbaFrame(const insv::Image& eq, int64_t timestamp_ms) {
    Frame f;
    f.width = eq.w;
    f.height = eq.h;
    f.timestamp_ms = timestamp_ms;
    const size_t n = static_cast<size_t>(eq.w) * static_cast<size_t>(eq.h);
    f.rgba.resize(n * 4);
    const uint8_t* src = eq.rgb.data();
    uint8_t* dst = f.rgba.data();
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
    }
    return f;
}

render::VulkanLensInput toRenderInput(const insv::VulkanLensFrame& lf) {
    render::VulkanLensInput in;
    in.image = lf.image;
    in.format = lf.format;
    in.layout = lf.layout;
    in.semaphore = lf.semaphore;
    in.semWaitValue = lf.semWaitValue;
    in.width = lf.width;
    in.height = lf.height;
    in.usage = lf.usage;
    return in;
}

} // namespace

const char* LensGuardProfileName(PreviewStitchConfig::LensGuard guard) {
    // ProtectorA / ProtectorS / ProtectorAS carry identical data on the X5;
    // which letter means which guard is a guess (A = premium, S = standard).
    switch (guard) {
        case PreviewStitchConfig::LensGuard::Standard: return "ProtectorS";
        case PreviewStitchConfig::LensGuard::Premium: return "ProtectorA";
        case PreviewStitchConfig::LensGuard::None: break;
    }
    return "";
}

NativeStitchSource::NativeStitchSource() = default;
NativeStitchSource::~NativeStitchSource() {
    // Held / kept frames go before the decoder.
    while (!kept_.empty())
        ReleaseKeptPair(kept_.begin()->first);
    // Tear the GPU stitch down before the decoder: on the all-GPU path the
    // stitcher borrows the decoder's VkDevice.
    gpu_.reset();
    vdec_.reset();
}

bool NativeStitchSource::Open(const std::string& clip_path, const PreviewStitchConfig& config) {
    while (!kept_.empty())
        ReleaseKeptPair(kept_.begin()->first);
    vdec_.reset();
    demux_.reset();
    stitcher_.reset();
    gpu_.reset();
    info_ = ClipInfo{};
    last_error_.clear();

    const auto notes = insv::ReadInsvMakerNotes(clip_path);
    if (!notes) {
        last_error_ = "no readable Insta360 trailer in " + clip_path;
        return false;
    }
    // The extended MEI calibration (what Insta360 Studio stitches with) --
    // every X5 clip carries it.
    insv::InsvCalibration calib;
    const auto ext = insv::ReadInsvExtendedCalibrationString(clip_path);
    if (!ext || !insv::ParseExtendedCalibrationString(*ext, calib)) {
        last_error_ = "no extended (MEI) lens calibration in the trailer";
        return false;
    }

    // Metadata: probe with a lightweight CPU demux open (cheap -- no decode).
    {
        insv::InsvDemux probe;
        std::string err;
        if (!probe.open(clip_path, err)) {
            last_error_ = "demux open failed: " + err;
            return false;
        }
        if (probe.lensCount() < 2) {
            last_error_ = "expected 2 fisheye video streams, found " +
                          std::to_string(probe.lensCount());
            return false;
        }
        const insv::InsvStreamInfo& l0 = probe.lens(0);
        info_.width = l0.width;
        info_.height = l0.height;
        info_.fps = l0.fps;
        info_.duration_ms = l0.durationMs;
    }

    eq_w_ = config.width > 0 ? config.width : AutoPreviewWidth(info_.width, info_.fps);
    eq_h_ = config.width > 0 && config.height > 0 ? config.height : eq_w_ / 2;

    insv::StitchParams params;
    params.depthSeam = config.depthSeam;
    depth_.reset();
    if (!seam_costs_)
        seam_costs_ = std::make_unique<insv::SeamCosts>();
    const char* guardProfile = LensGuardProfileName(config.lensGuard);
    if (!insv::ApplyLensProfile(params, *notes, guardProfile))
        std::fprintf(stderr, "[NativeStitchSource] no \"%s\" lens profile in the trailer; "
                             "stitching as a bare lens\n", guardProfile);
    auto stitcher = std::make_unique<insv::FisheyeStitcher>();
    if (!stitcher->configure(calib, info_.width, info_.height, params)) {
        last_error_ = "FisheyeStitcher::configure failed";
        return false;
    }
    gpu_params_ = stitcher->gpuParams();
    calib_ = calib;
    params_ = params;

    // --- Stage 2: all-GPU (Vulkan video decode -> GPU stitch on one device) ---
    if (!std::getenv("INSV_NO_GPU_DECODE") && !std::getenv("INSV_NO_GPU_STITCH")) {
        auto vdec = std::make_unique<insv::VulkanVideoDecoder>();
        std::string derr;
        if (vdec->open(clip_path, derr)) {
            VkInstance inst = VK_NULL_HANDLE;
            VkPhysicalDevice phys = VK_NULL_HANDLE;
            VkDevice dev = VK_NULL_HANDLE;
            uint32_t qf = 0;
            VkDeviceQueueCreateFlags qflags = 0;
            vdec->deviceHandles(inst, phys, dev, qf, qflags);
            auto gpu = std::make_unique<render::VulkanFisheyeStitcher>();
            std::string gerr;
            // The stitcher bakes the frames' real format (Main-10 clips come
            // in as G10X6..., 8-bit as G8_B8R8) into its ycbcr conversion;
            // peek a frame to learn it before the pipelines are built.
            const VkFormat frameFmt = vdec->frameFormat();
            if (gpu->initAdopt(inst, phys, dev, qf, qflags, frameFmt, gerr)) {
                auto ql = vdec->queueLock(qf, 0);
                gpu->setQueueLock(std::move(ql.lock), std::move(ql.unlock));
                gpu->setDeviceKeepAlive(vdec->deviceKeepAlive());
                vdec_ = std::move(vdec);
                gpu_ = std::move(gpu);
                stitcher_ = std::move(stitcher);
                core::log::Info("[NativeStitchSource] all-GPU path: Vulkan video decode + "
                                "compute stitch\n");
                return true;
            }
            std::fprintf(stderr, "[NativeStitchSource] GPU stitch adopt failed (%s); "
                                 "falling back to CPU decode\n",
                         gerr.c_str());
        } else {
            std::fprintf(stderr, "[NativeStitchSource] Vulkan video decode unavailable (%s); "
                                 "CPU decode\n",
                         derr.c_str());
        }
    }

    // --- Stage 1 / fallback: CPU decode + (GPU or CPU) stitch ---
    auto demux = std::make_unique<insv::InsvDemux>();
    std::string err;
    if (!demux->open(clip_path, err)) {
        last_error_ = "demux open failed: " + err;
        return false;
    }
    if (!std::getenv("INSV_NO_GPU_STITCH")) {
        auto gpu = std::make_unique<render::VulkanFisheyeStitcher>();
        std::string gerr;
        if (gpu->init(gerr))
            gpu_ = std::move(gpu);
        else
            std::fprintf(stderr, "[NativeStitchSource] GPU stitch unavailable (%s); using CPU\n",
                         gerr.c_str());
    }
    demux_ = std::move(demux);
    stitcher_ = std::move(stitcher);
    return true;
}

bool NativeStitchSource::SeekTo(int64_t timestamp_ms) {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    const int64_t t = timestamp_ms < 0 ? 0 : timestamp_ms;
    depth_.reset(); // the seam's temporal history is about the frames before
    std::string err;
    if (vdec_) {
        if (!vdec_->seekTo(t, err)) {
            last_error_ = "seek failed: " + err;
            return false;
        }
        return true;
    }
    if (!demux_) {
        last_error_ = "SeekTo before a successful Open";
        return false;
    }
    if (!demux_->seekTo(t, err)) {
        last_error_ = "seek failed: " + err;
        return false;
    }
    return true;
}

void NativeStitchSource::copyGpuTiming() {
    const render::StitchTiming& t = gpu_->lastTiming();
    last_timing_.seamMs = seam_ms_;
    last_timing_.fenceWaitMs = t.fenceWaitMs;
    last_timing_.stitchGpuMs = t.stitchGpuMs;
    last_timing_.reframeGpuMs = t.reframeGpuMs;
    last_timing_.copyMs = t.copyMs;
}

int64_t NativeStitchSource::NearestKeyframeMs(int64_t timestamp_ms) const {
    if (vdec_)
        return vdec_->nearestKeyframeMs(timestamp_ms);
    if (demux_)
        return demux_->nearestKeyframeMs(timestamp_ms);
    return timestamp_ms;
}

void NativeStitchSource::KeyframesAround(int64_t timestamp_ms, int64_t& before, int64_t& after) const {
    before = after = -1;
    if (vdec_)
        vdec_->keyframesAroundMs(timestamp_ms, before, after);
    else if (demux_)
        demux_->keyframesAroundMs(timestamp_ms, before, after);
}

Frame NativeStitchSource::readGpuAllVulkan() {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    int64_t pts_ms = 0;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(f0, f1, &pts_ms, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return Frame{};
    }

    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(f0, lf0);
    vdec_->lockFrame(f1, lf1);

    Frame frame;
    frame.width = eq_w_;
    frame.height = eq_h_;
    frame.timestamp_ms = pts_ms;
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    const insv::StitchGpuParams params = paramsWithSeam(pts_ms, in0, in1);
    const bool ok = gpu_->stitchVk(in0, in1, params, eq_w_, eq_h_, frame.rgba, &oc0, &oc1, gerr);

    insv::VulkanLensFrameResult r0{oc0.semSignalValue, oc0.newLayout};
    insv::VulkanLensFrameResult r1{oc1.semSignalValue, oc1.newLayout};
    if (ok) {
        vdec_->unlockFrame(f0, r0);
        vdec_->unlockFrame(f1, r1);
    } else {
        // On failure the frames are as the seam measure (if any) left them.
        vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
        vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);

    if (!ok) {
        last_error_ = "GPU stitchVk failed: " + gerr;
        return Frame{};
    }
    copyGpuTiming();
    return frame;
}

void NativeStitchSource::SetLens1Adjust(const double deg[3]) {
    if (!stitcher_ || std::equal(deg, deg + 3, params_.lens1AdjustDeg))
        return;
    std::copy(deg, deg + 3, params_.lens1AdjustDeg);
    if (stitcher_->configure(calib_, info_.width, info_.height, params_))
        gpu_params_ = stitcher_->gpuParams(); // keeps the per-frame RS fields: set in paramsAt
}

void NativeStitchSource::SetCaptureTiming(CaptureTiming timing, int64_t untilMs,
                                          CaptureTiming after) {
    std::lock_guard<std::mutex> lk(rs_mx_);
    capture_timing_ = std::move(timing);
    capture_until_ms_ = untilMs;
    capture_after_ = std::move(after);
}

bool NativeStitchSource::SetBlurQuads(BlurQuadsFn fn) {
    std::lock_guard<std::mutex> lk(rs_mx_);
    blur_quads_ = std::move(fn);
    return true;
}

insv::StitchGpuParams NativeStitchSource::paramsAt(int64_t pts_ms) const {
    insv::StitchGpuParams p = gpu_params_;
    CaptureTiming ct;
    BlurQuadsFn blur;
    {
        std::lock_guard<std::mutex> lk(rs_mx_);
        // copy: call rotationAt outside the lock
        ct = pts_ms < capture_until_ms_ ? capture_timing_ : capture_after_;
        blur = blur_quads_;
    }
    p.blurCount = 0;
    if (blur) {
        std::vector<insv::BlurQuad> quads;
        blur(pts_ms, quads);
        for (const insv::BlurQuad& q : quads) {
            if (p.blurCount == insv::kMaxBlurQuads) {
                if (!blur_warned_) {
                    blur_warned_ = true;
                    std::fprintf(stderr, "[NativeStitchSource] %zu blur regions at %lld ms; only %d "
                                         "fit\n",
                                 quads.size(), static_cast<long long>(pts_ms), insv::kMaxBlurQuads);
                }
                break;
            }
            if (insv::prepareBlurQuad(q, p.blur[p.blurCount]))
                ++p.blurCount;
        }
    }
    if (!ct.active())
        return p;
    const double t = pts_ms / 1000.0;
    constexpr int kTaps = insv::StitchGpuParams::kRsTaps;
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < kTaps; ++k)
            ct.rotationAt(t,
                          ct.lensOffsetSec[i] +
                              (static_cast<double>(k) / (kTaps - 1) - 0.5) * ct.readoutSec[i],
                          p.rsRot[i][k]);
    p.rsActive = 1;
    p.rsUseColumns = ct.scanColumns ? 1 : 0;
    return p;
}

insv::StitchGpuParams NativeStitchSource::paramsWithSeam(int64_t pts_ms,
                                                         render::VulkanLensInput& in0,
                                                         render::VulkanLensInput& in1) {
    insv::StitchGpuParams p = paramsAt(pts_ms);
    seam_ms_ = 0.0;
    if (!p.depthActive)
        return p;
    const auto t0 = std::chrono::steady_clock::now();
    render::VulkanLensOutcome o0, o1;
    std::string err;
    if (gpu_->measureSeamVk(in0, in1, p, *seam_costs_, &o0, &o1, err)) {
        in0.semWaitValue = o0.semSignalValue;
        in0.layout = o0.newLayout;
        in1.semWaitValue = o1.semSignalValue;
        in1.layout = o1.newLayout;
        depth_.update(*seam_costs_, pts_ms);
    } else if (!seam_warned_) {
        seam_warned_ = true;
        std::fprintf(stderr, "[NativeStitchSource] seam depth measure failed (%s); keeping the "
                             "last table\n",
                     err.c_str());
    }
    std::copy(depth_.table(), depth_.table() + insv::kSeamDepthBins, p.seamQ);
    seam_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                   .count();
    return p;
}

bool NativeStitchSource::SensorRowOf(const double eqDir[3], int& lens, double& rowFrac) const {
    if (!stitcher_ || info_.height <= 0)
        return false;
    const insv::Mat3& R = stitcher_->outRotation(); // stitch world = outRot * eqDir
    const insv::Vec3 w{R.m[0][0] * eqDir[0] + R.m[0][1] * eqDir[1] + R.m[0][2] * eqDir[2],
                       R.m[1][0] * eqDir[0] + R.m[1][1] * eqDir[1] + R.m[1][2] * eqDir[2],
                       R.m[2][0] * eqDir[0] + R.m[2][1] * eqDir[1] + R.m[2][2] * eqDir[2]};
    double u[2], v[2], th[2];
    bool ok[2];
    for (int i = 0; i < 2; ++i)
        ok[i] = stitcher_->project(i, w, u[i], v[i], th[i]);
    constexpr double kSeamBandRad = 12.0 * std::numbers::pi / 180.0;
    if (ok[0] && ok[1] && std::fabs(th[0] - th[1]) < kSeamBandRad)
        return false; // blended from both lenses
    if (!ok[0] && !ok[1])
        return false;
    lens = (ok[0] && (!ok[1] || th[0] < th[1])) ? 0 : 1;
    rowFrac = v[lens] / static_cast<double>(info_.height);
    return rowFrac >= 0.0 && rowFrac <= 1.0;
}

bool NativeStitchSource::GpuReframeAvailable() const {
    return vdec_ && gpu_ && (gpu_->reframeReady() || gpu_->viewReady());
}

bool NativeStitchSource::GpuViewAvailable() const {
    return vdec_ && gpu_ && gpu_->viewReady();
}

FlatFrame NativeStitchSource::ReadFlatFrame(int outW, int outH, const float pose[4],
                                            const float stab[9],
                                            const float colorGrade[4]) {
    return ReadFlatFrame(
        outW, outH,
        [pose, stab](int64_t, float p[4], float s[9]) {
            if (pose)
                std::copy(pose, pose + 4, p);
            if (stab)
                std::copy(stab, stab + 9, s);
        },
        colorGrade);
}

FlatFrame NativeStitchSource::ReadFlatFrame(int outW, int outH, const FramePoseFn& perFrame,
                                            const float colorGrade[4]) {
    if (!GpuReframeAvailable()) {
        last_error_ = "ReadFlatFrame without the all-GPU reproject path";
        return FlatFrame{};
    }
    // The fused pass unless it's missing or INSV_FLAT_VIA_EQUIRECT asks for
    // the old equirect + reproject (comparisons, and a way back if needed).
    const bool fused = GpuViewAvailable() &&
                       (!gpu_->reframeReady() || !std::getenv("INSV_FLAT_VIA_EQUIRECT"));
    FlatFrame frame;
    if (!readView(outW, outH, perFrame, colorGrade,
                  fused ? FlatPath::FusedNv12 : FlatPath::ViaEquirect, frame.nv12,
                  nullptr, frame.timestamp_ms))
        return FlatFrame{};
    frame.width = outW;
    frame.height = outH;
    return frame;
}

Frame NativeStitchSource::ReadViewFrame(int outW, int outH, const FramePoseFn& perFrame,
                                        const float colorGrade[4]) {
    if (!GpuViewAvailable()) {
        last_error_ = "ReadViewFrame without the fused GPU view path";
        return Frame{};
    }
    Frame frame;
    if (!readView(outW, outH, perFrame, colorGrade, FlatPath::FusedRgba, frame.rgba,
                  nullptr, frame.timestamp_ms))
        return Frame{};
    frame.width = outW;
    frame.height = outH;
    return frame;
}

AVBufferRef* NativeStitchSource::vulkanHwDevice() const {
    // Only the GPU path runs on Vulkan at all; the device is the decoder's
    // shared one.
    return vdec_ ? insv::sharedVulkanDevice() : nullptr;
}

bool NativeStitchSource::ReadFlatFrameVk(int outW, int outH, const FramePoseFn& perFrame,
                                         const float colorGrade[4], AVFrame* dst,
                                         int64_t& pts_ms) {
    if (!GpuViewAvailable()) {
        last_error_ = "ReadFlatFrameVk without the fused GPU view path";
        return false;
    }
    if (!dst || dst->format != AV_PIX_FMT_VULKAN || !dst->data[0]) {
        last_error_ = "ReadFlatFrameVk wants an AV_PIX_FMT_VULKAN frame";
        return false;
    }
    std::vector<uint8_t> unused;
    return readView(outW, outH, perFrame, colorGrade, FlatPath::FusedNv12, unused, dst,
                    pts_ms);
}

bool NativeStitchSource::readView(int outW, int outH, const FramePoseFn& perFrame,
                                  const float colorGrade[4], FlatPath path,
                                  std::vector<uint8_t>& out, AVFrame* vkDst,
                                  int64_t& pts_ms) {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    pts_ms = 0;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(f0, f1, &pts_ms, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return false;
    }

    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(f0, lf0);
    vdec_->lockFrame(f1, lf1);

    float pose[4] = {0.0f, 0.0f, 0.0f, 1.309f}; // forward view, the reproject's default
    float stab[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (perFrame)
        perFrame(pts_ms, pose, stab);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    const insv::StitchGpuParams params = paramsWithSeam(pts_ms, in0, in1);
    bool ok;
    if (vkDst) {
        // AVVkFrame: multi-plane NV12 is one image, per-plane is two.
        auto* vkf = reinterpret_cast<AVVkFrame*>(vkDst->data[0]);
        render::VulkanFisheyeStitcher::FlatImageTarget tgt{};
        tgt.nbImages = vkf->img[1] ? 2 : 1;
        for (int i = 0; i < tgt.nbImages; ++i) {
            tgt.img[i] = vkf->img[i];
            tgt.layout[i] = vkf->layout[i];
            tgt.access[i] = vkf->access[i];
            tgt.sem[i] = vkf->sem[i];
            tgt.semValue[i] = vkf->sem_value[i];
        }
        ok = gpu_->renderViewVkToImage(in0, in1, params, eq_w_, eq_h_, outW, outH, pose,
                                       stab, colorGrade, tgt, &oc0, &oc1, gerr);
        for (int i = 0; i < tgt.nbImages; ++i) {
            vkf->layout[i] = tgt.layout[i];
            vkf->access[i] = tgt.access[i];
            vkf->sem_value[i] = tgt.semValue[i];
        }
    } else {
        ok = path == FlatPath::ViaEquirect
                 ? gpu_->stitchAndReframeVk(in0, in1, params, eq_w_, eq_h_, outW, outH, pose,
                                            stab, colorGrade, out, &oc0, &oc1, gerr)
                 : gpu_->renderViewVk(in0, in1, params, eq_w_, eq_h_, outW, outH, pose, stab,
                                      colorGrade,
                                      path == FlatPath::FusedRgba
                                          ? render::VulkanFisheyeStitcher::ViewFormat::Rgba
                                          : render::VulkanFisheyeStitcher::ViewFormat::Nv12,
                                      out, &oc0, &oc1, gerr);
    }

    insv::VulkanLensFrameResult r0{oc0.semSignalValue, oc0.newLayout};
    insv::VulkanLensFrameResult r1{oc1.semSignalValue, oc1.newLayout};
    if (ok) {
        vdec_->unlockFrame(f0, r0);
        vdec_->unlockFrame(f1, r1);
    } else {
        vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
        vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);

    if (!ok) {
        last_error_ = std::string(path == FlatPath::ViaEquirect ? "GPU stitchAndReframeVk"
                                                                : "GPU renderViewVk") +
                      " failed: " + gerr;
        return false;
    }
    copyGpuTiming();
    return true;
}

struct NativeStitchSource::HeldPair {
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    insv::StitchGpuParams params;
    int64_t pts_ms = 0;
};

int NativeStitchSource::KeepNextPair(int64_t& pts_ms) {
    if (!GpuViewAvailable()) {
        last_error_ = "KeepNextPair without the fused view path";
        return 0;
    }
    auto h = std::make_unique<HeldPair>();
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(h->f0, h->f1, &h->pts_ms, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return 0;
    }
    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(h->f0, lf0);
    vdec_->lockFrame(h->f1, lf1);
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    h->params = paramsWithSeam(h->pts_ms, in0, in1);
    vdec_->unlockFrame(h->f0, {in0.semWaitValue, in0.layout});
    vdec_->unlockFrame(h->f1, {in1.semWaitValue, in1.layout});
    pts_ms = h->pts_ms;
    const int id = next_kept_++;
    kept_.emplace(id, std::move(h));
    return id;
}

bool NativeStitchSource::RenderKeptViews(int handle, int canvasW, int canvasH,
                                         const std::vector<ViewRect>& views, const float stab[9],
                                         uint32_t clearRgba, std::vector<uint8_t>& rgba) {
    const auto it = kept_.find(handle);
    if (it == kept_.end()) {
        last_error_ = "RenderKeptViews without that kept pair";
        return false;
    }
    HeldPair& h = *it->second;
    std::vector<render::VulkanFisheyeStitcher::ViewRect> vr(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        std::copy(views[i].pose, views[i].pose + 4, vr[i].pose);
        vr[i].x = views[i].x;
        vr[i].y = views[i].y;
        vr[i].w = views[i].w;
        vr[i].h = views[i].h;
    }
    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(h.f0, lf0);
    vdec_->lockFrame(h.f1, lf1);
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    const bool ok = gpu_->renderViewsVk(in0, in1, h.params, eq_w_, eq_h_, canvasW, canvasH, vr, stab,
                                        clearRgba, rgba, &oc0, &oc1, gerr);
    if (ok) {
        vdec_->unlockFrame(h.f0, {oc0.semSignalValue, oc0.newLayout});
        vdec_->unlockFrame(h.f1, {oc1.semSignalValue, oc1.newLayout});
        copyGpuTiming();
        return true;
    }
    vdec_->unlockFrame(h.f0, {in0.semWaitValue, in0.layout});
    vdec_->unlockFrame(h.f1, {in1.semWaitValue, in1.layout});
    last_error_ = "GPU renderViewsVk failed: " + gerr;
    return false;
}

void NativeStitchSource::ReleaseKeptPair(int handle) {
    const auto it = kept_.find(handle);
    if (it == kept_.end())
        return;
    vdec_->freeFrame(it->second->f0);
    vdec_->freeFrame(it->second->f1);
    kept_.erase(it);
    if (handle == held_)
        held_ = 0;
}

size_t NativeStitchSource::KeptPairBytes() const {
    // Two NV12 lens images (one decoded pair).
    size_t bytes = 0;
    if (vdec_)
        for (int i = 0; i < vdec_->lensCount(); ++i)
            bytes += size_t(vdec_->lens(i).width) * vdec_->lens(i).height * 3 / 2;
    return bytes;
}

bool NativeStitchSource::HoldNextPair(int64_t& pts_ms) {
    ReleaseHeldPair();
    held_ = KeepNextPair(pts_ms);
    if (!held_ && last_error_ == "KeepNextPair without the fused view path")
        last_error_ = "HoldNextPair without the fused view path";
    return held_ != 0;
}

bool NativeStitchSource::RenderHeldViews(int canvasW, int canvasH, const std::vector<ViewRect>& views,
                                         const float stab[9], uint32_t clearRgba,
                                         std::vector<uint8_t>& rgba) {
    if (!held_) {
        last_error_ = "RenderHeldViews without a held pair";
        return false;
    }
    return RenderKeptViews(held_, canvasW, canvasH, views, stab, clearRgba, rgba);
}

void NativeStitchSource::ReleaseHeldPair() {
    if (held_)
        ReleaseKeptPair(held_);
}

bool NativeStitchSource::LensFramesAvailable() const {
    return vdec_ && gpu_ && gpu_->lensExportReady() && !std::getenv("INSV_PLAYBACK_EQUIRECT");
}

void NativeStitchSource::TrimLensFrames() {
    if (gpu_)
        gpu_->trimLensRing();
}

bool NativeStitchSource::ReadLensFrame(render::LensFrame& out) {
    ReleaseHeldPair(); // a held frame goes before the decoder moves on
    if (!LensFramesAvailable()) {
        last_error_ = "ReadLensFrame without the all-GPU lens export path";
        return false;
    }
    AVFrame* f0 = nullptr;
    AVFrame* f1 = nullptr;
    int64_t pts_ms = 0;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = vdec_->readNextPair(f0, f1, &pts_ms, err);
    last_timing_ = ReadTiming{};
    last_timing_.decodeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!got) {
        last_error_ = err; // empty at EOS
        return false;
    }

    insv::VulkanLensFrame lf0, lf1;
    vdec_->lockFrame(f0, lf0);
    vdec_->lockFrame(f1, lf1);
    render::VulkanLensOutcome oc0, oc1;
    std::string gerr;
    render::VulkanLensInput in0 = toRenderInput(lf0), in1 = toRenderInput(lf1);
    const insv::StitchGpuParams params = paramsWithSeam(pts_ms, in0, in1);
    // The photometric measure runs on the equirect grid the stitch would use
    // at this source's configured size, so the seam matches the other paths.
    const bool ok = gpu_->fillLensFrameVk(in0, in1, params, eq_w_, eq_h_, out, &oc0, &oc1, gerr);
    if (ok) {
        vdec_->unlockFrame(f0, {oc0.semSignalValue, oc0.newLayout});
        vdec_->unlockFrame(f1, {oc1.semSignalValue, oc1.newLayout});
    } else {
        vdec_->unlockFrame(f0, {in0.semWaitValue, in0.layout});
        vdec_->unlockFrame(f1, {in1.semWaitValue, in1.layout});
    }
    vdec_->freeFrame(f0);
    vdec_->freeFrame(f1);
    if (!ok) {
        last_error_ = "GPU fillLensFrameVk failed: " + gerr;
        return false;
    }
    out.timestamp_ms = pts_ms;
    copyGpuTiming();
    return true;
}

Frame NativeStitchSource::readCpuDecode() {
    insv::Image a, b;
    int64_t pts_ms = 0;
    std::string err;
    if (!demux_->readNextPair(a, b, &pts_ms, err)) {
        last_error_ = err; // empty on a clean end-of-stream
        return Frame{};
    }

    // Depth-aware seam: the cost volume on the CPU here (no GPU-resident
    // lenses to run the GPU pass on).
    if (params_.depthSeam) {
        const auto t0 = std::chrono::steady_clock::now();
        insv::MeasureSeamCosts(*stitcher_, a, b, *seam_costs_);
        depth_.update(*seam_costs_, pts_ms);
        last_timing_.seamMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
    }
    const float* seamQ = params_.depthSeam ? depth_.table() : nullptr;

    if (gpu_) {
        Frame f;
        f.width = eq_w_;
        f.height = eq_h_;
        f.timestamp_ms = pts_ms;
        std::string gerr;
        insv::StitchGpuParams p = paramsAt(pts_ms);
        if (seamQ)
            std::copy(seamQ, seamQ + insv::kSeamDepthBins, p.seamQ);
        if (gpu_->stitch(a.rgb.data(), b.rgb.data(), p, eq_w_, eq_h_, f.rgba, gerr))
            return f;
        std::fprintf(stderr, "[NativeStitchSource] GPU stitch failed (%s); falling back to CPU\n",
                     gerr.c_str());
        gpu_.reset();
    }

    insv::Image eq;
    eq.alloc(eq_w_, eq_h_);
    const insv::StitchGpuParams p = paramsAt(pts_ms); // the privacy blur quads
    stitcher_->renderEquirect(a, b, eq, seamQ, p.blur, p.blurCount);
    return ToRgbaFrame(eq, pts_ms);
}

Frame NativeStitchSource::ReadFrame() {
    if (vdec_)
        return readGpuAllVulkan();
    if (demux_ && stitcher_)
        return readCpuDecode();
    last_error_ = "ReadFrame before a successful Open";
    return Frame{};
}

Frame NativeStitchSource::GetFrame(int64_t timestamp_ms) {
    if (!SeekTo(timestamp_ms))
        return Frame{};
    return ReadFrame();
}

std::unique_ptr<IStitchSource> MakeStitchSource() {
    return std::make_unique<NativeStitchSource>();
}

} // namespace stitch
