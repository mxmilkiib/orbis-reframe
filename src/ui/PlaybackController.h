#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>

#include <IStitchSource.h> // stitch::Frame
#include <LensFrame.h>     // render::LensFrame

#include "InsetFeed.h" // video-track insets
#include "../render/FrameStats.h" // ORBIS_FRAME_STATS

#include <QStringList>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class QTimer;
class EquirectViewportWindow;

namespace stitch {
class IStitchSource;
}

// Plays a *timeline* -- an ordered list of clip segments laid end to end --
// through the stitch sources into the Vulkan viewport. Frames are decoded
// fisheye pairs (render::LensFrame: the viewport renders the view from them)
// whenever both the source and the viewport can share them, else stitched
// equirects. Positions and seeks are in *timeline time* (0 .. total), not any
// single clip's source time; the per-clip trims are baked into each segment's
// [inMs, outMs] source window, so there is no separate "trim" concept here.
//
// Two threads cooperate through a bounded frame queue:
//
//   * a decoder thread runs SeekTo/ReadFrame on the current segment's source,
//     rewrites each frame's timestamp into timeline time, and pushes into
//     m_queue. When a frame reaches the segment's trim-out (or the source
//     hits EOF) it advances to the next segment -- opening that source if the
//     path changed, whose first SeekTo blocks on that clip's first stitch;
//     the "Buffering…" indicator covers it. Consecutive segments that share
//     a source path switch with just a SeekTo.
//
//   * the GUI-facing side (this QObject, on its own worker QThread -- see
//     MainWindow) runs a fixed-cadence present QTimer that only *presents*:
//     wall clock in timeline time, pop the newest frame that isn't in the
//     future, drop the rest.
//
// setTimeline() replaces the whole segment list and may be called repeatedly
// (a trim edit, a project (re)load). It re-clamps the playhead and re-seeks.
// Only the decoder thread ever touches the stitch sources. Opened sources are
// kept in a small LRU cache (kMaxCachedSources) so scrubbing back and forth
// across a clip boundary doesn't re-open (and re-parse the trailer of) the
// same file each time; sources dropped from the timeline entirely are
// released on the next setTimeline().
class PlaybackController : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(QObject *parent = nullptr);
    ~PlaybackController() override;

    // Set once, before the first setTimeline(). Not owned.
    void setViewport(EquirectViewportWindow *viewport) { m_viewport = viewport; }
    // What opens a clip's stitch source (ClipBackend::makeSource); the
    // default is stitch::MakeStitchSource. Set before the first clip plays.
    using SourceFactory = std::function<std::unique_ptr<stitch::IStitchSource>()>;
    void setSourceFactory(SourceFactory factory) { m_makeSource = std::move(factory); }

    // One clip on the timeline: a source file and the [in, out) source-time
    // window to play from it. MainWindow builds these from the Timeline.
    // AudioPlayer reuses this same struct for its own setTimeline() (the
    // timeline's length and cuts; the sound is on the audio tracks).
    struct Segment {
        QString sourcePath;
        qint64 inMs = 0;
        qint64 outMs = 0;
        // Playback speed (ClipItem::speed): the segment spans (outMs - inMs) /
        // speed on the timeline. Same rounding as ClipItem::timelineDurationMs.
        double speed = 1.0;
        // The Timeline clip it plays (ClipItem::id): what the per-clip
        // capture timing and stabilization are looked up by, since two clips
        // of one file can differ.
        int clipId = 0;
        // A flat video (ClipItem::flat): played through a FlatClipSource of
        // this rate and length, its picture drawn by the insets.
        bool flat = false;
        double fps = 0.0;
        qint64 sourceDurationMs = 0;
        // The clip's stitch settings (ClipItem::lensGuard / depthSeam), the
        // same for every segment of a file; the source is opened with them
        // over the preview config's (setPreviewConfig).
        stitch::PreviewStitchConfig::LensGuard lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
        bool depthSeam = true;
        qint64 timelineSpanMs() const { return std::llround((outMs - inMs) / speed); }
        qint64 sourceAtLocalMs(qint64 localMs) const { return inMs + std::llround(localMs * speed); }
        qint64 localAtSourceMs(qint64 sourceMs) const
        {
            return std::llround((sourceMs - inMs) / speed);
        }
    };

    // A segment plus its offset on the timeline. Both this controller and
    // AudioPlayer place the same Segment list with placeSegments() and look
    // positions up with segmentAt(), so picture and sound cut at the same
    // instants.
    struct Placed {
        Segment seg;
        qint64 startMs = 0; // timeline offset of seg.inMs
        qint64 durationMs() const { return seg.timelineSpanMs(); }
        qint64 endMs() const { return startMs + durationMs(); }
    };
    // `segments` laid end to end from 0; the timeline's total is the last
    // one's endMs().
    static QList<Placed> placeSegments(const QList<Segment> &segments);
    // Index into `placed` whose timeline span contains `timelineMs` (past the
    // end -> the last); -1 only if `placed` is empty.
    static int segmentAt(const QList<Placed> &placed, qint64 timelineMs);

    // Replace the timeline. NOT a slot: MainWindow invokes it through a queued
    // lambda (QMetaObject::invokeMethod), so QList<Segment> needs no metatype
    // registration. Runs on the worker thread.
    void setTimeline(const QList<Segment> &segments);
    // Open these files afresh from the next setTimeline() on, not from the
    // sources cached for them: a file that was gone and is back (a drive
    // plugged in again) left a cached source that can't read any more.
    void reopenSources(const QStringList &paths);

    // Change the preview-stitch settings (Stitching panel). Like
    // setTimeline(), invoked via a queued lambda so it runs on the worker
    // thread. If the config actually changed and a timeline is loaded, the
    // decoder drops its open sources and re-stitches from the current
    // position under the new cache tag -- a brief "Buffering…" like a cold
    // scrub. No-op if unchanged.
    void setPreviewConfig(const stitch::PreviewStitchConfig &config);

    // Per-clip capture-time correction for the stitch (see
    // stitch::IStitchSource::SetCaptureTiming), by Segment::clipId; replaces
    // the whole set, and a clip not in it = off. Thread-safe, callable from
    // the GUI thread; the decoder applies a clip's timing to its source
    // before the clip's next frame.
    void setCaptureTimings(std::map<int, stitch::CaptureTiming> timings);
    // Seam alignment fine-tune for one source (lens-1 yaw/pitch/roll, deg;
    // IStitchSource::SetLens1Adjust). Thread-safe; applies from the next
    // decoded frame -- seek to refresh a paused picture.
    void setSeamAdjust(const QString &path, std::array<double, 3> deg);
    // Privacy blur per clip id (IStitchSource::SetBlurQuads): the whole set,
    // a clip not in it = none. Per clip, not per source, as two clips of one
    // file may differ. Thread-safe; applies from the next decoded frame --
    // seek to refresh a paused picture.
    void setBlurQuads(std::map<int, stitch::BlurQuadsFn> blurs);
    // Clip `clipId`'s blur as set; empty = none. Thread-safe.
    stitch::BlurQuadsFn blurQuads(int clipId);

    // Gyro de-rotation for a presented frame: fills `m` (row-major 3x3) for
    // source time `sourceMs` of clip `clipId` (Segment::clipId) and returns
    // true, or returns false when it has nothing for that clip (the viewport
    // then keeps its current matrix). Called on the controller thread for every presented frame, with
    // the frame's own source time, and handed to the viewport with the frame.
    // Thread-safe to set from the GUI thread.
    using StabProvider = std::function<bool(int clipId, qint64 sourceMs, float m[9])>;
    void setStabilizationProvider(StabProvider provider);

    // Region outlines for a presented frame (the Privacy panel's "Show
    // regions"): appends those of source time `sourceMs` of clip `clipId`.
    // Called like the StabProvider, and handed to the viewport with the
    // frame. Empty = none. Thread-safe to set from the GUI thread; seek to
    // refresh a paused picture.
    using OutlineProvider = std::function<void(
        int clipId, qint64 sourceMs, std::vector<EquirectViewportWindow::RegionOutline> &out)>;
    void setOutlineProvider(OutlineProvider provider);

    // The view and colour grade for a presented frame (its clip's reframe
    // path at source time `sourceMs`, its grade). Called like the
    // StabProvider and handed to the viewport with the frame, so a cut
    // changes the view on the frame it changes the picture. Thread-safe to
    // set from the GUI thread.
    using ViewProvider = std::function<void(int clipId, qint64 sourceMs,
                                            EquirectViewportWindow::FrameView &view)>;
    void setViewProvider(ViewProvider provider);

    // The video tracks' inset clips, bottom-most first. Thread-safe,
    // callable from the GUI thread; they're drawn over the view from the next
    // present tick, at the playhead.
    void setInsets(std::vector<InsetFeed::Clip> clips) { m_insets.setClips(std::move(clips)); }

