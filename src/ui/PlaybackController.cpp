#include "PlaybackController.h"
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


PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
}

PlaybackController::~PlaybackController()
{
    stopDecoder();
}

// ---------------------------------------------------------------------------
// Timeline math
// ---------------------------------------------------------------------------

QList<PlaybackController::Placed> PlaybackController::placeSegments(const QList<Segment> &segments)
{
    QList<Placed> placed;
    qint64 acc = 0;
    for (const Segment &s : segments) {
        Placed p;
        p.seg = s;
        p.startMs = acc;
        acc += p.durationMs();
        placed.push_back(p);
    }
    return placed;
}

int PlaybackController::segmentAt(const QList<Placed> &placed, qint64 timelineMs)
{
    for (int i = 0; i < placed.size(); ++i) {
        if (timelineMs < placed[i].endMs())
            return i;
    }
    return placed.size() - 1; // past the end, or -1 if empty
}

// ---------------------------------------------------------------------------
// GUI-thread entry points
// ---------------------------------------------------------------------------

void PlaybackController::setTimeline(const QList<Segment> &segments)
{
    QList<Placed> placed = placeSegments(segments);
    m_totalMs = placed.isEmpty() ? 0 : placed.back().endMs();
    m_haveTimeline = !placed.isEmpty();
    m_positionMs = clampTimeline(m_positionMs);
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_presentAfterSeek = true;
    m_reachedEnd = false;

    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_placed = std::move(placed);
        m_timelineDirty = true;
        m_decoderEof = false;
        requestSeekLocked(m_positionMs);
    }
    m_cv.notify_all();

    if (m_haveTimeline) {
        if (!m_timer) {
            m_timer = new QTimer(this);
            m_timer->setTimerType(Qt::PreciseTimer);
            connect(m_timer, &QTimer::timeout, this, &PlaybackController::tick);
        }
        if (!m_timer->isActive())
            m_timer->start(kPresentIntervalMs);
        startDecoder();
    } else if (m_viewport) {
        // Nothing to present any more: don't leave the last project's
        // picture up.
        m_viewport->ClearPicture();
    }

    emit durationChanged(m_totalMs);
    emit positionChanged(m_positionMs);
}

void PlaybackController::reopenSources(const QStringList &paths)
{
    std::lock_guard<std::mutex> lk(m_mx);
    for (const QString &p : paths)
        if (!m_reopenPaths.contains(p))
            m_reopenPaths << p;
}

void PlaybackController::setPreviewConfig(const stitch::PreviewStitchConfig &config)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (config == m_pendingPreviewConfig)
            return;
        m_pendingPreviewConfig = config;
        m_previewConfigDirty = true;
        m_decoderEof = false;
        // Re-decode from where we are so the current frame re-stitches under
        // the new settings. requestSeekLocked bumps m_seekGen and sets
        // m_hardStall via the decoder, so the presenter freezes the playhead
        // through the re-stitch instead of running the clock on.
        if (m_haveTimeline)
            requestSeekLocked(m_positionMs);
    }
    // While paused the presenter only picks up a frame after a seek; without
    // this the re-stitched frame was decoded but never shown until the next
    // play/scrub.
    if (m_haveTimeline)
        m_presentAfterSeek = true;
    m_cv.notify_all();
}

void PlaybackController::play()
{
    if (!m_haveTimeline || m_playing)
        return;
    if (m_positionMs >= m_totalMs - kEndEpsilonMs) {
        m_positionMs = 0;
        std::lock_guard<std::mutex> lk(m_mx);
        requestSeekLocked(0);
    }
    m_reachedEnd = false;
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_prerolling = true; // start on a cushion of frames -- see kResumeFrames
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_playing = true;
    }
    m_cv.notify_all();
    emit playingChanged(true);
}

void PlaybackController::pause()
{
    if (!m_playing)
        return;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_playing = false;
    }
    m_positionMs = clampTimeline(m_clockBaseMs + clockElapsedMs());
    emit playingChanged(false);
    emit positionChanged(m_positionMs);
}

void PlaybackController::togglePlay()
{
    if (m_playing)
        pause();
    else
        play();
}

void PlaybackController::seek(qint64 timelineMs)
{
    if (!m_haveTimeline)
        return;
    m_positionMs = clampTimeline(timelineMs);
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_reachedEnd = false;
    // Frozen from here until kResumeFrames of the new position are queued --
    // not just until the first one is read (m_hardStall): resuming on a
    // single frame ran the queue dry again at once.
    m_prerolling = true;
    if (m_settleTimer)
        m_settleTimer->stop();
    {
        std::lock_guard<std::mutex> lk(m_mx);
        const Handy h = frameAtLocked(m_positionMs);
        if (h == Handy::No)
            requestSeekLocked(m_positionMs);
        m_presentAfterSeek = h != Handy::OnScreen;
    }
    m_cv.notify_all();
    emit positionChanged(m_positionMs);
    if (m_resumeAfterScrub) { // the end of a drag that started during playback
        m_resumeAfterScrub = false;
        play();
    }
}

