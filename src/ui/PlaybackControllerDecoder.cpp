#include "PlaybackController.h"
#include "FlatClipSource.h"
#include "PlaybackTuning.h"

#include "../render/EquirectViewportWindow.h"

#include <NativeStitchSource.h> // stitch::MakeStitchSource

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

using namespace playbacktuning;

namespace {

// The next frame of `src`: the lens pair if `useLens`, else the stitched
// equirect. False at the end of the source or on an error.
bool readSourceFrame(stitch::IStitchSource &src, bool useLens, stitch::Frame &frame,
                     std::shared_ptr<render::LensFrame> &lens)
{
    if (useLens) {
        lens = std::make_shared<render::LensFrame>();
        if (src.ReadLensFrame(*lens)) {
            frame.timestamp_ms = lens->timestamp_ms;
            return true;
        }
        lens.reset();
        return false;
    }
    frame = src.ReadFrame(); // the slow part
    return !frame.rgba.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// Decoder thread
// ---------------------------------------------------------------------------

void PlaybackController::startDecoder()
{
    if (m_decoder.joinable())
        return;
    m_decoder = std::thread([this] { decoderLoop(); });
}

void PlaybackController::stopDecoder()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_decoder.joinable())
        m_decoder.join();
    m_prefetch.reset(); // waits for a job still running
    m_heldRead.reset();
}

void PlaybackController::touchSourceLru(const QString &path)
{
    m_srcLru.removeAll(path);
    m_srcLru.append(path); // most-recently-used at the back
    // Evict the coldest while over budget, never the active source.
    while (m_srcCache.size() > kMaxCachedSources && !m_srcLru.isEmpty()) {
        const QString victim = m_srcLru.front();
        if (victim == m_activePath) {
            if (m_srcLru.size() <= 1)
                break;
            m_srcLru.move(0, m_srcLru.size() - 1);
            continue;
        }
        m_srcLru.removeFirst();
        m_srcCache.erase(victim);
    }
}

std::unique_ptr<stitch::IStitchSource> PlaybackController::makeSource(const Segment &seg) const
{
    if (seg.flat)
        return std::make_unique<FlatClipSource>(seg.fps, seg.sourceDurationMs);
    return m_makeSource ? m_makeSource() : stitch::MakeStitchSource();
}

stitch::PreviewStitchConfig PlaybackController::openConfig(stitch::PreviewStitchConfig preview,
                                                           const Segment &seg)
{
    preview.lensGuard = seg.lensGuard;
    preview.depthSeam = seg.depthSeam;
    return preview;
}

std::shared_ptr<stitch::IStitchSource> PlaybackController::acquireSource(const Segment &seg,
                                                                         QString *err)
{
    const QString &path = seg.sourcePath;
    auto it = m_srcCache.find(path);
    if (it != m_srcCache.end()) {
        touchSourceLru(path);
        return it->second;
    }

    std::shared_ptr<stitch::IStitchSource> src = makeSource(seg);
    if (!src->Open(path.toStdString(), openConfig(m_decPreviewConfig, seg))) {
        if (err)
            *err = QString::fromStdString(src->GetLastError());
        return nullptr;
    }
    m_srcCache.emplace(path, src);
    touchSourceLru(path);
    return src;
}

void PlaybackController::setCaptureTimings(std::map<int, stitch::CaptureTiming> timings)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        m_rsSettings = std::move(timings);
    }
    m_rsGen.fetch_add(1);
}

void PlaybackController::setSeamAdjust(const QString &path, std::array<double, 3> deg)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        m_seamAdjust[path] = deg;
    }
    m_rsGen.fetch_add(1);
}

void PlaybackController::setBlurQuads(std::map<int, stitch::BlurQuadsFn> blurs)
{
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        m_blurQuads = std::move(blurs);
    }
    m_rsGen.fetch_add(1);
}

stitch::BlurQuadsFn PlaybackController::blurQuads(int clipId)
{
    std::lock_guard<std::mutex> lk(m_rsMx);
    const auto it = m_blurQuads.find(clipId);
    return it != m_blurQuads.end() ? it->second : stitch::BlurQuadsFn{};
}

void PlaybackController::setStabilizationProvider(StabProvider provider)
{
    std::lock_guard<std::mutex> lk(m_stabMx);
    m_stabProvider = std::move(provider);
}

void PlaybackController::setViewProvider(ViewProvider provider)
{
    std::lock_guard<std::mutex> lk(m_stabMx);
    m_viewProvider = std::move(provider);
}