public slots:
    void play();
    void pause();
    void togglePlay();
    void seek(qint64 timelineMs);
    // A seek while dragging the playhead: shows the source's nearest keyframe
    // (inside the clip's trim) instead of the exact frame -- one decode instead
    // of up to a GOP's worth -- and the drag's final seek() lands on the exact
    // frame. During playback the first scrub pauses and that final seek()
    // resumes, so a drag behaves the same playing or paused.
    void scrub(qint64 timelineMs);
    // Playback speed of the preview (the player bar's 0.25x .. 2x): the clock
    // runs at `rate` times real time. Every frame is still decoded, so a fast
    // rate shows what the decoder keeps up with. Not the clips' own speed
    // (Segment::speed), which is part of the edit.
    void setRate(double rate);

signals:
    void openFailed(const QString &error);
    void durationChanged(qint64 totalMs);
    void positionChanged(qint64 timelineMs);
    void playingChanged(bool playing);
    // True while the presenter wants a frame it doesn't have (decoder behind,
    // or a new clip still opening). The UI debounces this.
    void bufferingChanged(bool buffering);
    // True while the playback clock is frozen (a seek / stall, until
    // kResumeFrames are queued). Unlike bufferingChanged -- which also fires
    // when a tick merely finds the queue empty while the clock runs on --
    // this is what audio must follow: AudioPlayer::setHold.
    void clockHeldChanged(bool held);

