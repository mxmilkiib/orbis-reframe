#include "RenderJobs.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>


#include "ClipViewSource.h"
#include "PrivacyBlur.h"
#include "TitleRender.h"
#include "../core/ClipItem.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"

namespace renderjobs {

namespace {

// A title's largest text size, over its keys too.
float maxTitleSize(const TrackClip &c)
{
    float size = c.look.width;
    for (const InsetKey &k : c.insetKeys.keys())
        size = std::max(size, k.look.width);
    return size;
}

// Playback renders a title's text no bigger than this (the view is mostly
// smaller than the output); the export at the size it shows at, up to the
// second.
constexpr int kViewFontCapPx = 360;
constexpr int kExportFontCapPx = 2048;

// A 360 clip's view as a layer's pictures (ClipViewSource): what the export
// renders it with, its stabilizer built when the source opens.
ClipViewSource::Spec viewSpec(const ClipItem &c, const Timeline &timeline,
                              const PrivacyBlur &privacy)
{
    ClipViewSource::Spec spec;
    ReframeExportController::ClipJob &job = spec.job;
    job.clipPath = c.sourcePath;
    job.colorGrade = c.color.render();
    job.blur = privacy.blurFor(c);
    job.lensGuard = c.lensGuard;
    job.depthSeam = c.depthSeam;
    timeline.sourceSeamAdjust(c.sourcePath, job.seamAdjustDeg);
    if (!c.reframe.isEmpty())
        job.poseAt = [track = c.reframe](qint64 sourceMs, float pose[4]) {
            const ReframePose p = track.sample(sourceMs);
            pose[0] = p.yaw;
            pose[1] = p.pitch;
            pose[2] = p.roll;
            pose[3] = p.fovY;
        };
    spec.buildStab = c.stabilization;
    spec.fps = c.fps;
    spec.syncMs = std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs;
    return spec;
}

// Everything a 360 clip's view depends on (viewSpec), as a string: the
// layer's InsetFeed::Clip::pictureKey.
QString viewKey(const ClipItem &c, const Timeline &timeline)
{
    std::array<double, 3> seam{};
    timeline.sourceSeamAdjust(c.sourcePath, seam);
    const insv::StabConfig &s = c.stabilization;
    QString key = QStringLiteral("%1|%2|%3,%4,%5,%6|%7,%8|%9,%10,%11")
                      .arg(c.id)
                      .arg(c.sourcePath)
                      .arg(c.color.exposureEv)
                      .arg(c.color.contrast)
                      .arg(c.color.saturation)
                      .arg(c.color.temperature)
                      .arg(int(c.lensGuard))
                      .arg(c.depthSeam)
                      .arg(seam[0])
                      .arg(seam[1])
                      .arg(seam[2]);
    key += QStringLiteral("|%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11")
               .arg(int(s.mode))
               .arg(s.windowYawSec)
               .arg(s.windowPitchSec)
               .arg(s.windowRollSec)
               .arg(s.horizonLockAmount)
               .arg(s.horizonGain)
               .arg(s.accelLowPassHz)
               .arg(s.autoLevel)
               .arg(s.pitchOffsetRad)
               .arg(s.rollOffsetRad)
               .arg(std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs);
    key += QStringLiteral("|%1,%2,%3,%4").arg(c.blurPlates).arg(c.plateScan)
               .arg(c.blurRemovals.size()).arg(c.blurAdditions.size());
    for (const ReframeKey &k : c.reframe.keys())
        key += QStringLiteral("|%1:%2,%3,%4,%5,%6")
                   .arg(k.timeMs)
                   .arg(k.pose.yaw)
                   .arg(k.pose.pitch)
                   .arg(k.pose.roll)
                   .arg(k.pose.fovY)
                   .arg(int(k.ease));
    return key;
}

// A dip to black's picture: a few opaque black pixels (premultiplied RGBA),
// stretched over the frame. One, shared: playback uploads it once.
constexpr int kBlackPx = 4;
std::shared_ptr<const std::vector<uint8_t>> blackPicture()
{
    static const auto black = [] {
        auto px = std::make_shared<std::vector<uint8_t>>(kBlackPx * kBlackPx * 4, 0);
        for (size_t i = 3; i < px->size(); i += 4)
            (*px)[i] = 255;
        return std::shared_ptr<const std::vector<uint8_t>>(std::move(px));
    }();
    return black;
}

} // namespace

std::vector<TransitionLayer> transitionLayers(const Timeline &timeline)
{
    std::vector<TransitionLayer> layers;
    for (int i = 0; i + 1 < timeline.count(); ++i) {
        const ClipItem &a = timeline.clipAt(i);
        const ClipItem &b = timeline.clipAt(i + 1);
        const Transition *t = timeline.transitionAfter(a.id);
        if (!t)
            continue;
        const Timeline::TransitionSpan span = timeline.transitionSpan(*t);
        if (!span.plays())
            continue;
        const qint64 half = span.durationMs / 2;
        if (!t->usesHandles()) { // black, in and out again
            TransitionLayer in;
            in.id = -2 * a.id;
            in.overClipId = a.id;
            in.startMs = span.cutMs - half;
            in.trimOutMs = half;
            in.rampFrom = 0.0f;
            in.rampTo = 1.0f;
            TransitionLayer out;
            out.id = -2 * a.id - 1;
            out.overClipId = b.id;
            out.startMs = span.cutMs;
            out.trimOutMs = half;
            out.rampFrom = 1.0f;
            out.rampTo = 0.0f;
            layers.push_back(in);
            layers.push_back(out);
            continue;
        }
        TransitionLayer in; // b, blending in up to the cut
        in.id = -2 * a.id;
        in.clipIndex = i + 1;
        in.overClipId = a.id;
        in.startMs = span.cutMs - half;
        in.trimInMs = b.trimInMs - std::llround(half * b.speed);
        in.trimOutMs = b.trimInMs;
        in.rampFrom = 0.0f;
        in.rampTo = 0.5f;
        TransitionLayer out; // a, blending out after it
        out.id = -2 * a.id - 1;
        out.clipIndex = i;
        out.overClipId = b.id;
        out.startMs = span.cutMs;
        out.trimInMs = a.trimOutMs;
        out.trimOutMs = a.trimOutMs + std::llround(half * a.speed);
        out.rampFrom = 0.5f;
        out.rampTo = 0.0f;
        layers.push_back(in);
        layers.push_back(out);
    }
    return layers;
}

std::vector<insv::MixClip> mixClips(const Timeline &timeline)
{
    constexpr int kFramesPerMs = 48; // playback and export both mix at 48 kHz
    // Each transition's half span, by the clip it runs on past (its end at
    // the cut) and by the one it starts early (its start at the cut).
    struct Cross {
        qint64 cutMs = 0;
        qint64 half = 0;
        bool handles = true; // a crossfade's: past the cut; a dip's: up to it
    };
    std::map<int, Cross> pastEnd, beforeStart;
    for (const Transition &t : timeline.transitions()) {
        const Timeline::TransitionSpan span = timeline.transitionSpan(t);
        if (span.plays()) {
            pastEnd[t.leftClipId] = {span.cutMs, span.durationMs / 2, t.usesHandles()};
            beforeStart[t.rightClipId] = {span.cutMs, span.durationMs / 2, t.usesHandles()};
        }
    }
    std::vector<insv::MixClip> mix;
    for (int i = 0; i < timeline.trackCount(); ++i) {
        const Track &t = timeline.trackAt(i);
        if (t.muted || t.kind != Track::Kind::Audio)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.muted || c.mediaMissing)
                continue;
            insv::MixClip m;
            m.path = c.sourcePath.toStdString();
            m.stream = c.stream;
            m.startFrame = c.startMs * kFramesPerMs;
            m.lengthFrames = c.timelineDurationMs() * kFramesPerMs;
            m.inFrame = c.trimInMs * kFramesPerMs;
            m.speed = c.speed;
            m.gain = static_cast<float>(std::pow(10.0, (c.gainDb + t.gainDb) / 20.0));
            m.fadeInFrames = c.fadeInMs * kFramesPerMs;
            m.fadeOutFrames = c.fadeOutMs * kFramesPerMs;
            // A clip's own sound at a transition, its edge on the cut (a
            // couple of ms of rounding either way). A crossfade's: on past it
            // / from before it, fading over the whole transition. A dip's:
            // fading out up to it / in from it, over half (or the clip's own
            // fade, if longer).
            constexpr qint64 kOnCutMs = 2;
            if (const auto x = pastEnd.find(c.linkedClipId);
                x != pastEnd.end() && std::abs(c.endMs() - x->second.cutMs) <= kOnCutMs) {
                const qint64 half = x->second.half;
                if (x->second.handles) {
                    m.lengthFrames += half * kFramesPerMs;
                    m.fadeOutFrames = 2 * half * kFramesPerMs;
                } else {
                    m.fadeOutFrames = std::max<int64_t>(m.fadeOutFrames, half * kFramesPerMs);
                }
            }
            if (const auto x = beforeStart.find(c.linkedClipId);
                x != beforeStart.end() && std::abs(c.startMs - x->second.cutMs) <= kOnCutMs) {
                const qint64 half = x->second.half;
                if (x->second.handles) {
                    m.startFrame -= half * kFramesPerMs;
                    m.inFrame -= std::llround(half * c.speed) * kFramesPerMs;
                    m.lengthFrames += half * kFramesPerMs;
                    m.fadeInFrames = 2 * half * kFramesPerMs;
                } else {
                    m.fadeInFrames = std::max<int64_t>(m.fadeInFrames, half * kFramesPerMs);
                }
            }
            m.noise = c.noise;
            mix.push_back(std::move(m));
        }
    }
    return mix;
}