void PlaybackController::scrub(qint64 timelineMs)
{
    if (!m_haveTimeline)
        return;
    // Dragging during playback scrubs exactly like paused: pause for the drag,
    // and the drag's final seek() resumes.
    if (m_playing) {
        pause();
        m_resumeAfterScrub = true;
    }
    m_positionMs = clampTimeline(timelineMs);
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_reachedEnd = false;
    m_prerolling = true;
    Handy h;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        h = frameAtLocked(m_positionMs);
        if (h == Handy::No)
            requestSeekLocked(m_positionMs, /*keyframe=*/true);
        m_presentAfterSeek = h != Handy::OnScreen;
    }
    m_cv.notify_all();
    emit positionChanged(m_positionMs);
    // The decoder may land on a keyframe; once the pointer rests, settle()
    // brings the exact frame.
    if (h == Handy::No) {
        if (!m_settleTimer) {
            m_settleTimer = new QTimer(this);
            m_settleTimer->setSingleShot(true);
            m_settleTimer->setInterval(kScrubSettleMs);
            connect(m_settleTimer, &QTimer::timeout, this, &PlaybackController::settle);
        }
        m_settleTimer->start();
    }
}

void PlaybackController::setRate(double rate)
{
    rate = std::clamp(rate, 0.05, 8.0);
    if (rate == m_rate)
        return;
    // Re-anchor so the time played so far keeps the old rate.
    if (m_playing && !m_prerolling)
        m_positionMs = clampTimeline(m_clockBaseMs + clockElapsedMs());
    m_clockBaseMs = m_positionMs;
    m_clock.restart();
    m_rate = rate;
}

void PlaybackController::settle()
{
    if (!m_haveTimeline || m_playing)
        return;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        const Handy h = frameAtLocked(m_positionMs);
        if (h == Handy::No)
            requestSeekLocked(m_positionMs);
        m_presentAfterSeek = h != Handy::OnScreen;
    }
    m_cv.notify_all();
}

PlaybackController::Handy PlaybackController::frameAtLocked(qint64 timelineMs)
{
    // Only while paused, with the shown frame and the queue both from the
    // latest seek: then the queue is the decoder's lookahead, in order, right
    // after the shown frame.
    if (m_playing || m_seekPending || m_shownGen != m_seekGen)
        return Handy::No;
    const qint64 limit = timelineMs + kSameFrameMs; // a frame starting by here
    size_t n = 0;
    while (n < m_queue.size() && m_queue[n].frame.timestamp_ms <= limit)
        ++n;
    if (n == 0) {
        // Before every queued frame: the shown one, if it starts by then --
        // and with nothing queued only if it starts right there.
        const bool shown = m_queue.empty()
                               ? std::abs(m_shownMs - timelineMs) <= kSameFrameMs
                               : m_shownMs <= limit && timelineMs >= m_shownMs - kSameFrameMs;
        return shown ? Handy::OnScreen : Handy::No;
    }
    // Queued frame n-1 is the one, if a later one is queued or it starts
    // right there (no telling where the last one ends).
    if (n == m_queue.size() &&
        std::abs(m_queue[n - 1].frame.timestamp_ms - timelineMs) > kSameFrameMs)
        return Handy::No;
    m_queue.erase(m_queue.begin(), m_queue.begin() + (n - 1));
    return Handy::Queued;
}

void PlaybackController::requestSeekLocked(qint64 timelineMs, bool keyframe)
{
    m_seekPending = true;
    m_seekTargetMs = timelineMs;
    m_seekToKeyframe = keyframe;
    m_decoderEof = false;
    ++m_seekGen;
    // While playing, drop buffered frames so the queue re-forms cleanly around
    // the new clock position. A paused exact seek drops them too -- they're
    // from before it and must never be shown; the viewport keeps its
    // picture until the target arrives. While scrubbing keep an earlier
    // scrub's target not shown yet: a fast drag fires seeks faster than one
    // decode, and clearing every one would freeze the picture for the whole
    // drag (tick()). The rest is lookahead nobody will show; it would only
    // fill the queue and stall the decoder.
    if (m_playing || !keyframe)
        m_queue.clear();
    else
        std::erase_if(m_queue, [this](const QueuedFrame &q) { return q.gen <= m_shownGen; });
}

