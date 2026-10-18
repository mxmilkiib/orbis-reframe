#include "ReframeExportController.h"

#include "ReframeFlatten.h"  // exportpipe::FlattenWriter (system FFmpeg)
#include "ReframeRenderer.h" // exportpipe::RenderRectilinear

#include <InsvTrailer.h>        // insv::ReadInsvTelemetry
#include <NativeStitchSource.h> // stitch::NativeStitchSource, stitch::Frame, stitch::PreviewStitchConfig
#include <ProbeClip.h>          // stitch::ProbeClip
#include <VulkanVideoDecoder.h> // insv::sharedVulkanDevice

extern "C" {
#include <libavutil/buffer.h> // av_buffer_unref
#include <libavutil/frame.h>  // av_frame_alloc / av_frame_ref / av_frame_free
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kAudioRate = 48000; // the X5's own rate -- no resampling in practice
constexpr int kAudioChannels = 2;

} // namespace

stitch::CaptureTiming
ReframeExportController::captureTimingFor(std::shared_ptr<const insv::GyroStabilizer> stab)
{
    stitch::CaptureTiming t;
    if (!stab || !stab->ready())
        return t;
    t.readoutSec[0] = t.readoutSec[1] = stab->readoutSec();
    t.rotationAt = [stab](double tSec, double dtSec, float r[3]) {
        stab->bodyRotation(tSec, dtSec, r);
    };
    return t;
}

std::shared_ptr<const insv::GyroStabilizer>
ReframeExportController::buildStabilizer(const QString &path, double fps,
                                         const insv::StabConfig &config, double syncMs)
{
    auto tele = insv::ReadInsvTelemetry(path.toStdString());
    if (!tele || tele->imu.size() < 2)
        return nullptr;
    auto stab = std::make_shared<insv::GyroStabilizer>();
    stab->build(*tele, fps, config, syncMs / 1000.0);
    return stab->ready() ? stab : nullptr;
}

QImage ReframeExportController::renderStill(const ClipJob &c, qint64 sourceMs,
                                            const float pose[4], int outW, int outH,
                                            QString &error)
{
    if (outW <= 0 || outH <= 0) {
        error = tr("Invalid output size.");
        return {};
    }
    if (c.flat) { // the picture is the caller's inset layer
        QImage black(outW, outH, QImage::Format_RGB888);
        black.fill(Qt::black);
        return black;
    }
    stitch::ClipProbe probe;
    std::string perr;
    if (!stitch::ProbeClip(c.clipPath.toStdString(), probe, perr))
        probe = stitch::ClipProbe{};
    stitch::PreviewStitchConfig sc;
    sc.width = stitch::ExportStitchWidth(probe.width, outW) & ~1;
    sc.height = (sc.width / 2) & ~1;
    sc.lensGuard = c.lensGuard;
    sc.depthSeam = c.depthSeam;

    stitch::NativeStitchSource src;
    if (!src.Open(c.clipPath.toStdString(), sc) || !src.SeekTo(sourceMs)) {
        error = QString::fromStdString(src.GetLastError());
        return {};
    }
    src.SetCaptureTiming(captureTimingFor(c.stab));
    src.SetLens1Adjust(c.seamAdjustDeg.data());
    if (c.blur && !src.SetBlurQuads(c.blur)) {
        error = tr("This stitch can't apply the privacy blur.");
        return {};
    }

    // Stab at the decoded frame's own time, as the export does.
    auto stabAt = [&c](int64_t ptsMs, float stab[9]) {
        if (c.stab) {
            float m[3][3];
            c.stab->correctionMatrix(ptsMs / 1000.0, m);
            std::copy(&m[0][0], &m[0][0] + 9, stab);
        }
    };

    // The fused GPU pass (straight from the fisheyes, one resample)
    // when there is one -- needs outW % 4 == 0, as every preset is.
    if (src.GpuViewAvailable() && outW % 4 == 0 && outH % 2 == 0) {
        const stitch::Frame f = src.ReadViewFrame(
            outW, outH,
            [&](int64_t ptsMs, float p[4], float stab[9]) {
                std::copy(pose, pose + 4, p);
                stabAt(ptsMs, stab);
            },
            &c.colorGrade);
        if (f.rgba.empty()) {
            error = src.GetLastError().empty() ? tr("No frame at the playhead.")
                                               : QString::fromStdString(src.GetLastError());
            return {};
        }
        const QImage view(f.rgba.data(), outW, outH, outW * 4, QImage::Format_RGBA8888);
        return view.convertToFormat(QImage::Format_RGB888); // deep copy; alpha is 255
    }

    // Else: the equirect, reprojected on the CPU.
    const stitch::Frame eq = src.ReadFrame();
    if (eq.rgba.empty()) {
        error = src.GetLastError().empty() ? tr("No frame at the playhead.")
                                           : QString::fromStdString(src.GetLastError());
        return {};
    }

    exportpipe::ReframeView view;
    view.yaw = pose[0];
    view.pitch = pose[1];
    view.roll = pose[2];
    view.fovY = pose[3];
    view.aspect = static_cast<float>(outW) / outH;
    stabAt(eq.timestamp_ms, view.stab);
    view.colorGrade = c.colorGrade;

    QImage img(outW, outH, QImage::Format_RGBA8888);
    exportpipe::RenderRectilinear(eq.rgba.data(), eq.width, eq.height, img.bits(), outW, outH,
                                  view);
    return img.convertToFormat(QImage::Format_RGB888); // opaque: alpha is always 255
}