private:
    // A decoded frame (timestamp already rewritten to timeline time) plus the
    // seek generation it was produced for.
    struct QueuedFrame {
        stitch::Frame frame; // timestamp always set; rgba empty for a lens frame
        std::shared_ptr<const render::LensFrame> lens; // set => a lens frame
        uint64_t gen = 0;
        QString sourcePath;  // clip the frame came from
        qint64 sourceMs = 0; // its source time (frame.timestamp_ms is timeline time)
        int clipId = 0;      // Segment::clipId
    };

    void startDecoder();
    void stopDecoder();
    void decoderLoop();
    // Caller holds m_mx; bumps m_seekGen. `keyframe`: land on the nearest
    // keyframe instead of the exact frame (scrub()).
    void requestSeekLocked(qint64 timelineMs, bool keyframe = false);
    // Paused: is the frame shown at `timelineMs` on screen already, or among
    // the queued lookahead (then the frames before it are dropped, and tick()
    // shows it next)? Either way no seek is needed. Caller holds m_mx.
    enum class Handy { No, OnScreen, Queued };
    Handy frameAtLocked(qint64 timelineMs);
    void settle(); // a scrub's pointer rested: the exact frame
    void tick();                                // present step, on the QObject's thread
    void presentInsets(qint64 timelineMs, bool playing); // tick's inset step
    void setClockHeld(bool held);               // emits clockHeldChanged on change

    // --- decoder-thread helpers ---
    // Returns the open source for `path` (from the small cache, or freshly
    // opened). nullptr + *err set on failure.
    std::shared_ptr<stitch::IStitchSource> acquireSource(const Segment &seg, QString *err);
    // A new, unopened source for `seg`: a FlatClipSource for a flat clip,
    // else m_makeSource's, or the real one. Any thread.
    std::unique_ptr<stitch::IStitchSource> makeSource(const Segment &seg) const;
    // What `seg`'s source is opened with: the preview config with the clip's
    // stitch settings.
    static stitch::PreviewStitchConfig openConfig(stitch::PreviewStitchConfig preview,
                                                  const Segment &seg);
    // Make segment `index` the one being decoded, opening its source if the
    // path changed. Prunes any other cached source. false (openFailed emitted)
    // on open failure.
    bool switchToSegment(int index);
    qint64 clampTimeline(qint64 ms) const {
        return ms < 0 ? 0 : (ms > m_totalMs ? m_totalMs : ms);
    }

    EquirectViewportWindow *m_viewport = nullptr;
    QTimer *m_timer = nullptr;

    // --- presenter-thread (the controller's QThread) only ---
    bool m_haveTimeline = false;
    qint64 m_totalMs = 0;
    qint64 m_positionMs = 0; // timeline time
    QElapsedTimer m_clock;
    qint64 m_clockBaseMs = 0;
    double m_rate = 1.0; // setRate()
    // Timeline ms played since m_clock's restart, at m_rate.
    qint64 clockElapsedMs() const { return std::llround(double(m_clock.elapsed()) * m_rate); }
    bool m_presentAfterSeek = false;
    uint64_t m_shownGen = 0; // seek generation of the frame on screen
    qint64 m_shownMs = 0;    // and its timeline time
    int m_shownClipId = 0;   // and clip (Segment::clipId): what a transition's layer goes over
    QTimer *m_settleTimer = nullptr; // scrub(): the pointer rested -> settle()
    bool m_buffering = false;
    bool m_reachedEnd = false;
    bool m_prerolling = false; // frozen after a stall until kResumeFrames are queued
    bool m_clockHeld = false;  // last clockHeldChanged value
    // ORBIS_FRAME_STATS: while playing, a per-second summary of what the
    // presenter showed and skipped, and what the decoder took per frame.
    struct Stats {
        render::framestats::Clock::time_point windowStart{};
        int shown = 0;
        int skipped = 0;        // decoded but never shown: the clock had passed them
        int bufferingTicks = 0; // ticks spent frozen, waiting for the decoder
        size_t minQueue = SIZE_MAX;
        render::framestats::Samples lateMs; // clock minus the shown frame's time
        render::framestats::Samples stabMs; // the stabilization lookup
    } m_stats;
    void statsTick(bool playing, size_t queued);

    // --- shared with the decoder thread, guarded by m_mx ---
    std::mutex m_mx;
    std::condition_variable m_cv;
    QList<Placed> m_placed;       // authoritative timeline
    bool m_timelineDirty = false; // decoder should re-copy m_placed
    QStringList m_reopenPaths; // drop these cached sources with the next timeline
    stitch::PreviewStitchConfig m_pendingPreviewConfig;
    bool m_previewConfigDirty = false; // decoder should drop sources + re-stitch
    std::deque<QueuedFrame> m_queue;
    size_t m_frameBytes = 0; // size of the last decoded frame; sets the queue cap
    bool m_lensQueue = false; // the last decoded frame was a lens frame (kLensQueueMax cap)
    render::framestats::Samples m_statsReadMs; // ORBIS_FRAME_STATS: decoder read per frame
    std::thread m_decoder;
    bool m_playing = false;
    bool m_stop = false;
    bool m_decoderEof = false;
    bool m_seekPending = false;
    qint64 m_seekTargetMs = 0; // timeline time
    bool m_seekToKeyframe = false; // the pending seek is a scrub (see scrub())
    bool m_resumeAfterScrub = false; // a drag paused playback; the next seek() resumes
    uint64_t m_seekGen = 0;
    // The decoder is seeking: from a seek or a clip boundary until the first
    // frame after it is read. While set, tick() freezes the playhead once the
    // queue runs dry instead of letting the wall clock run on over a frozen
    // picture.
    bool m_hardStall = false;

    // --- decoder-thread only ---
    static constexpr int kMaxCachedSources = 4;
    QList<Placed> m_decPlaced;
    // Preview-stitch config the decoder opens sources with. Updated from
    // m_pendingPreviewConfig when m_previewConfigDirty is seen.
    stitch::PreviewStitchConfig m_decPreviewConfig;
    SourceFactory m_makeSource;
    int m_decSegIndex = -1;
    qint64 m_decLastSourceMs = -1; // source time of the last frame read; -1 after a seek
    std::map<QString, std::shared_ptr<stitch::IStitchSource>> m_srcCache;
    QStringList m_srcLru; // paths, least-recently-used first
    std::shared_ptr<stitch::IStitchSource> m_activeSource;
    QString m_activePath;

    // The next segment's source, sought and its first frame read on a worker
    // thread shortly before the boundary, so the cut doesn't wait for the
    // seek's decode (startPrefetch / takePrefetch). The source is the
    // job's own until taken: a cached one is moved out of m_srcCache, and a
    // jump within the active file opens a second one.
    struct Prefetch {
        int segIndex = -1;
        QString path;
        std::shared_ptr<stitch::IStitchSource> src;
        bool opened = false; // src open, SeekTo done, the read below tried
        stitch::Frame frame;
        std::shared_ptr<render::LensFrame> lens;
        bool got = false;
        bool stale = false; // a seek / edit since: dropped once done
        std::future<void> done; // last: destroyed (waited for) first
    };
    std::unique_ptr<Prefetch> m_prefetch;
    // A read already made for the active segment (a taken prefetch's first
    // frame); the decoder's next read returns it.
    struct HeldRead {
        stitch::Frame frame;
        std::shared_ptr<render::LensFrame> lens;
        bool got = false;
    };
    std::optional<HeldRead> m_heldRead;

    // Capture-timing settings per clip id, written by the GUI thread.
    using RsSetting = stitch::CaptureTiming;
    std::mutex m_stabMx;
    StabProvider m_stabProvider; // guarded by m_stabMx
    OutlineProvider m_outlineProvider; // guarded by m_stabMx
    ViewProvider m_viewProvider;       // guarded by m_stabMx
    std::mutex m_rsMx;
    std::map<int, RsSetting> m_rsSettings; // guarded by m_rsMx
    std::map<QString, std::array<double, 3>> m_seamAdjust; // guarded by m_rsMx, same generation
    std::map<int, stitch::BlurQuadsFn> m_blurQuads;        // guarded by m_rsMx, same generation
    std::atomic<uint64_t> m_rsGen{0};          // bumped on every change
    uint64_t m_decRsGen = ~uint64_t(0);        // decoder: generation last applied
    QString m_decRsPath;                        // decoder: source it was applied to
    int m_decRsClipId = 0;                      // decoder: clip it was applied for
    // The hand-over to the next clip in the same file, if any (see
    // ClipHandOver): what was applied, so a timeline edit re-applies.
    qint64 m_decRsUntilMs = 0;
    int m_decRsAfterClipId = 0;
    void applyCaptureTimingIfChanged();         // decoder thread
    // Where segment `index`'s source hands over to the next segment's capture
    // timing: the next segment reads on in the same file (a split), so the
    // frame that crosses the cut -- read before the decoder knows it crosses
    // -- is stitched with the next clip's timing, not this one's. No
    // hand-over: untilMs = INT64_MAX. Decoder thread (reads m_decPlaced).
    struct ClipHandOver {
        qint64 untilMs = INT64_MAX;
        int afterClipId = 0;
    };
    ClipHandOver handOverAfter(int index) const;
    // Reads m_rsMx: `path`'s settings, and clip `clipId`'s capture timing
    // and blur (then `handOver.afterClipId`'s from `handOver.untilMs` on).
    void applySourceSettings(stitch::IStitchSource &src, const QString &path, int clipId,
                             ClipHandOver handOver);

    // The insets' own decoder, and what was last handed to the viewport
    // (frame key + look per inset) so an unchanged picture isn't re-sent.
    InsetFeed m_insets;
    std::vector<EquirectViewportWindow::InsetDraw> m_lastInsets;

    void touchSourceLru(const QString &path);   // decoder thread
    void pruneSourcesToTimeline();               // decoder thread
    void setHardStall(bool on);                   // decoder thread
    void startPrefetch(const Placed &cur, qint64 timelineMs); // decoder thread
    bool takePrefetch(int next);                  // decoder thread
    void invalidatePrefetch();                    // decoder thread
};