// ---------------------------------------------------------------------------
// Present tick (controller's QThread)
// ---------------------------------------------------------------------------

void PlaybackController::setClockHeld(bool held)
{
    if (held != m_clockHeld) {
        m_clockHeld = held;
        emit clockHeldChanged(held);
    }
}

void PlaybackController::presentInsets(qint64 timelineMs, bool playing)
{
    std::vector<EquirectViewportWindow::InsetDraw> draws = m_insets.frameFor(timelineMs, playing, m_shownClipId);
    auto same = [](const EquirectViewportWindow::InsetDraw &a,
                   const EquirectViewportWindow::InsetDraw &b) {
        return a.clipId == b.clipId && a.frameKey == b.frameKey && a.look == b.look &&
               a.frame == b.frame && a.aspect == b.aspect && a.colorGrade == b.colorGrade;
    };
    if (draws.size() == m_lastInsets.size() &&
        std::equal(draws.begin(), draws.end(), m_lastInsets.begin(), same))
        return;
    m_lastInsets = draws;
    if (m_viewport)
        m_viewport->SetFrameInsets(std::move(draws)); // with the picture they go over
}

void PlaybackController::statsTick(bool playing, size_t queued)
{
    Stats &s = m_stats;
    const auto now = render::framestats::Clock::now();
    if (!playing) { // only playback is summarised; start afresh on the next play
        s = Stats{};
        return;
    }
    if (s.windowStart == render::framestats::Clock::time_point{})
        s.windowStart = now;
    s.minQueue = std::min(s.minQueue, queued);
    if (render::framestats::MsBetween(s.windowStart, now) < 1000.0)
        return;
    render::framestats::Samples read;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        std::swap(read, m_statsReadMs);
    }
    std::fprintf(stderr,
                 "[stats] playback @%.1fs: shown %d, skipped %d, buffering ticks %d, min queue "
                 "%zu | late mean %.1f max %.1f ms | decoder read n %zu mean %.2f p99 %.2f max "
                 "%.2f ms | stab lookup n %zu mean %.3f max %.3f ms\n",
                 m_positionMs / 1000.0, s.shown, s.skipped, s.bufferingTicks,
                 s.minQueue == SIZE_MAX ? size_t(0) : s.minQueue, s.lateMs.mean(), s.lateMs.max(),
                 read.count(), read.mean(), read.pct(0.99), read.max(), s.stabMs.count(),
                 s.stabMs.mean(), s.stabMs.max());
    s = Stats{};
    s.windowStart = now;
}