std::vector<InsetFeed::Clip> insetFeed(const Timeline &timeline, int outW, int outH,
                                       const PrivacyBlur &privacy)
{
    std::vector<InsetFeed::Clip> clips;
    for (int i = 0; i < timeline.count(); ++i) {
        const ClipItem &c = timeline.clipAt(i);
        if (!c.flat || c.mediaMissing)
            continue;
        InsetFeed::Clip f;
        f.id = c.id;
        f.path = c.sourcePath;
        f.startMs = timeline.clipToTimeline(i, 0);
        f.trimInMs = c.trimInMs;
        f.trimOutMs = c.trimOutMs;
        f.speed = c.speed;
        f.aspect = c.sourceHeight > 0 ? double(c.sourceWidth) / c.sourceHeight : 16.0 / 9.0;
        f.look = inset::fitted(f.aspect, outW, outH, c.fit == ClipItem::Fit::Fill);
        f.fullFrame = true;
        f.colorGrade = c.color.render();
        clips.push_back(std::move(f));
    }
    // The transitions' other clips and black, over the main track, under the
    // overlays.
    for (const TransitionLayer &l : transitionLayers(timeline)) {
        if (l.clipIndex < 0) {
            if (outW <= 0 || outH <= 0)
                continue;
            InsetFeed::Clip f;
            f.id = l.id;
            f.startMs = l.startMs;
            f.trimInMs = l.trimInMs;
            f.trimOutMs = l.trimOutMs;
            f.rampFrom = l.rampFrom;
            f.rampTo = l.rampTo;
            f.overMainClipId = l.overClipId;
            f.fullFrame = true;
            f.aspect = double(outW) / outH;
            f.look = inset::fitted(f.aspect, outW, outH, false);
            f.still = blackPicture();
            f.stillW = f.stillH = kBlackPx;
            f.stillWidthPerSize = 1.0;
            clips.push_back(std::move(f));
            continue;
        }
        const ClipItem &c = timeline.clipAt(l.clipIndex);
        if (c.mediaMissing)
            continue;
        InsetFeed::Clip f;
        f.id = l.id;
        f.path = c.sourcePath;
        f.startMs = l.startMs;
        f.trimInMs = l.trimInMs;
        f.trimOutMs = l.trimOutMs;
        f.speed = c.speed;
        f.rampFrom = l.rampFrom;
        f.rampTo = l.rampTo;
        f.overMainClipId = l.overClipId;
        f.fullFrame = true;
        if (c.flat) {
            f.aspect = c.sourceHeight > 0 ? double(c.sourceWidth) / c.sourceHeight : 16.0 / 9.0;
            f.look = inset::fitted(f.aspect, outW, outH, c.fit == ClipItem::Fit::Fill);
            f.colorGrade = c.color.render();
        } else if (outW > 0 && outH > 0) {
            f.aspect = double(outW) / outH;
            f.look = inset::fitted(f.aspect, outW, outH, false);
            f.picture = [spec = viewSpec(c, timeline, privacy)] {
                return std::make_unique<ClipViewSource>(spec);
            };
            f.pictureKey = viewKey(c, timeline);
        } else {
            continue;
        }
        clips.push_back(std::move(f));
    }
    // Bottom-most first: the lowest overlay row draws first, the top row last.
    for (int i = timeline.trackCount() - 1; i >= 0; --i) {
        const Track &t = timeline.trackAt(i);
        if (t.kind != Track::Kind::Video || t.muted)
            continue; // (hidden)
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            titlerender::Picture pic;
            if (c.isTitle()) {
                pic = titlerender::render(
                    *c.title, titlerender::fontPxFor(maxTitleSize(c), outH, kViewFontCapPx));
                if (pic.empty() || outW <= 0)
                    continue; // no text
            }
            InsetFeed::Clip f;
            f.id = c.id;
            f.path = c.sourcePath;
            f.startMs = c.startMs;
            f.trimInMs = c.trimInMs;
            f.trimOutMs = c.trimOutMs;
            f.speed = c.speed;
            f.aspect = c.aspect();
            f.fadeInMs = c.fadeInMs;
            f.fadeOutMs = c.fadeOutMs;
            f.look = c.look;
            f.frame = c.frame;
            f.keys = c.insetKeys;
            if (!pic.empty()) {
                f.still = pic.rgba;
                f.stillW = pic.width;
                f.stillH = pic.height;
                f.stillWidthPerSize = pic.widthPerFontPx * outH / outW;
                f.aspect = double(pic.width) / pic.height;
                f.frame = {};
            }
            clips.push_back(std::move(f));
        }
    }
    return clips;
}

