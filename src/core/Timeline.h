#pragma once

#include "TrackClip.h"
#include "ClipItem.h"
#include "Transition.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The main video track -- an ordered list of ClipItems laid end to end, no
// gaps, a cut between two clips either hard or a transition (Transition.h)
// -- plus any number of extra tracks under it,
// whose clips sit at free positions: audio tracks, and video (overlay) tracks
// whose clips are drawn as insets over the output (grouped, see Track). The main track
// defines the timeline's length; anything on the extra tracks past its end is
// not played. The only thing that mutates a ClipItem, so every
// successful edit becomes a signal here. Lives on the GUI thread.
class Timeline : public QObject {
    Q_OBJECT

public:
    explicit Timeline(QObject *parent = nullptr);

    // A timeline instant expressed as (which clip, how far into its span on the
    // timeline). clipLocalMs is *timeline* ms from the clip's start; its source
    // time is ClipItem::sourceAtLocalMs(clipLocalMs) (they differ when the
    // clip's speed isn't 1). clipIndex is -1 only when empty.
    struct Point {
        int clipIndex = -1;
        qint64 clipLocalMs = 0;
    };

    int count() const { return static_cast<int>(m_clips.size()); }
    bool isEmpty() const { return m_clips.isEmpty(); }

    const ClipItem &clipAt(int index) const { return m_clips.at(index); }
    int indexOfId(int id) const;
    const ClipItem *clipById(int id) const;

    // Sum of every clip's timeline span (trimmed duration / speed).
    qint64 totalDurationMs() const;

    // timeline ms -> (clip, clip-local ms). Input clamped to [0, total]. A cut
    // boundary belongs to the later clip; the very end resolves to the last
    // clip at its full trimmed span. clipIndex == -1 iff the timeline is empty.
    Point timelineToClip(qint64 timelineMs) const;
    // (clip, clip-local ms) -> timeline ms. clipLocalMs is clamped to the
    // clip's trimmed span. Returns 0 for an out-of-range index.
    qint64 clipToTimeline(int clipIndex, qint64 clipLocalMs) const;

    // Keyboard navigation. The timeline ms `frames` source frames of the clip
    // under `timelineMs` away (negative = back), landing mid-frame so the
    // result is unambiguous; crossing a cut lands on the next clip's first
    // frame / the previous clip's last one. Clamped to [0, total].
    qint64 stepFrames(qint64 timelineMs, int frames) const;
    // The nearest edit point strictly after (direction > 0) or before
    // (direction < 0) `timelineMs`: the timeline start / end, a cut, or an
    // extra-track clip's start / end (within the main timeline). Returns
    // `timelineMs` if there is none that way.
    qint64 adjacentEditPoint(qint64 timelineMs, int direction) const;
    // The nearest keyframe strictly after / before `timelineMs`, in timeline
    // ms: a reframe key of any video clip, or with `overlayClipId` > 0 a key
    // of that overlay clip. Keys trimmed out of view are skipped, and so is
    // one within a few ms of `timelineMs` (the playhead already on it).
    // Returns `timelineMs` if there is none that way.
    qint64 adjacentKey(qint64 timelineMs, int direction, int overlayClipId = 0) const;

    // A flattened, playback-facing view: one entry per clip in order, carrying
    // just what the playback layer needs to decode it. std::string (not
    // QString) because the consumer is PlaybackController / the stitch source.
    // Rebuilt on demand -- push it to the controller on any structural or trim
    // change.
    struct ClipPlan {
        std::string sourcePath;
        qint64 trimInMs = 0;
        qint64 trimOutMs = 0;
    };
    std::vector<ClipPlan> playbackPlan() const;

    // --- transitions (Transition.h) ------------------------------------
    // In no particular order.
    const QList<Transition> &transitions() const { return m_transitions; }
    // The transition at the cut after main-track clip `leftClipId`, or null.
    const Transition *transitionAfter(int leftClipId) const;
    // What a transition plays as, centred on its cut at `cutMs`: its
    // duration, shortened to the footage there is past the two clips' trims
    // (at their speeds; one that uses it, Transition::usesHandles) and to
    // half of either clip, so the transitions at a
    // clip's two ends never meet. `limit` says what shortened it.
    struct TransitionSpan {
        enum class Limit { None, FootageBefore, FootageAfter, ClipLength };
        qint64 cutMs = 0;
        qint64 durationMs = 0; // even; < Transition::kMinMs => it doesn't play
        Limit limit = Limit::None;
        bool plays() const { return durationMs >= Transition::kMinMs; }
        qint64 startMs() const { return cutMs - durationMs / 2; }
        qint64 endMs() const { return cutMs + durationMs / 2; }
    };
    // A zero span if the transition's clips aren't on the timeline.
    TransitionSpan transitionSpan(const Transition &t) const;
    // The source range main-track clip `clipId` shows: its trim, widened by
    // the footage the transitions at its two ends play past it (half of
    // each crossfade, at the clip's speed). False if there's no such clip.
    bool shownSourceRange(int clipId, qint64 &fromMs, qint64 &toMs) const;