void PlaybackController::tick()
{
    if (!m_haveTimeline)
        return;

    const bool playing = m_playing;

    bool hardStall = false;
    bool seekPending = false;
    size_t queued = 0;
    size_t resumeFrames = kResumeFrames; // never more than the queue can hold
    bool decoderEof = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        hardStall = m_hardStall;
        seekPending = m_seekPending;
        queued = m_queue.size();
        resumeFrames = std::min(kResumeFrames,
                                m_lensQueue ? kLensQueueMax : queueCapFor(m_frameBytes));
        decoderEof = m_decoderEof;
    }

    // A stitch is blocking the decoder (a seek, or opening the next clip at a
    // boundary) and we've shown everything we had. Freeze the playhead and show
    // "Buffering…"; re-anchor so the clock resumes from here instead of
    // counting the stall as elapsed playback. Stay frozen until kResumeFrames
    // are queued, so playback resumes smoothly.
    if (playing) {
        const bool stalled = (hardStall || seekPending) && queued == 0;
        if (stalled)
            m_prerolling = true;
        if (m_prerolling) {
            if (stalled || (queued < resumeFrames && !decoderEof)) {
                if (render::framestats::Enabled()) {
                    ++m_stats.bufferingTicks;
                    statsTick(true, queued);
                }
                m_clockBaseMs = m_positionMs;
                m_clock.restart();
                if (!m_buffering) {
                    m_buffering = true;
                    emit bufferingChanged(true);
                }
                setClockHeld(true);
                presentInsets(m_positionMs, false); // frozen with the picture
                return;
            }
            m_prerolling = false;
        }
        setClockHeld(false);
    }

    qint64 clockMs = m_positionMs;
    if (playing) {
        clockMs = std::clamp<qint64>(m_clockBaseMs + clockElapsedMs(), 0, m_totalMs);
    }

    stitch::Frame chosen;
    std::shared_ptr<const render::LensFrame> chosenLens;
    QString chosenPath;
    qint64 chosenSourceMs = 0;
    int chosenClipId = 0;
    bool haveChosen = false;
    bool chosenIsCurrent = false;
    bool queueEmpty = true;
    bool eof = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (playing) {
            while (!m_queue.empty() && m_queue.front().frame.timestamp_ms <= clockMs) {
                if (haveChosen)
                    ++m_stats.skipped; // replaced before it was ever shown
                chosen = std::move(m_queue.front().frame);
                chosenLens = std::move(m_queue.front().lens);
                chosenPath = m_queue.front().sourcePath;
                chosenSourceMs = m_queue.front().sourceMs;
                chosenClipId = m_queue.front().clipId;
                m_shownGen = m_queue.front().gen;
                m_shownMs = m_queue.front().frame.timestamp_ms;
                m_shownClipId = m_queue.front().clipId;
                m_queue.pop_front();
                haveChosen = true;
            }
        } else if (m_presentAfterSeek && !m_queue.empty()) {
            // The seek's own frame is the first of its generation; any after
            // it are lookahead, kept for a step forward (seek()). Without one
            // yet, a scrub may show an earlier scrub's target that never
            // reached the screen -- a fast drag outruns the decoder
            // -- but never older lookahead, or anything from
            // before an exact seek. The queue is in generation order.
            const uint64_t gen = m_queue.back().gen;
            chosenIsCurrent = gen == m_seekGen;
            if (chosenIsCurrent || (m_seekToKeyframe && gen > m_shownGen)) {
                auto it = std::find_if(m_queue.begin(), m_queue.end(),
                                       [gen](const QueuedFrame &q) { return q.gen == gen; });
                chosen = std::move(it->frame);
                chosenLens = std::move(it->lens);
                chosenPath = it->sourcePath;
                chosenSourceMs = it->sourceMs;
                chosenClipId = it->clipId;
                m_queue.erase(m_queue.begin(), std::next(it));
                m_shownGen = gen;
                m_shownMs = chosen.timestamp_ms;
                m_shownClipId = chosenClipId;
                haveChosen = true;
            }
        }
        queueEmpty = m_queue.empty();
        eof = m_decoderEof;
    }
    if (haveChosen)
        m_cv.notify_all(); // room in the queue -- let the decoder run

    if (haveChosen) {
        m_presentAfterSeek = !playing && !chosenIsCurrent;
        if (m_viewport) {
            StabProvider provider;
            OutlineProvider outlineProvider;
            ViewProvider viewProvider;
            {
                std::lock_guard<std::mutex> lk(m_stabMx);
                provider = m_stabProvider;
                outlineProvider = m_outlineProvider;
                viewProvider = m_viewProvider;
            }
            float stab[9];
            const auto stabT0 = render::framestats::Clock::now();
            const bool haveStab = provider && provider(chosenClipId, chosenSourceMs, stab);
            if (render::framestats::Enabled() && playing) {
                ++m_stats.shown;
                m_stats.lateMs.add(double(clockMs - chosen.timestamp_ms));
                if (haveStab)
                    m_stats.stabMs.add(
                        render::framestats::MsBetween(stabT0, render::framestats::Clock::now()));
            }
            std::vector<EquirectViewportWindow::RegionOutline> outlines;
            if (outlineProvider)
                outlineProvider(chosenClipId, chosenSourceMs, outlines);
            EquirectViewportWindow::FrameView view;
            if (viewProvider)
                viewProvider(chosenClipId, chosenSourceMs, view);
            if (chosenLens)
                m_viewport->SetLensFrame(std::move(chosenLens), haveStab ? stab : nullptr,
                                         std::move(outlines), view);
            else
                m_viewport->SetFrame(chosen.width, chosen.height, std::move(chosen.rgba),
                                     haveStab ? stab : nullptr, std::move(outlines), view);
        }
    }

    presentInsets(clockMs, playing);
    if (render::framestats::Enabled())
        statsTick(playing, queued);

    const bool wantFrame = playing || m_presentAfterSeek;
    const bool starved = wantFrame && !haveChosen && queueEmpty && !eof;
    if (starved != m_buffering) {
        m_buffering = starved;
        emit bufferingChanged(m_buffering);
    }

    if (playing) {
        m_positionMs = clockMs;
        emit positionChanged(m_positionMs);
        const bool clockAtEnd = clockMs >= m_totalMs - kEndEpsilonMs;
        if ((clockAtEnd || (queueEmpty && eof)) && !m_reachedEnd) {
            m_reachedEnd = true;
            m_positionMs = m_totalMs;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                m_playing = false;
                m_queue.clear();
            }
            if (m_buffering) {
                m_buffering = false;
                emit bufferingChanged(false);
            }
            setClockHeld(false);
            emit playingChanged(false);
            emit positionChanged(m_positionMs);
        }
    }
}