void PlaybackController::setOutlineProvider(OutlineProvider provider)
{
    std::lock_guard<std::mutex> lk(m_stabMx);
    m_outlineProvider = std::move(provider);
}

void PlaybackController::applyCaptureTimingIfChanged()
{
    if (!m_activeSource)
        return;
    const uint64_t gen = m_rsGen.load();
    const int clipId = m_decPlaced[m_decSegIndex].seg.clipId;
    const ClipHandOver handOver = handOverAfter(m_decSegIndex);
    if (gen == m_decRsGen && m_activePath == m_decRsPath && clipId == m_decRsClipId &&
        handOver.untilMs == m_decRsUntilMs && handOver.afterClipId == m_decRsAfterClipId)
        return;
    applySourceSettings(*m_activeSource, m_activePath, clipId, handOver);
    m_decRsGen = gen;
    m_decRsPath = m_activePath;
    m_decRsClipId = clipId;
    m_decRsUntilMs = handOver.untilMs;
    m_decRsAfterClipId = handOver.afterClipId;
}

PlaybackController::ClipHandOver PlaybackController::handOverAfter(int index) const
{
    // Any frame from the cut on read in this segment either carries on into
    // the next one or is dropped for a seek, so the hand-over can sit at
    // the trim-out whether or not the decoder ends up carrying on.
    ClipHandOver h;
    if (index < 0 || index + 1 >= m_decPlaced.size())
        return h;
    const Segment &cur = m_decPlaced[index].seg;
    const Segment &next = m_decPlaced[index + 1].seg;
    if (next.sourcePath != cur.sourcePath)
        return h;
    h.untilMs = cur.outMs;
    h.afterClipId = next.clipId;
    return h;
}

void PlaybackController::applySourceSettings(stitch::IStitchSource &src, const QString &path,
                                             int clipId, ClipHandOver handOver)
{
    RsSetting s;
    RsSetting after;
    std::array<double, 3> seam{0, 0, 0};
    stitch::BlurQuadsFn blur, blurAfter;
    {
        std::lock_guard<std::mutex> lk(m_rsMx);
        auto it = m_rsSettings.find(clipId);
        if (it != m_rsSettings.end())
            s = it->second;
        auto at = m_rsSettings.find(handOver.afterClipId);
        if (handOver.afterClipId && at != m_rsSettings.end())
            after = at->second;
        auto st = m_seamAdjust.find(path);
        if (st != m_seamAdjust.end())
            seam = st->second;
        auto bt = m_blurQuads.find(clipId);
        if (bt != m_blurQuads.end())
            blur = bt->second;
        auto ba = m_blurQuads.find(handOver.afterClipId);
        if (handOver.afterClipId && ba != m_blurQuads.end())
            blurAfter = ba->second;
    }
    if (blurAfter || (blur && handOver.afterClipId))
        blur = [own = std::move(blur), after = std::move(blurAfter),
                untilMs = handOver.untilMs](int64_t ptsMs, std::vector<insv::BlurQuad> &out) {
            if (const stitch::BlurQuadsFn &fn = ptsMs < untilMs ? own : after)
                fn(ptsMs, out);
        };
    src.SetCaptureTiming(std::move(s), handOver.untilMs, std::move(after));
    src.SetLens1Adjust(seam.data());
    src.SetBlurQuads(std::move(blur));
}

void PlaybackController::setHardStall(bool on)
{
    std::lock_guard<std::mutex> lk(m_mx);
    m_hardStall = on;
}

void PlaybackController::pruneSourcesToTimeline()
{
    for (auto it = m_srcCache.begin(); it != m_srcCache.end();) {
        const bool referenced =
            std::any_of(m_decPlaced.begin(), m_decPlaced.end(),
                        [&](const Placed &p) { return p.seg.sourcePath == it->first; });
        if (referenced) {
            ++it;
        } else {
            m_srcLru.removeAll(it->first);
            it = m_srcCache.erase(it);
        }
    }
}

bool PlaybackController::switchToSegment(int index)
{
    const QString path = m_decPlaced[index].seg.sourcePath;
    if (m_activeSource && m_activePath == path) {
        touchSourceLru(path);
        return true;
    }

    QString err;
    auto src = acquireSource(m_decPlaced[index].seg, &err);
    if (!src) {
        emit openFailed(err.isEmpty() ? QStringLiteral("could not open %1").arg(path) : err);
        return false;
    }
    // The source we leave keeps its lens ring only for frames still queued or
    // on screen; drop the rest of its GPU memory.
    if (m_activeSource)
        m_activeSource->TrimLensFrames();
    m_activeSource = std::move(src);
    m_activePath = path;
    return true;
}