    // --- extra tracks --------------------------------------------------
    // Ordered top to bottom below the main track, group by group
    // (Track::Group): the main video's stream tracks, the Audio group, then
    // each overlay track followed by its audio track.
    int trackCount() const { return static_cast<int>(m_tracks.size()); }
    const Track &trackAt(int index) const { return m_tracks.at(index); }
    static constexpr qint64 kInsetKeySnapMs = 20;
    static constexpr qint64 kInsetKeyDeleteSnapMs = 150;
    // The audio track holding the main video's `role` stream (Track::Role),
    // or 0 if there is none (yet).
    int streamTrackId(Track::Role role) const;
    // The audio track of overlay track `overlayTrackId`, or 0 if it has none
    // (yet).
    int audioTrackId(int overlayTrackId) const;
    // The first audio track (`video`: overlay track) that has nothing in
    // [startMs, endMs) and that `accept` takes; 0 if none.
    int freeTrackFor(qint64 startMs, qint64 endMs, bool video,
                     const std::function<bool(const Track &)> &accept) const;
    // The video clip audio clip `a` would link to: a main-track or overlay
    // clip of the same footage whose source range overlaps the audio's, the
    // one nearest on the timeline (the audio was unlinked from it, or cut
    // from it). 0 if none.
    int linkTargetFor(const TrackClip &a) const;
    int trackIndexOfId(int trackId) const;
    const Track *trackById(int trackId) const;
    // The track clip with this id, and (if non-null) the id of its track.
    const TrackClip *trackClipById(int clipId, int *trackId = nullptr) const;
    // Latest end of any clip on an extra track (0 if none).
    qint64 extraTracksEndMs() const;
    // Whether clip `clipId` may be moved onto track `trackId`. The video
    // and overlay groups' audio tracks hold only their own source's sound:
    // a stream track the main video's streams of its role, an overlay's
    // audio track the sound of a file on that overlay track. Anything goes
    // on an Audio-group track, except linked sound, which stays on its own
    // track (unlink it to use it freely). A clip may always stay on its
    // own track; an overlay clip goes on any overlay track.
    bool canHoldClip(int trackId, int clipId) const;