ReframeExportController::ReframeExportController(QObject *parent)
    : QObject(parent)
{
}

void ReframeExportController::run(const Request &req)
{
    m_cancel.store(false, std::memory_order_relaxed);

    if (req.clips.isEmpty()) {
        emit finished(false, tr("The timeline is empty."), req.outPath);
        return;
    }

    // Which output frames each clip fills: those whose timeline time
    // (llround(k * outFrameMs), as the overlays and transitions are drawn
    // at) falls in its span on the timeline. So the cut lands on the same
    // frame for the main track as for a transition's layers over it -- a
    // frame apart, one clip would show at full strength in the middle of a
    // crossfade -- and it doesn't drift by rounding each clip's length on
    // its own. Also each clip's weight for the one progress bar.
    const double outFrameMs = 1000.0 / req.fps;
    const auto frameAtOrAfter = [outFrameMs](int64_t ms) {
        int64_t k = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(ms / outFrameMs)));
        while (k > 0 && std::llround((k - 1) * outFrameMs) >= ms)
            --k;
        while (std::llround(k * outFrameMs) < ms)
            ++k;
        return k;
    };
    const int n = req.clips.size();
    std::vector<int64_t> frames(n), firstFrame(n), clipStartMs(n);
    int64_t totalFrames = 0;
    int64_t startMs = 0;
    for (int i = 0; i < n; ++i) {
        const ClipJob &c = req.clips[i];
        // Its span on the timeline, at its speed (ClipItem::timelineDurationMs).
        const int64_t endMs = startMs + std::llround((c.trimOutMs - c.trimInMs) / c.speed);
        clipStartMs[i] = startMs;
        firstFrame[i] = frameAtOrAfter(startMs);
        frames[i] = std::max<int64_t>(0, frameAtOrAfter(endMs) - firstFrame[i]);
        totalFrames += frames[i];
        startMs = endMs;
    }
    if (totalFrames <= 0)
        totalFrames = 1;

    auto emitPct = [&](double pct, const QString &label) {
        emit progress(std::clamp(static_cast<int>(std::lround(pct)), 0, 100), label);
    };


    exportpipe::FlattenConfig cfg;
    cfg.out_path = req.outPath.toStdString();
    cfg.out_w = req.outWidth;
    cfg.out_h = req.outHeight;
    cfg.fps = req.fps;
    cfg.h265 = req.h265;
    cfg.bitrate_kbps = req.bitrateKbps;
    // An audio track only if there's something on the audio tracks.
    const bool withAudio = !req.mix.empty();
    insv::AudioMixer mixer(kAudioRate, kAudioChannels);
    mixer.setClips(req.mix);
    if (withAudio) {
        cfg.audio_sample_rate = kAudioRate;
        cfg.audio_channels = kAudioChannels;
    }
    // Let a *_vulkan encoder share the decode/stitch device, so export can
    // stay GPU-resident (FlattenWriter::vulkanEncodeAvailable / the
    // ReadFlatFrameVk path below). Unset when the machine has no Vulkan.
    AVBufferRef* sharedVkDev = insv::sharedVulkanDevice();
    cfg.vulkan_device = sharedVkDev;

    std::string err;

    // Held by pointer so a cancelled export can close (destroy) the encoder and
    // its half-written output file *before* deleting that file below.
    auto writer = std::make_unique<exportpipe::FlattenWriter>();
    if (!writer->open(cfg, err)) {
        av_buffer_unref(&sharedVkDev);
        emit finished(false, QString::fromStdString(err), req.outPath);
        return;
    }
    av_buffer_unref(&sharedVkDev); // the encoder holds its own reference now
    // Overlays: output frame k is timeline time k / fps (the audio mix's
    // clock too).
    auto insets = std::make_shared<exportpipe::InsetCompositor>(req.insets, req.outWidth,
                                                                req.outHeight);
    if (!insets->empty())
        writer->setFrameHook([insets, outFrameMs](int64_t k, uint8_t *y, int ys, uint8_t *uv,
                                                  int uvs, std::string &e) {
            return insets->composite(std::llround(k * outFrameMs), y, ys, uv, uvs, e);
        });

    bool cancelled = false;
    int64_t framesBefore = 0;
    int64_t outFrames = 0; // video frames actually written, across every clip
    std::vector<float> pcm;
    std::vector<uint8_t> black; // a flat clip's frames, NV12 (BT.709 limited range)
    for (int i = 0; i < n && !cancelled; ++i) {
        const ClipJob &c = req.clips[i];
        const double base = framesBefore * 100.0 / totalFrames;
        const double span = frames[i] * 100.0 / totalFrames;
        const QString label = tr("Rendering clip %1 of %2…").arg(i + 1).arg(n);

        auto feedAudio = [&]() -> bool {
            if (!withAudio)
                return true;
            // Audio up to the end of output frame `outFrames` on the timeline.
            const int64_t target = std::llround(outFrames * kAudioRate / req.fps);
            const int64_t need = target - writer->audioFramesWritten();
            if (need <= 0)
                return true;
            pcm.assign(static_cast<size_t>(need) * kAudioChannels, 0.0f);
            mixer.mixInto(pcm.data(), writer->audioFramesWritten(), need); // the audio tracks
            insv::AudioMixer::limit(pcm.data(), need * kAudioChannels);
            return writer->addAudio(pcm.data(), need, err);
        };

        if (c.flat) {
            // Black frames for its span; the frame hook draws the clip (one of
            // the insets) over each at its timeline time.
            black.resize(size_t(req.outWidth) * req.outHeight * 3 / 2);
            std::fill(black.begin(), black.begin() + size_t(req.outWidth) * req.outHeight, 16);
            std::fill(black.begin() + size_t(req.outWidth) * req.outHeight, black.end(), 128);
            for (int64_t k = 0; k < frames[i]; ++k) {
                if (m_cancel.load(std::memory_order_relaxed)) {
                    cancelled = true;
                    break;
                }
                ++outFrames;
                if (!writer->addNv12Frame(black.data(), req.outWidth, req.outHeight, err) ||
                    !feedAudio()) {
                    emit finished(false, QString::fromStdString(err), req.outPath);
                    return;
                }
                emitPct(base + span * static_cast<double>(k + 1) / frames[i], label);
            }
            framesBefore += frames[i];
            continue;
        }

        // Equirect intermediate size, 2:1: per clip, the full source sphere (and
        // at least 2x the output width), so the export keeps all the detail
        // the camera recorded (stitch::ExportStitchWidth).
        stitch::ClipProbe probe;
        std::string perr;
        if (!stitch::ProbeClip(c.clipPath.toStdString(), probe, perr))
            probe = stitch::ClipProbe{};
        int eqW = req.stitchWidth;
        if (eqW <= 0)
            eqW = stitch::ExportStitchWidth(probe.width, req.outWidth);
        // Conform: output frame k of this clip shows the source frame whose
        // time window [pts - half, pts + half) holds the source time of its
        // timeline time. A faster source (or speed > 1) drops frames, a
        // slower one (or slow motion) repeats them.
        const double srcHalfMs = 500.0 / probe.fps;
        auto targetSrcMs = [&](int64_t k) {
            const int64_t ms = std::llround((firstFrame[i] + k) * outFrameMs);
            return c.trimInMs + double(ms - clipStartMs[i]) * c.speed;
        };
        eqW &= ~1;
        stitch::PreviewStitchConfig sc;
        sc.width = eqW;
        sc.height = (req.stitchHeight > 0 ? req.stitchHeight : eqW / 2) & ~1;
        sc.lensGuard = c.lensGuard;
        sc.depthSeam = c.depthSeam;

        auto src = std::make_unique<stitch::NativeStitchSource>();
        if (!src->Open(c.clipPath.toStdString(), sc) || !src->SeekTo(c.trimInMs)) {
            emit finished(false, QString::fromStdString(src->GetLastError()), req.outPath);
            return;
        }
        src->SetCaptureTiming(captureTimingFor(c.stab));
        src->SetLens1Adjust(c.seamAdjustDeg.data());
        if (c.blur && !src->SetBlurQuads(c.blur)) {
            emit finished(false, tr("This stitch can't apply the privacy blur."), req.outPath);
            return;
        }

        // GPU reproject straight to NV12 -> NVENC when the all-GPU stitch path
        // is active (no CPU equirect->rectilinear pass, no RGBA->NV12 swscale).
        // Needs outW % 4 == 0 and outH % 2 == 0.
        const bool gpuFlat = src->GpuReframeAvailable() && (req.outWidth % 4) == 0 &&
                             (req.outHeight % 2) == 0;
        // And no host round-trip at all when the encoder shares the stitch
        // device: the fused view's NV12 goes straight into an encoder frame
        // on-device. Off when a frame hook needs CPU pixels (insets).
        const bool gpuVk = gpuFlat && writer->vulkanEncodeAvailable() &&
                           src->GpuViewAvailable();

        // Pose + stab for a decoded frame, at that frame's source time.
        auto poseAt = [&c](int64_t ptsMs, float pose[4], float stab[9]) {
            if (c.poseAt)
                c.poseAt(ptsMs, pose);
            if (c.stab) {
                float m[3][3];
                c.stab->correctionMatrix(ptsMs / 1000.0, m);
                std::copy(&m[0][0], &m[0][0] + 9, stab);
            }
        };

        int64_t written = 0;
        // How many of this clip's next output frames the source frame at
        // `ptsMs` stands for (0 = dropped).
        auto repeatsFor = [&](int64_t ptsMs) {
            int64_t n = 0;
            while (written + n < frames[i] && targetSrcMs(written + n) < ptsMs + srcHalfMs)
                ++n;
            return n;
        };
        while (written < frames[i]) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }

            if (gpuVk) {
                AVFrame* vkFrame = writer->acquireVulkanFrame();
                int64_t ptsMs = 0;
                if (!vkFrame ||
                    !src->ReadFlatFrameVk(req.outWidth, req.outHeight, poseAt, &c.colorGrade,
                                          vkFrame, ptsMs)) {
                    av_frame_free(&vkFrame);
                    const std::string &e = src->GetLastError();
                    if (!e.empty()) {
                        emit finished(false, QString::fromStdString(e), req.outPath);
                        return;
                    }
                    break; // clean end-of-clip
                }
                if (ptsMs + srcHalfMs < c.trimInMs) {
                    av_frame_free(&vkFrame);
                    continue; // straggler ahead of the in-point
                }
                const int64_t reps = repeatsFor(ptsMs);
                for (int64_t r = 0; r < reps; ++r) {
                    // A repeated frame (slow output / speed < 1) shares the
                    // image: every send refs the same AVVkFrame.
                    AVFrame* f = av_frame_alloc();
                    if (!f || av_frame_ref(f, vkFrame) < 0) {
                        av_frame_free(&f);
                        av_frame_free(&vkFrame);
                        emit finished(false, tr("Out of memory."), req.outPath);
                        return;
                    }
                    ++outFrames;
                    if (!writer->addVulkanFrame(f, err) || !feedAudio()) {
                        emit finished(false, QString::fromStdString(err), req.outPath);
                        return;
                    }
                }
                av_frame_free(&vkFrame); // addVulkanFrame consumed the sends
                written += reps;
            } else if (gpuFlat) {
                stitch::FlatFrame ff = src->ReadFlatFrame(req.outWidth, req.outHeight, poseAt, &c.colorGrade);
                if (ff.nv12.empty()) { // clean end-of-clip, or error
                    const std::string &e = src->GetLastError();
                    if (!e.empty()) {
                        emit finished(false, QString::fromStdString(e), req.outPath);
                        return;
                    }
                    break;
                }
                if (ff.timestamp_ms + srcHalfMs < c.trimInMs)
                    continue; // straggler ahead of the in-point
                const int64_t reps = repeatsFor(ff.timestamp_ms);
                for (int64_t r = 0; r < reps; ++r) {
                    ++outFrames;
                    if (!writer->addNv12Frame(ff.nv12.data(), ff.width, ff.height, err) ||
                        !feedAudio()) {
                        emit finished(false, QString::fromStdString(err), req.outPath);
                        return;
                    }
                }
                written += reps;
            } else {
                stitch::Frame eq = src->ReadFrame();
                if (eq.rgba.empty()) { // clean end-of-clip, or error
                    const std::string &e = src->GetLastError();
                    if (!e.empty()) {
                        emit finished(false, QString::fromStdString(e), req.outPath);
                        return;
                    }
                    break;
                }
                // NativeStitchSource::SeekTo already lands at-or-after trimInMs;
                // drop any straggler still ahead of the in-point just in case.
                if (eq.timestamp_ms + srcHalfMs < c.trimInMs)
                    continue;
                const int64_t reps = repeatsFor(eq.timestamp_ms);
                if (reps == 0)
                    continue; // dropped: skip the CPU reproject too
                float pose[4] = {0.0f, 0.0f, 0.0f, 1.309f};
                float stab[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                poseAt(eq.timestamp_ms, pose, stab);
                for (int64_t r = 0; r < reps; ++r) {
                    ++outFrames;
                    if (!writer->addEquirectFrame(eq.rgba.data(), eq.width, eq.height,
                                                  c.poseAt ? pose : nullptr,
                                                  c.stab ? stab : nullptr, &c.colorGrade, err) ||
                        !feedAudio()) {
                        emit finished(false, QString::fromStdString(err), req.outPath);
                        return;
                    }
                }
                written += reps;
            }
            if (frames[i] > 0)
                emitPct(base + span * static_cast<double>(written) / frames[i], label);
        }
        framesBefore += frames[i];
    }

    if (cancelled) {
        writer.reset(); // close the encoder + file before removing it
        std::remove(cfg.out_path.c_str());
        // Empty error string on !ok is the "cancelled by the user" signal.
        emit finished(false, QString(), req.outPath);
        return;
    }

    const int64_t total = writer->finish(err);
    if (total < 0) {
        emit finished(false, QString::fromStdString(err), req.outPath);
        return;
    }
    emitPct(100, tr("Done"));
    emit finished(true, QString(), req.outPath);
}