std::vector<exportpipe::InsetClipJob> insetJobs(const Timeline &timeline, int outW, int outH,
                                                const PrivacyBlur &privacy)
{
    std::vector<exportpipe::InsetClipJob> jobs;
    // The flat main-track clips first, then the transitions' other clips,
    // under the overlays -- as insetFeed.
    for (const InsetFeed::Clip &f : insetFeed(timeline, outW, outH, privacy)) {
        if (!f.fullFrame)
            break; // the overlays: below
        exportpipe::InsetClipJob j;
        j.path = f.path.toStdString();
        j.startMs = f.startMs;
        j.trimInMs = f.trimInMs;
        j.trimOutMs = f.trimOutMs;
        j.speed = f.speed;
        j.aspect = f.aspect;
        j.colorGrade = f.colorGrade;
        j.rampFrom = f.rampFrom;
        j.rampTo = f.rampTo;
        j.picture = f.picture;
        j.still = f.still; // a dip's black
        j.stillW = f.stillW;
        j.stillH = f.stillH;
        j.lookAt = [look = f.look](int64_t) { return look; };
        jobs.push_back(std::move(j));
    }
    for (int i = timeline.trackCount() - 1; i >= 0; --i) {
        const Track &t = timeline.trackAt(i);
        if (t.kind != Track::Kind::Video || t.muted)
            continue;
        for (const TrackClip &c : t.clips) {
            if (c.mediaMissing)
                continue;
            titlerender::Picture pic;
            if (c.isTitle()) {
                pic = titlerender::render(
                    *c.title, titlerender::fontPxFor(maxTitleSize(c), outH, kExportFontCapPx));
                if (pic.empty() || outW <= 0)
                    continue; // no text
            }
            exportpipe::InsetClipJob j;
            j.path = c.sourcePath.toStdString();
            j.startMs = c.startMs;
            j.trimInMs = c.trimInMs;
            j.trimOutMs = c.trimOutMs;
            j.speed = c.speed;
            j.aspect = c.aspect();
            j.fadeInMs = c.fadeInMs;
            j.fadeOutMs = c.fadeOutMs;
            j.frame = c.frame;
            // A copy: the export thread samples it.
            j.lookAt = [look = c.look, keys = c.insetKeys](int64_t sourceMs) {
                return keys.isEmpty() ? look : keys.sample(sourceMs);
            };
            if (!pic.empty()) {
                // The look's width is the text size: the picture's is that
                // much wider.
                const double widthPerSize = pic.widthPerFontPx * outH / outW;
                j.lookAt = [look = c.look, keys = c.insetKeys, widthPerSize](int64_t sourceMs) {
                    inset::Look l = keys.isEmpty() ? look : keys.sample(sourceMs);
                    l.width = float(l.width * widthPerSize);
                    return l;
                };
                j.still = pic.rgba;
                j.stillW = pic.width;
                j.stillH = pic.height;
                j.aspect = double(pic.width) / pic.height;
                j.frame = {};
            }
            jobs.push_back(std::move(j));
        }
    }
    return jobs;
}