    // How many audio streams a source file has (InsvAudioDemux::
    // countAudioStreams; the model has no FFmpeg), which says which stream
    // is which mic (Track::roleOfStream). Unset, or 0 for a file: as many as
    // the timeline's clips of it use.
    void setAudioStreamCounter(std::function<int(const QString &)> counter)
    {
        m_audioStreamCounter = std::move(counter);
    }

public slots:
    // --- structure ------------------------------------------------------
    // Insert a clip at `index` (clamped to [0, count]). source* come from
    // stitch::ProbeClip, or for a `flat` video (ClipItem::flat) from
    // exportpipe::FlatVideoReader::probe. Trim starts as the whole source,
    // reframe path empty. A 360 clip's stabilization is the 360 clip before
    // it's, else the one after it's (Off without one); a flat clip's stays Off.
    // Its sound: addClipSound with `audioStreams`, the source's audio stream count (InsvAudioDemux::
    // countAudioStreams; 0 = none, or added by the caller -- a project load).
    // Returns the new clip's id.
    int insertClip(int index, const QString &sourcePath, qint64 sourceDurationMs,
                   double fps, int sourceWidth = 0, int sourceHeight = 0,
                   int audioStreams = 0, bool flat = false);
    // Append; same as insertClip(count(), ...).
    int appendClip(const QString &sourcePath, qint64 sourceDurationMs, double fps,
                   int sourceWidth = 0, int sourceHeight = 0, int audioStreams = 0,
                   bool flat = false);
    // Put each of a main-track clip's `streamCount` audio streams on its
    // stream track (Track::roleOfStream; the track is made when missing) as
    // an audio clip linked to it, covering its whole trimmed span. Streams
    // already linked to the clip are skipped. Returns the new clips' ids, by
    // stream (0 where none was added).
    QList<int> addClipSound(int clipId, int streamCount);
    // Put overlay clip `clipId`'s sound on its overlay track's audio track
    // (made when missing) as an audio clip linked to it, over the same span,
    // so it follows its moves, trims, splits and removal. Its sound already
    // on a track: that clip, rather than play it twice. Returns the audio
    // clip's id; 0 if `clipId` isn't an overlay clip. The caller checks the
    // file has sound.
    int addOverlaySound(int clipId);
    // Remove the clip with this id. Returns true if one was removed.
    bool removeClip(int clipId);
    // Reorder so the clip with this id ends up at `toIndex` (clamped to
    // [0, count-1]). Returns true if the order actually changed.
    bool moveClip(int clipId, int toIndex);
    // Split the clip at `atSourceMs` (its own source time). The clip keeps
    // [trimIn, atSourceMs]; a new clip with [atSourceMs, trimOut] and a copy
    // of the reframe track is inserted right after. Returns the new clip's id,
    // or 0 if the point isn't strictly inside the trim range (min gap each
    // side).
    int splitClip(int clipId, qint64 atSourceMs);
    // Insert a copy of a main-track clip right after it, with everything it
    // has (trim, speed, reframe keys, colour, stabilization, stitch,
    // privacy, fit) and a copy of its linked sound; the clips after it move
    // right. Returns the copy's id, or 0 if there's no such clip.
    int duplicateClip(int clipId);
    // Drop every clip, transition and extra track.
    void clear();

    // --- transitions ------------------------------------------------------
    // Put a transition of `type` at the cut after main-track clip
    // `leftClipId`, or change the one there; `durationMs` is clamped to
    // [Transition::kMinMs, kMaxMs]. False if there's no clip after it, or
    // nothing changed.
    bool setTransition(int leftClipId, Transition::Type type, qint64 durationMs);
    // Remove the transition at the cut after `leftClipId`. True if there was
    // one.
    bool removeTransition(int leftClipId);