void PlaybackController::startPrefetch(const Placed &cur, qint64 timelineMs)
{
    if (m_prefetch) {
        if (!m_prefetch->stale)
            return; // already under way for this boundary
        if (m_prefetch->done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return; // one job at a time
        m_prefetch.reset();
    }
    const int next = m_decSegIndex + 1;
    if (next >= m_decPlaced.size() || cur.endMs() - timelineMs > kBoundaryPrefetchMs)
        return;
    const Placed &np = m_decPlaced[next];
    // The same file on from here (a split): the decoder carries on reading.
    if (np.seg.sourcePath == cur.seg.sourcePath && np.seg.inMs == cur.seg.outMs)
        return;

    auto p = std::make_unique<Prefetch>();
    p->segIndex = next;
    p->path = np.seg.sourcePath;
    // A cached source other than the active one is the job's until taken; a
    // jump within the active file needs a second one (opened by the job).
    if (p->path != m_activePath) {
        auto it = m_srcCache.find(p->path);
        if (it != m_srcCache.end()) {
            p->src = std::move(it->second);
            m_srcCache.erase(it);
            m_srcLru.removeAll(p->path);
        }
    }
    const int clipId = np.seg.clipId;
    const ClipHandOver handOver = handOverAfter(next);
    if (p->src)
        applySourceSettings(*p->src, p->path, clipId, handOver);
    const bool viewportLens = m_viewport && m_viewport->LensFramesSupported();
    Prefetch *job = p.get();
    p->done = std::async(std::launch::async,
                         [this, job, viewportLens, clipId, handOver, seg = np.seg,
                          inMs = np.seg.inMs, config = openConfig(m_decPreviewConfig, np.seg)] {
                             if (!job->src) {
                                 auto src = makeSource(seg);
                                 if (!src->Open(job->path.toStdString(), config))
                                     return; // the boundary opens it again and reports why
                                 applySourceSettings(*src, job->path, clipId, handOver);
                                 job->src = std::move(src);
                             }
                             job->src->SeekTo(inMs);
                             job->got = readSourceFrame(*job->src,
                                                        viewportLens && job->src->LensFramesAvailable(),
                                                        job->frame, job->lens);
                             job->opened = true;
                         });
    m_prefetch = std::move(p);
}

bool PlaybackController::takePrefetch(int next)
{
    if (!m_prefetch || m_prefetch->stale || m_prefetch->segIndex != next)
        return false;
    setHardStall(true);      // cleared by the next read, as after a SeekTo()
    m_prefetch->done.wait(); // normally done long ago
    std::unique_ptr<Prefetch> p = std::move(m_prefetch);
    if (!p->opened)
        return false;
    if (m_activeSource)
        m_activeSource->TrimLensFrames(); // as switchToSegment
    m_srcCache[p->path] = p->src; // a jump within the file replaces the active one
    m_activeSource = std::move(p->src);
    m_activePath = p->path;
    touchSourceLru(m_activePath);
    m_decRsGen = ~uint64_t(0); // re-apply: settings may have changed since
    m_heldRead = HeldRead{std::move(p->frame), std::move(p->lens), p->got};
    return true;
}

void PlaybackController::invalidatePrefetch()
{
    if (m_prefetch)
        m_prefetch->stale = true;
    m_heldRead.reset();
}

void PlaybackController::decoderLoop()
{
    for (;;) {
        bool doSeek = false;
        bool tlChanged = false;
        bool cfgChanged = false;
        QStringList reopen; // m_reopenPaths, taken with the timeline
        qint64 seekMs = 0;
        bool seekToKeyframe = false;
        uint64_t gen = 0;
        bool playing = false;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this] {
                const size_t cap = !m_playing   ? kScrubLookahead
                                   : m_lensQueue ? kLensQueueMax
                                                 : queueCapFor(m_frameBytes);
                const bool canDecode =
                    !m_decPlaced.isEmpty() && !m_decoderEof && m_queue.size() < cap;
                return m_stop || m_seekPending || m_timelineDirty || m_previewConfigDirty ||
                       canDecode;
            });
            if (m_stop)
                return;
            if (m_timelineDirty) {
                // A file whose stitch settings changed is opened afresh, as
                // one asked for by reopenSources.
                reopen = std::exchange(m_reopenPaths, {});
                for (const Placed &now : m_placed)
                    for (const Placed &was : m_decPlaced)
                        if (was.seg.sourcePath == now.seg.sourcePath &&
                            (was.seg.lensGuard != now.seg.lensGuard ||
                             was.seg.depthSeam != now.seg.depthSeam) &&
                            !reopen.contains(now.seg.sourcePath)) {
                            reopen << now.seg.sourcePath;
                            break;
                        }
                m_decPlaced = m_placed;
                m_timelineDirty = false;
                tlChanged = true;
            }
            if (m_previewConfigDirty) {
                m_decPreviewConfig = m_pendingPreviewConfig;
                m_previewConfigDirty = false;
                cfgChanged = true;
            }
            if (m_seekPending) {
                m_seekPending = false;
                doSeek = true;
                seekMs = m_seekTargetMs;
                seekToKeyframe = m_seekToKeyframe;
                m_hardStall = true; // hold the presenter clock across the seek
            }
            gen = m_seekGen;
            playing = m_playing;
        }
        if (tlChanged || cfgChanged || doSeek)
            invalidatePrefetch();

        if (tlChanged) {
            pruneSourcesToTimeline(); // drop sources no longer on the timeline
            for (const QString &p : reopen) {
                m_srcCache.erase(p);
                m_srcLru.removeAll(p);
                if (m_activePath == p) {
                    m_activeSource.reset();
                    m_activePath.clear();
                    m_decSegIndex = -1;
                }
            }
            if (m_decPlaced.isEmpty()) {
                m_activeSource.reset();
                m_activePath.clear();
                m_decSegIndex = -1;
            }
        }

        if (cfgChanged) {
            // Every cached source was opened with the old settings. Drop them
            // all so the next switchToSegment re-opens under the new config;
            // the pending seek (from setPreviewConfig) then re-stitches the
            // current frame.
            m_srcCache.clear();
            m_srcLru.clear();
            m_activeSource.reset();
            m_activePath.clear();
            m_decSegIndex = -1;
        }

        if (m_decPlaced.isEmpty())
            continue; // nothing to play; wait for the next timeline/seek/stop

        if (doSeek) {
            const int idx = segmentAt(m_decPlaced, seekMs);
            const Placed &ps = m_decPlaced[idx];
            const qint64 clamped = std::clamp<qint64>(seekMs, ps.startMs, ps.endMs());
            if (!switchToSegment(idx)) {
                setHardStall(false);
                std::lock_guard<std::mutex> lk(m_mx);
                m_decoderEof = true;
                continue;
            }
            // At most the clip's last frame: a seek to its end (or to the last
            // frame's own time, a hair past it once rounded) decoded the frame
            // after the trim-out, read as the end -- and the old picture
            // stayed on screen.
            const double fps = m_activeSource->GetInfo().fps;
            const qint64 frameMs = std::llround(1000.0 / fps);
            const qint64 sourceMs = std::max(
                ps.seg.inMs, std::min(ps.seg.sourceAtLocalMs(clamped - ps.startMs), ps.seg.outMs - frameMs));
            qint64 target = sourceMs;
            if (seekToKeyframe) {
                // Scrubbing: the exact frame when it decodes about as fast as a
                // keyframe -- a little after its own keyframe, or a little
                // ahead of the decoder (the source's SeekTo decodes forward to
                // it). Else a keyframe on the way from where the decoder is,
                // which decodes in one step: a stop on the way, never past the
                // target, so the picture only ever moves the way the pointer
                // went. With none on the way, the exact frame after all --
                // scrubs arriving meanwhile merge into the next one. Keyframes
                // only inside this clip's trim.
                int64_t before = -1, after = -1;
                m_activeSource->KeyframesAround(sourceMs, before, after);
                const bool sameSeg = idx == m_decSegIndex && m_decLastSourceMs >= 0;
                const qint64 at = m_decLastSourceMs; // ~the frame on screen + lookahead
                const bool cheap =
                    (before >= 0 && sourceMs - before <= kScrubDecodeAheadMs) ||
                    (sameSeg && sourceMs > at && sourceMs - at <= kScrubDecodeAheadMs);
                qint64 kf = -1;
                if (!cheap && !sameSeg) // nothing on screen to keep in step with
                    kf = m_activeSource->NearestKeyframeMs(sourceMs);
                else if (!cheap && sourceMs > at && before > at)
                    kf = before;
                else if (!cheap && sourceMs < at && after >= 0 && after < at - kScrubHoldMs)
                    kf = after;
                if (kf >= ps.seg.inMs && kf < ps.seg.outMs)
                    target = kf;
            }
            m_activeSource->SeekTo(target); // m_hardStall stays until the first read
            m_decLastSourceMs = -1;
            m_decSegIndex = idx;
            std::lock_guard<std::mutex> lk(m_mx);
            m_decoderEof = false;
            continue;
        }

        if (m_decSegIndex < 0 || m_decSegIndex >= m_decPlaced.size()) {
            // No valid position yet -- seek to the current clamped target.
            std::lock_guard<std::mutex> lk(m_mx);
            m_seekPending = true;
            m_seekTargetMs = seekMs;
            continue;
        }

        Placed cur = m_decPlaced[m_decSegIndex]; // small copy; released lock

        applyCaptureTimingIfChanged();
        // Lens frames when the viewport can take them: the decoded pair, drawn
        // from the lenses at present time. Else the stitched equirect.
        const bool useLens = m_viewport && m_viewport->LensFramesSupported() &&
                             m_activeSource->LensFramesAvailable();
        const auto t0 = std::chrono::steady_clock::now();
        stitch::Frame frame;
        std::shared_ptr<render::LensFrame> lens;
        bool got = false;
        if (m_heldRead) {
            frame = std::move(m_heldRead->frame);
            lens = std::move(m_heldRead->lens);
            got = m_heldRead->got;
            m_heldRead.reset();
        } else {
            got = readSourceFrame(*m_activeSource, useLens, frame, lens);
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (render::framestats::Enabled() && got) {
            std::lock_guard<std::mutex> lk(m_mx);
            m_statsReadMs.add(render::framestats::MsBetween(t0, t1));
        }
        // SeekTo() only positions the decoder; this first read after it does
        // the decoding up to the target, the slow part of a seek or boundary.
        setHardStall(false);

        const bool sourceEnded = !got;
        const bool pastTrimOut = !sourceEnded && frame.timestamp_ms >= cur.seg.outMs;
        if (sourceEnded || pastTrimOut) {
            const int next = m_decSegIndex + 1;
            if (next >= m_decPlaced.size()) {
                std::lock_guard<std::mutex> lk(m_mx);
                m_decoderEof = true;
                continue;
            }
            // The next clip carries on in this file from this very frame (a
            // split): it's the frame SeekTo(trim-in) would give, so skip the
            // seek and its decode. The source was told to hand over to this
            // clip's capture timing at the cut (handOverAfter), so the frame
            // was stitched with the right one.
            const Placed &np = m_decPlaced[next];
            const bool carriesOn = pastTrimOut && np.seg.sourcePath == m_activePath &&
                                   m_decLastSourceMs >= 0 && m_decLastSourceMs < np.seg.inMs &&
                                   np.seg.inMs <= frame.timestamp_ms &&
                                   frame.timestamp_ms < np.seg.outMs;
            if (carriesOn) {
                m_decSegIndex = next;
                cur = np;
            } else if (takePrefetch(next)) {
                // Its first frame is read already: the next read returns it.
                m_decLastSourceMs = -1;
                m_decSegIndex = next;
                continue;
            } else if (!switchToSegment(next)) {
                std::lock_guard<std::mutex> lk(m_mx);
                m_decoderEof = true;
                continue;
            } else {
                // Until the next clip's first frame is read: its seek and
                // decode up to the trim-in.
                setHardStall(true);
                m_activeSource->SeekTo(m_decPlaced[next].seg.inMs);
                m_decLastSourceMs = -1;
                m_decSegIndex = next;
                continue;
            }
        }

        // Rewrite the frame's timestamp from source time to timeline time.
        const qint64 sourceMs = frame.timestamp_ms;
        m_decLastSourceMs = sourceMs;
        frame.timestamp_ms = cur.startMs + cur.seg.localAtSourceMs(frame.timestamp_ms);
        if (playing)
            startPrefetch(cur, frame.timestamp_ms);

        {
            std::lock_guard<std::mutex> lk(m_mx);
            // A stale frame from before a seek: dropped unless a scrub is in
            // flight, where any recent picture beats a frozen one.
            if (m_seekGen != gen && (m_playing || !m_seekToKeyframe))
                continue;
            if (!m_playing) {
                while (m_queue.size() >= kScrubLookahead)
                    m_queue.pop_front();
            }
            m_lensQueue = lens != nullptr;
            if (!lens)
                m_frameBytes = frame.rgba.size();
            m_queue.push_back({std::move(frame), std::move(lens), gen, cur.seg.sourcePath, sourceMs,
                               cur.seg.clipId});
        }
    }
}