ReframeExportController::ClipJob clipJob(const ClipItem &c, const Timeline &timeline,
                                         const PrivacyBlur &privacy)
{
    // Stabilization (Lean follow / Horizon Lock / Custom) is a per-frame
    // sphere-frame matrix next to the reframe pose, applied after it like the
    // viewport's pc.stab -- the native stitch itself is never stabilized.
    const bool nativeStab = c.stabilization.mode != insv::StabMode::Off;
    ReframeExportController::ClipJob job;
    job.flat = c.flat;
    job.clipPath = c.sourcePath;
    job.trimInMs = c.trimInMs;
    job.trimOutMs = c.trimOutMs;
    job.speed = c.speed;
    job.colorGrade = c.color.render();
    job.blur = privacy.blurFor(c);
    job.lensGuard = c.lensGuard;
    job.depthSeam = c.depthSeam;
    timeline.sourceSeamAdjust(c.sourcePath, job.seamAdjustDeg);
    if (nativeStab)
        job.stab = ReframeExportController::buildStabilizer(
            c.sourcePath, c.fps, c.stabilization, std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs);
    // No keyframes -> a static forward view.
    if (!c.reframe.isEmpty())
        job.poseAt = [track = c.reframe](qint64 sourceMs, float pose[4]) {
            const ReframePose p = track.sample(sourceMs);
            pose[0] = p.yaw;
            pose[1] = p.pitch;
            pose[2] = p.roll;
            pose[3] = p.fovY;
        };
    return job;
}