    // --- extra tracks --------------------------------------------------
    // Add a track at the end of its group (Track::Group): a free audio
    // track at the bottom of the Audio group, a video track at the bottom of
    // the Overlay group. An empty name gets "Audio N" / "Overlay N". A stream
    // track (CameraMic / ExternalMic, audio only) goes first, the camera
    // mic's above the external mic's, named after its role when `name` is
    // empty. An OverlayAudio track goes right under its overlay track
    // `ownerTrackId` ("<overlay> audio" when unnamed); 0 if there's no such
    // overlay track. Sound linked to an overlay clip is put on its track's
    // audio track by the timeline itself, which makes the track when needed;
    // an audio track goes when its last clip is moved or removed. Returns the new track's id.
    // `id` > 0 gives the track that id instead of a new one (an undo putting
    // a track back as it was), unless a track has it already; ids handed out
    // later stay above it.
    int addTrack(Track::Kind kind, const QString &name = QString(),
                 Track::Role role = Track::Role::Free, int ownerTrackId = 0, int id = 0);
    bool removeTrack(int trackId); // and its clips; an overlay track's audio track too
    // Rename (trimmed; an empty name is refused). Returns true if it changed.
    // An overlay's audio track still called "<overlay> audio" follows it.
    bool setTrackName(int trackId, const QString &name);
    // Reorder within the track's group: it ends up at `toIndex`, clamped to
    // the group's span. An overlay track moves with its audio track, into the
    // place of the overlay track (or audio track) at `toIndex`; an
    // overlay's audio track only moves with its overlay. Returns true if the order changed.
    bool moveTrack(int trackId, int toIndex);
    // Track-level gain (clamped to audiolimits::k{Min,Max}GainDb) and mute.
    void setTrackAudio(int trackId, double gainDb, bool muted);
    // Put a clip on a track: `clip` as given (source, trims, level, fades,
    // start ...), with a fresh id; values are clamped. Returns the id, or 0 if
    // there's no such track.
    int insertTrackClip(int trackId, const TrackClip &clip);
    // The common case: the whole of `sourcePath` at `startMs`. 0 if the
    // track isn't an audio track.
    int addAudioClip(int trackId, const QString &sourcePath, qint64 sourceDurationMs,
                     qint64 startMs, int stream = -1);
    // The whole of video file `sourcePath` (display size w x h) at `startMs`
    // on a video track, placed at the default inset spot. 0 if the track
    // isn't a video track.
    int addVideoClip(int trackId, const QString &sourcePath, qint64 sourceDurationMs,
                     qint64 startMs, int sourceWidth, int sourceHeight);
    // A title (TrackClip::title) `spanMs` long at `startMs` on a video
    // track, centred, at title::kDefaultSize. 0 if the track isn't a video
    // track.
    int addTitleClip(int trackId, qint64 startMs, const title::Style &style,
                     qint64 spanMs = title::kDefaultSpanMs);
    // A title's text and how it's drawn. No-op on a clip that isn't one.
    void setTrackClipTitle(int clipId, const title::Style &style);
    bool removeTrackClip(int clipId);
    // Move a clip to `startMs` on the track `toTrackId` (may be its own;
    // canHoldClip must allow it -- else nothing happens). A linked clip is
    // locked in time to its video clip and stays on its own track (unlink,
    // move, link again to re-sync it -- linking keeps the position).
    void moveTrackClip(int clipId, int toTrackId, qint64 startMs);
    // Link an audio clip to `videoClipId` (0 = unlink): a main-track clip,
    // or an overlay clip (an overlay's own sound). The offset is chosen
    // so nothing moves in time; the audio takes the clip's speed, and goes
    // back onto its own track (made when missing) if it was moved off it.
    // Unlinking moves nothing. Returns false if either clip doesn't exist
    // (or the target is an audio clip).
    bool linkAudioClip(int audioClipId, int videoClipId);
    // Split the clip at timeline `atMs`: it keeps the part before (and its
    // fade-in), a new clip right after it on the same track gets the rest
    // (and the fade-out). Returns the new clip's id, or 0 if `atMs` isn't
    // strictly inside it (min gap each side).
    int splitTrackClip(int clipId, qint64 atMs);
    void setTrackClipTrimIn(int clipId, qint64 sourceMs);
    void setTrackClipTrimOut(int clipId, qint64 sourceMs);
    void setAudioClipLevel(int clipId, double gainDb, bool muted);
    void setAudioClipNoise(int clipId, const insv::NoiseReduction &nr);
    void setTrackClipFades(int clipId, qint64 fadeInMs, qint64 fadeOutMs);
    // A video-track clip's inset: its static look (used while it has no
    // keyframes) and frame, both clamped.
    void setTrackClipInset(int clipId, const inset::Look &look, const inset::Frame &frame);
    // Look keyframes, at the clip's source time. Upsert snaps to a key within
    // kInsetKeySnapMs; remove to one within kInsetKeyDeleteSnapMs.
    void upsertTrackClipInsetKey(int clipId, qint64 sourceMs, const inset::Look &look);
    void removeTrackClipInsetKeyNear(int clipId, qint64 sourceMs);
    // Drop every key; the clip keeps the look at `sourceMs` as its static one.
    void clearTrackClipInsetKeys(int clipId, qint64 sourceMs);
    // Ease of the move from the key at sourceMs (within kInsetKeyDeleteSnapMs,
    // else the last one before) to the next (keytrack::segmentAt).
    void setTrackClipInsetEase(int clipId, qint64 sourceMs, keytrack::Ease ease);

    // --- per-clip edits, addressed by clip id ----------------------------
    // positionMs / sourceMs are in the clip's own source time (0 .. its source
    // duration).
    void setClipTrimIn(int clipId, qint64 positionMs);
    void setClipTrimOut(int clipId, qint64 positionMs);
    void clearClipTrim(int clipId);
    void upsertClipReframeKey(int clipId, qint64 sourceMs, ReframePose pose);
    void removeClipReframeKeyNear(int clipId, qint64 sourceMs);
    // Ease of the move from the key at sourceMs (within
    // ClipItem::kReframeDeleteSnapMs, else the last one before) to the next.
    void setClipReframeEase(int clipId, qint64 sourceMs, keytrack::Ease ease);
    void clearClipReframe(int clipId);
    // Replace the clip's keys in [fromMs, toMs] (source time) with `keys` --
    // one edit, one undo step (Track subject writes its path this way).
    void replaceClipReframeSpan(int clipId, qint64 fromMs, qint64 toMs,
                                const QList<ReframeKey> &keys);
    // Playback speed (clamped to ClipItem::kMin/MaxSpeed). Changes the clip's
    // span on the timeline: emits clipSpeedChanged + durationChanged.
    void setClipSpeed(int clipId, double speed);
    // How a flat clip fills the output frame (ClipItem::fit); the Video tab
    // edits it. No-op on a 360 clip.
    void setClipFit(int clipId, ClipItem::Fit fit);
    // Colour grade (clamped to ColorGrade's own ranges); the Colour panel edits
    // all four fields together.
    void setClipColor(int clipId, const ColorGrade &grade);
    // Gyro stabilization (ClipItem::stabilization); the Stabilization panel
    // edits it.
    void setClipStabilization(int clipId, const insv::StabConfig &config);
    // Privacy blur on / off (ClipItem::blurPlates); the Privacy panel edits it.
    void setClipBlurPlates(int clipId, bool on);
    // The clip's plate scan (ClipItem::plateScan), set when one is made.
    // Emits clipPrivacyChanged too.
    void setClipPlateScan(int clipId, const QString &key);
    // The blur tracks removed from the clip by hand (ClipItem::blurRemovals).
    // Emits clipPrivacyChanged too.
    void setClipBlurRemovals(int clipId, const std::vector<privacy::BlurRemoval> &removals);
    // The plates blurred by hand (ClipItem::blurAdditions). Emits
    // clipPrivacyChanged too.
    void setClipBlurAdditions(int clipId, const std::vector<privacy::BlurAddition> &additions);
    // Mark whether a clip's source file is currently resolvable. Set by the
    // project loader; the timeline widget will show missing clips differently.
    void setClipMediaMissing(int clipId, bool missing);
    // Every clip (main track and extra tracks) cut from `from` now reads
    // `to` (a moved or renamed file found again); their mediaMissing is set
    // from whether `to` exists. Emits clipsReordered / tracksChanged for the
    // tracks that had one. Returns how many clips changed.
    int relinkSource(const QString &from, const QString &to);
    // Set mediaMissing from `missing` (path -> missing) on every clip, main
    // track and extra tracks, cut from a path it names. Emits
    // mediaMissingChanged() -- not an edit, nothing else. Returns the paths
    // whose clips changed.
    QStringList setSourcesMissing(const QHash<QString, bool> &missing);
    // The stitch settings (ClipItem::lensGuard / depthSeam) for every clip
    // cut from `sourcePath`; the Stitching panel edits them. Emits
    // clipStitchChanged per clip that changed.
    void setSourceStitch(const QString &sourcePath, stitch::PreviewStitchConfig::LensGuard lensGuard,
                         bool depthSeam);
    // Measured sync fine-tune (ClipItem::syncAdjustMs) for every clip cut from
    // `sourcePath`.
    void setSourceSyncAdjust(const QString &sourcePath, double ms);
    // Measured seam fine-tune (ClipItem::seamAdjustDeg / seamAdjustGuard) for
    // every clip cut from `sourcePath`.
    void setSourceSeamAdjust(const QString &sourcePath, const std::array<double, 3> &deg,
                             int lensGuard);
    // A source's sync fine-tune: the first main-track clip cut from it; NaN
    // when it has none (not measured, or no clip of it).
    double sourceSyncAdjust(const QString &sourcePath) const;
    // A source's seam fine-tune if one of its main-track clips has it
    // measured under `lensGuard`; else false and `deg` zero.
    bool sourceSeamAdjust(const QString &sourcePath, int lensGuard,
                          std::array<double, 3> &deg) const;
    // The same, measured under the lens guards the source is set to now
    // (ClipItem::lensGuard): what its stitch uses.
    bool sourceSeamAdjust(const QString &sourcePath, std::array<double, 3> &deg) const;

signals:
    void clipInserted(int index);
    void clipRemoved(int id);
    void clipsReordered(); // also emitted by clear() as a wholesale-change notice
    // One clip's trim range changed. Carries the new range so a consumer can
    // forward it without re-reading.
    void clipTrimChanged(int id, qint64 inMs, qint64 outMs);
    // One clip's reframe track changed (key added / moved / removed / cleared).
    void clipReframeChanged(int id);
    // One flat clip's fit changed. Carries the new value.
    void clipFitChanged(int id, ClipItem::Fit fit);
    // One clip's colour grade changed. Carries the new value.
    void clipColorChanged(int id, ColorGrade grade);
    // One clip's stabilization changed. Carries the new value.
    void clipStabilizationChanged(int id, insv::StabConfig config);
    // One clip's privacy blur was switched on / off, or its removed tracks /
    // hand-added blurs changed.
    void clipPrivacyChanged(int id, bool blurPlates);
    // One clip's playback speed changed (its timeline span changed with it).
    void clipSpeedChanged(int id, double speed);
    // One clip's stitch settings (lens guards, depth-aware seam) changed.
    void clipStitchChanged(int id);
    // One clip's measured sync fine-tune changed.
    void clipSyncChanged(int id, double ms);
    // One clip's measured seam fine-tune changed.
    void clipSeamChanged(int id);
    // Total timeline duration changed (append / clear / a trim edit).
    void durationChanged(qint64 totalMs);
    // Anything about the extra tracks changed: a track added / removed /
    // re-levelled, or a clip on one added / removed / moved / trimmed /
    // re-levelled. Coarse on purpose -- every consumer rebuilds from scratch.
    void tracksChanged();
    // Only clips' mediaMissing changed (setSourcesMissing): consumers that
    // skip or mark missing clips refresh.
    void mediaMissingChanged();
    // A transition was added, changed or removed -- by hand, or dropped
    // because its cut went (a clip moved or removed). A trim or speed change
    // can change what one plays as (transitionSpan) without this: it comes
    // with its own signal.
    void transitionsChanged();

private:
    ClipItem *mutableClipById(int id);
    // What linked audio follows: a main-track clip or an overlay clip. Its
    // timeline start, trims and speed; false if `clipId` is neither.
    struct LinkAnchor {
        qint64 startMs = 0;
        qint64 trimInMs = 0;
        qint64 trimOutMs = 0;
        double speed = 1.0;
    };
    bool linkAnchor(int clipId, LinkAnchor *anchor = nullptr) const;
    // Linked audio. After any edit to what it follows: re-derive every linked
    // clip's speed and start, unlink any whose video clip is gone, and
    // tidyOverlayAudio(). Returns true if anything changed (the caller emits
    // tracksChanged).
    bool relinkAudio();
    // Sound linked to an overlay clip onto that overlay track's audio track
    // (made when missing); an audio track left empty by that goes. True if
    // anything changed.
    bool tidyOverlayAudio();
    // Remove those of these tracks that are empty overlay audio tracks.
    void dropEmptyOverlayAudioTracks(const QList<int> &trackIds);
    // Insert `t` at the end of its group (see addTrack). No signal.
    void placeTrack(const Track &t);
    // A video clip's trim went from [oldIn, oldOut] to its current range:
    // linked audio edges that sat on the old edges follow; the rest are
    // clamped into the new range; audio left with nothing inside it is
    // removed. Returns true if anything changed.
    bool followTrim(int videoClipId, qint64 oldIn, qint64 oldOut);
    // Video clip `leftId` was split at its source time `atSourceMs` into
    // itself and `rightId`: its linked audio past the cut goes with the right
    // part, split at the same source instant if it spans it.
    bool splitLinkedAudio(int leftId, int rightId, qint64 atSourceMs);
    // Remove every audio clip linked to this video clip; the ids of the
    // tracks it was on are added to `tracks`.
    bool removeLinkedAudio(int videoClipId, QList<int> *tracks = nullptr);
    TrackClip *mutableTrackClipById(int id, Track **track = nullptr);
    Track *mutableTrackById(int trackId);
    // The mic a main-video stream clip is (Track::roleOfStream); Free when
    // it isn't one (stream < 0) or its kind isn't known.
    Track::Role streamRole(const TrackClip &a) const;
    // Move clip `clipId` from `from` to `to` keeping its time. No signal.
    void transferTrackClip(int clipId, Track *from, Track *to);

    // Drop the transitions whose cut is gone (their clips no longer next to
    // each other). True if any went.
    bool pruneTransitions();
    // The transition at the cut after `fromLeftId` now belongs to the cut
    // after `toLeftId` (a split or a copy put a clip in between): the clip
    // after it stays the same. True if there was one. No signal.
    bool moveTransitionLeft(int fromLeftId, int toLeftId);

    QList<ClipItem> m_clips;
    QList<Transition> m_transitions;
    QList<Track> m_tracks;
    int m_nextId = 1; // shared by main and audio clips: ids are timeline-unique
    int m_nextTrackId = 1;
    std::function<int(const QString &)> m_audioStreamCounter;
};