QStringList missingSources(const Timeline &timeline)
{
    QStringList out;
    const auto add = [&out](const QString &path) {
        if (!out.contains(path))
            out << path;
    };
    for (int i = 0; i < timeline.count(); ++i)
        if (const ClipItem &c = timeline.clipAt(i); c.mediaMissing)
            add(c.sourcePath);
    for (int i = 0; i < timeline.trackCount(); ++i) {
        const Track &t = timeline.trackAt(i);
        if (t.muted)
            continue; // a hidden video track, a muted audio track
        for (const TrackClip &c : t.clips)
            if (c.mediaMissing && !c.isTitle() && !(t.kind == Track::Kind::Audio && c.muted))
                add(c.sourcePath);
    }
    return out;
}

QString suggestedExportPath(const QString &projectFile)
{
    if (projectFile.isEmpty())
        return QDir::homePath() + QStringLiteral("/timeline.mp4");
    QString dir = projectfolder::ensure(projectfolder::exportsDir(projectFile));
    if (dir.isEmpty())
        dir = QFileInfo(projectFile).absolutePath();
    const QString base = QFileInfo(projectFile).completeBaseName();
    QString suggested = QDir(dir).filePath(base + QStringLiteral(".mp4"));
    for (int n = 2; QFileInfo::exists(suggested); ++n)
        suggested = QDir(dir).filePath(QStringLiteral("%1-%2.mp4").arg(base).arg(n));
    return suggested;
}

QString suggestedScreenshotPath(const QString &projectFile, const QString &clipSource,
                                qint64 timelineMs)
{
    QString dir;
    QString base;
    if (!projectFile.isEmpty()) {
        const QFileInfo fi(projectFile);
        dir = projectfolder::ensure(projectfolder::screenshotsDir(projectFile));
        if (dir.isEmpty())
            dir = fi.absolutePath();
        base = fi.completeBaseName();
    } else {
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (dir.isEmpty() || !QDir(dir).exists())
            dir = QDir::homePath();
        base = QFileInfo(clipSource).completeBaseName();
    }
    const qint64 t = std::max<qint64>(0, timelineMs);
    const QString stamp = QStringLiteral("%1m%2s%3")
                              .arg(t / 60000, 2, 10, QLatin1Char('0'))
                              .arg(t / 1000 % 60, 2, 10, QLatin1Char('0'))
                              .arg(t % 1000, 3, 10, QLatin1Char('0'));
    QString suggested = QDir(dir).filePath(QStringLiteral("%1_%2.png").arg(base, stamp));
    for (int n = 2; QFileInfo::exists(suggested); ++n)
        suggested = QDir(dir).filePath(QStringLiteral("%1_%2-%3.png").arg(base, stamp).arg(n));
    return suggested;
}

} // namespace renderjobs
