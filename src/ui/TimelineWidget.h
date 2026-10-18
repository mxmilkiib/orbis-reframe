#pragma once

#include <QSet>
#include <QVector>
#include <QWidget>

class QLineEdit;
class QMimeData;
class QPainter;
class QScrollBar;
class QTimer;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// The timeline: a ruler, the main video track (clip blocks sized by timeline
// span, butt-joined; picture only; a transition shows as a box over its
// cut, as long as it plays) and the extra tracks below it
// in three groups, a little apart: its own sound (the stream tracks), the
// Audio group, and the overlay (video) tracks, each followed by its sound
// track. Each row has a header on the left; a playhead runs over all of them.
// A video track's own sound tracks (the main track's stream tracks, an
// overlay track's audio track) can be folded away under it.
// Everything here is in *timeline time*.
//
// Pure view. MainWindow feeds it the clips, tracks and playhead and turns its
// signals into Timeline / PlaybackController calls; the widget never touches
// the model. Interactions:
//   * click on the ruler               -> seekRequested on press (exact frame)
//   * drag on the ruler                -> then scrubRequested while it moves,
//                                        and seekRequested on release. The
//                                        ruler is the only thing that moves
//                                        the playhead; clicks on the track
//                                        rows select, they don't seek.
//   Main track:
//   * drag a clip's left/right edge    -> clipTrimIn/OutRequested (source ms)
//   * click a clip                     -> clipSelected (no seek)
//   * drag a clip body                 -> clipMoved (reorder)
//   * right-click a clip               -> split / speed / move / remove,
//                                        insert a clip before / after it,
//                                        copy / paste its colour grade or
//                                        its stabilization (or paste
//                                        either to all clips)
//   * right-click elsewhere            -> "Add clip here…" at the nearest
//                                        cut (also on an empty timeline)
//   * click the main track's header    -> fold its sound tracks away / back
//                                        -> soundCollapsedToggled (they
//                                        still play and export)
//   Audio and overlay tracks:
//   * click a clip                     -> trackClipSelected (no seek)
//   * drag a clip body                 -> trackClipMoved (any track of its
//                                        kind; snaps to the playhead, cuts,
//                                        edges).
//                                        A linked clip keeps its time: it
//                                        only changes track.
//   * drag a clip's edge               -> trackClipTrimIn/OutRequested
//   * an overlay track's triangle      -> fold its audio track away / back
//                                        -> soundCollapsedToggled
//   * header "M" box                   -> trackMuteToggled ("H" on an
//                                        overlay track: hide it). The main
//                                        track has none: its sound is on
//                                        the stream tracks under it.
//   * drag a header up / down          -> trackMoved (reorder, within its
//                                        group; an overlay track takes its
//                                        audio track along, which can't be
//                                        dragged on its own)
//   * double-click a header            -> rename in place: Enter commits,
//                                        Escape cancels -> trackRenameRequested
//   * right-click                      -> split a clip here / remove it /
//                                        put an overlay's sound on an audio
//                                        track / add an audio file here / add,
//                                        rename, move or remove a track /
//                                        zoom
//   Files: audio / video files dragged in from a file manager drop at the
//   pointer: onto an Audio-group / overlay track, or anywhere else for the
//   first such track with room (or a new one) -> audioFilesDropped /
//   videoFilesDropped. .insv clips go onto the main track at the cut
//   nearest the pointer, also on an empty timeline -> clipFilesDropped.
//   Titles: dragged from the Titles tab, onto an overlay track like a video
//   file -> titleDropped (not on an empty timeline).
//   Transitions: dragged from the Transitions tab onto the main track, at
//   the cut nearest the pointer -> transitionDropped (with two clips or
//   more). A transition's box: click -> transitionSelected; right-click ->
//   remove it; its tooltip says what it is.
//   Waveforms: a clip given `peaks` draws its audio's level (dB scale), with
//   its gain and fades applied.
//   Reframe keys: a main-track clip's keys show as diamonds along its bottom;
//   a clip without any is labelled with a hollow diamond (and says why in
//   its tooltip) -- it exports a static forward view.
//   Zoom: Ctrl+wheel (around the pointer) or zoomIn / zoomOut / zoomToFit;
//   the wheel or the scrollbar under the tracks pans.
//   Height: whatever the window gives it; rows that don't fit scroll up and
//   down under the ruler (the wheel over the headers, Shift+wheel, or the
//   scrollbar at the right). While zoomed, the view
//   follows the playhead when it runs off screen, and a drag past either
//   edge scrolls.
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // A clip's waveform: peak level per 1/kPeaksPerSec of source time (see
    // insv::AudioPeaks). Null = none (yet).
    static constexpr int kPeaksPerSec = 100;
    using Peaks = std::shared_ptr<const std::vector<float>>;

    struct Clip {
        int id = 0;
        QString label;
        qint64 trimInMs = 0;
        qint64 trimOutMs = 0;
        qint64 sourceDurationMs = 0;
        bool missing = false;
        QVector<qint64> keySourceMs; // reframe key times, in the clip's source time
        double speed = 1.0;          // ClipItem::speed
        // Span on the timeline, same rounding as ClipItem::timelineDurationMs.
        qint64 timelineSpanMs() const
        {
            return std::max<qint64>(0, std::llround((trimOutMs - trimInMs) / speed));
        }
    };

    struct TrackClipView {
        int id = 0;
        QString label;
        qint64 startMs = 0;
        qint64 trimInMs = 0;
        qint64 trimOutMs = 0;
        qint64 sourceDurationMs = 0;
        double speed = 1.0;
        qint64 fadeInMs = 0;
        qint64 fadeOutMs = 0;
        bool muted = false;
        bool missing = false;
        bool linked = false;   // follows a video clip
        bool linkable = false; // could be linked: a video clip shows the same footage
        Peaks peaks;
        double gain = 1.0; // the clip's level, linear (not the track's)
        QVector<qint64> keySourceMs; // an overlay's look keys, in its source time
        // An overlay whose file has sound: the context menu offers to put it
        // on an audio track.
        bool hasSound = false;
        // The other tracks it may be dragged onto (Timeline::canHoldClip).
        QVector<int> moveTargets;
        qint64 spanMs() const { return std::llround((trimOutMs - trimInMs) / speed); }
        qint64 endMs() const { return startMs + spanMs(); }
    };
    struct TrackView {
        enum class Group { Main, Audio, Overlay }; // as Track::Group
        int id = 0;
        QString name;
        bool muted = false; // an overlay track: hidden
        bool video = false; // an overlay track
        Group group = Group::Audio;
        bool overlayAudio = false; // an overlay track's audio track, right under it
        // (Main: the video track's mics. Both have their names indented.)
        QVector<TrackClipView> clips;
    };

    // A transition at a cut (Transition.h).
    struct TransitionView {
        int leftClipId = 0;    // at the cut after this clip
        qint64 durationMs = 0; // what plays (Timeline::transitionSpan); 0 = it can't
        bool dip = false;      // a dip to black, else a crossfade
        QString toolTip;       // what it is, and why it's shorter than set
    };

    void setClips(const QVector<Clip> &clips);
    void setTransitions(const QVector<TransitionView> &transitions);
    void setSelectedTransition(int leftClipId); // 0 = none; does not emit
    // Top to bottom, group by group (TrackView::Group).
    void setTracks(const QVector<TrackView> &tracks);
    void setPlayhead(qint64 timelineMs);
    qint64 playhead() const { return m_playheadMs; }
    void setActiveClip(int clipId); // -1 = none
    void setSelectedTrackClip(int clipId); // -1 = none; does not emit
    void setCanPasteGrade(bool canPaste);  // a colour grade has been copied
    void setCanPasteStabilization(bool canPaste); // a stabilization setting has been copied
    // A video track's own sound tracks folded away under it (view only;
    // doesn't emit). `videoTrackId` 0 = the main track; else an overlay
    // track's id.
    void setSoundCollapsed(int videoTrackId, bool collapsed);
    bool isSoundCollapsed(int videoTrackId) const { return m_collapsedSound.contains(videoTrackId); }

public slots:
    void zoomIn();   // around the playhead if it's in view, else the centre
    void zoomOut();
    void zoomToFit();

signals:
    void seekRequested(qint64 timelineMs);  // exact: a press, a drag's release
    void scrubRequested(qint64 timelineMs); // while dragging: fast preview
    void clipTrimInRequested(int clipId, qint64 newTrimInMs);
    void clipTrimOutRequested(int clipId, qint64 newTrimOutMs);
    void clipMoved(int clipId, int toIndex);
    void clipRemoveRequested(int clipId);
    void clipSplitRequested(int clipId, qint64 atSourceMs);
    void clipDuplicateRequested(int clipId);
    void clipSpeedRequested(int clipId, double speed);
    void gradeCopyRequested(int clipId);
    void gradePasteRequested(int clipId);
    void gradePasteAllRequested();
    void stabilizationCopyRequested(int clipId);
    void stabilizationPasteRequested(int clipId);
    void stabilizationPasteAllRequested();
    // "Add clip here…" / "Insert clip before/after…": a file picker, then the
    // clip inserted on the main track at position `index`.
    void addClipRequested(int index);
    // .insv files dropped from a file manager: inserted on the main track at
    // position `index`, in order.
    void clipFilesDropped(const QStringList &paths, int index);
    void clipSelected(int clipId); // a video clip clicked; 0 = a click on empty space
    void transitionSelected(int leftClipId); // a transition's box clicked
    void transitionRemoveRequested(int leftClipId);
    // A transition dragged from the Transitions tab (Transition::kMimeType;
    // `type` its data) onto the cut after clip `leftClipId`.
    void transitionDropped(int leftClipId, const QString &type);

    void trackClipSelected(int clipId); // -1 = selection cleared
    void trackClipMoved(int clipId, int toTrackId, qint64 startMs);
    void trackClipTrimInRequested(int clipId, qint64 sourceMs);
    void trackClipTrimOutRequested(int clipId, qint64 sourceMs);
    void trackClipRemoveRequested(int clipId);
    void trackClipSplitRequested(int clipId, qint64 atTimelineMs);
    void audioClipLinkRequested(int clipId, bool link); // link to its video clip / unlink
    void overlayAudioRequested(int clipId); // put an overlay's sound on an audio track
    void trackMuteToggled(int trackId, bool muted);
    void trackRemoveRequested(int trackId);
    void trackRenameRequested(int trackId, const QString &name);
    void trackMoved(int trackId, int toIndex);
    void soundCollapsedToggled(int videoTrackId, bool collapsed); // by the user; 0 = main track
    void addAudioTrackRequested();
    void addVideoTrackRequested();
    // "Add audio file here…": a file picker, then a clip at `atMs` on the
    // track (0 = a new track).
    void addAudioFileRequested(int trackId, qint64 atMs);
    // Files dropped from a file manager at `atMs`, onto track `trackId` (0 =
    // the first track with room there, else a new one).
    void audioFilesDropped(const QStringList &paths, int trackId, qint64 atMs);
    // The same for video files, onto an overlay track.
    void addVideoFileRequested(int trackId, qint64 atMs);
    void videoFilesDropped(const QStringList &paths, int trackId, qint64 atMs);
    // A title dragged from the Titles tab (title::kMimeType), onto overlay
    // track `trackId` (0 = the first with room there, else a new one).
    void titleDropped(int trackId, qint64 atMs);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dragLeaveEvent(QDragLeaveEvent *) override;
    void dropEvent(QDropEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    bool event(QEvent *) override; // tooltips
    bool eventFilter(QObject *watched, QEvent *event) override; // the rename box's Escape

private:
    struct Row { // a main-track clip, laid out
        Clip clip;
        qint64 startMs = 0;
        qint64 durMs = 0;
        QRect rect;
    };
    struct Hit { // what's under a point
        enum Kind { None, Ruler, Header, MainClip, MainEmpty, TrackClip, TrackEmpty, Transition } kind = None;
        int row = -1;   // main: index into m_rows; a transition: into m_transitions
        int track = -1; // audio: index into m_tracks
        int clip = -1;  // audio: index into that track's clips
    };

    void rebuildLayout();
    // Each extra track's row (m_trackY, m_shown), after the tracks or a
    // folded state changed.
    void layoutRows();
    bool trackShown(int index) const { return m_shown[index]; }
    // The main track (-1) or an overlay track has sound tracks to fold.
    bool hasOwnSound(int trackIndex) const;
    int videoTrackIdAt(int trackIndex) const { return trackIndex < 0 ? 0 : m_tracks[trackIndex].id; }
    // The triangle in a header (the main track's: trackIndex -1).
    QRect foldBox(int trackIndex) const;
    void toggleSound(int trackIndex); // by the user: emits
    // An overlay track's layer: 1 = in front, counting down the Overlay
    // group (the first overlay track draws on top); *count the overlay
    // tracks. 0 for any other track.
    int overlayLayer(int trackIndex, int *count = nullptr) const;
    // The layer number's badge in an overlay track's header.
    QRect layerBox(int trackIndex) const;
    QString layerToolTip(int trackIndex) const;
    Hit hitTest(QPoint pos) const;
    // Main-track insert position for a clip added at `ms`: the cut nearest to it.
    int insertIndexAt(qint64 ms) const;
    // Transition `index`'s box over its cut; null if its clip isn't laid out.
    QRect transitionRect(int index) const;
    int trackTop(int trackIndex) const; // y of an extra track's row
    QRect trackClipRect(int trackIndex, const TrackClipView &c) const;
    int trackAtY(int y) const; // index, or -1
    qint64 timelineMsAt(int x) const;       // clamped to [0, extent]
    int xForTimelineMs(qint64 ms) const;    // may lie off screen when zoomed
    int contentLeft() const;
    int contentWidth() const;
    // Zoom: the span of timeline shown across the content width.
    qint64 visibleSpanMs() const;
    bool zoomed() const { return visibleSpanMs() < std::max<qint64>(m_extentMs, 1); }
    void zoomBy(double factor, int anchorX); // > 1 zooms in; anchorX keeps its time
    void scrollToMs(qint64 viewStartMs);
    void clampView();
    void updateScrollBar();
    // The drag in progress, to `pos` (also re-run by the edge auto-scroll).
    void dragTo(QPoint pos);
    // A file drag over the timeline at `pos`: where it would land.
    void dropTo(QPoint pos);
    // How far one auto-scroll tick moves the view for a drag held at `pos`
    // (x in pixels of timeline, y in pixels of rows; negative = back / up);
    // null when it's not near an edge it can scroll past.
    QPoint edgeScrollStep(QPoint pos) const;
    void autoScrollTick();
    int trackGapAtY(int y) const; // insertion gap for a header drag, [0, tracks]
    // The span [first, end) of track `index`'s group in m_tracks.
    void groupSpan(int index, int *first, int *end) const;
    // An overlay track and its audio track(s): the index just past them.
    int unitEnd(int index) const;
    void startRename(int trackIndex);
    void finishRename();
    // Timeline ms at pixel x, unclamped and fractional (waveform columns).
    double msAtX(double x) const;
    // Draw a clip's waveform into `rc` (its full, possibly off-screen,
    // rect): `startMs` its timeline start, source time trimInMs + local *
    // speed; `gain` linear; fades in timeline ms.
    void drawWaveform(QPainter &p, const QRect &rc, const Peaks &peaks, qint64 startMs,
                      qint64 trimInMs, double speed, double gain, qint64 fadeInMs,
                      qint64 fadeOutMs, qint64 spanMs, const QColor &color) const;
    // The files of one kind among a drag's URLs.
    enum class DropKind { Audio, Overlay, Clip };
    static QStringList droppableFiles(const QMimeData *mime, DropKind kind);
    int dropIndexForX(int x) const;         // gap index in [0, count]
    // `ms` snapped to the nearest playhead / cut / audio edge within a few px
    // (excluding the clip being dragged).
    qint64 snapMs(qint64 ms, int excludeClipId) const;
    int mainTop() const;
    int rowsHeight() const;   // every row, unscrolled
    int scrollBarTop() const; // the bottom of the rows' view
    int maxViewTop() const;   // how far the rows can scroll up
    void scrollRowsBy(int dy);

    QVector<Clip> m_clips;
    QVector<Row> m_rows;
    QVector<TransitionView> m_transitions;
    int m_selectedTransition = 0; // its leftClipId
    QVector<TrackView> m_tracks;
    QVector<int> m_trackY;  // each track's row, y under the main track's top
    QVector<bool> m_shown;  // ... not folded away
    int m_rowsEndY = 0;     // ... and the end of the last one
    QSet<int> m_collapsedSound; // video track ids, 0 = the main track
    qint64 m_totalMs = 0;  // the main track: the timeline's length
    qint64 m_extentMs = 0; // what the view spans: the main track, or longer audio
    qint64 m_playheadMs = 0;
    int m_activeClipId = -1;
    bool m_canPasteGrade = false;
    bool m_canPasteStab = false;
    int m_selectedAudioId = -1;

    enum class Drag {
        None, Seek, TrimIn, TrimOut, Reorder, AudioMove, AudioTrimIn, AudioTrimOut, TrackMove
    };
    Drag m_drag = Drag::None;
    int m_dragRow = -1;
    QPoint m_pressPos;
    qint64 m_pressMs = 0;  // timeline ms under the press (the view may scroll)
    QPoint m_lastDragPos; // also a file drag's, for the auto-scroll
    bool m_dragMoved = false;
    qint64 m_provTrimMs = 0; // provisional trim edge during a TrimIn/TrimOut drag
    int m_reorderTarget = -1;
    // Audio drags: the clip (a copy, as at the press) and its provisional
    // place.
    TrackClipView m_dragAudioOrig;
    TrackClipView m_dragAudio; // provisional: moved start / trims
    int m_dragAudioTrack = -1;
    qint64 m_provStartMs = 0;
    int m_provTrack = -1;
    // Header drag (track reorder): the track and the gap it would drop into.
    int m_dragTrackIndex = -1;
    int m_trackGap = -1;

    // Zoom / scroll.
    qint64 m_visibleMs = 0; // 0 = fit the whole extent
    qint64 m_viewStartMs = 0;
    QScrollBar *m_hscroll = nullptr;
    // Rows scrolled up under the ruler, in pixels, when they don't fit.
    int m_viewTopPx = 0;
    QScrollBar *m_vscroll = nullptr;
    QTimer *m_autoScroll = nullptr;

    // A file drag hovering over the timeline: where it would land.
    bool m_dropActive = false;
    int m_dropTrack = -1; // track index, -1 = first with room / new
    bool m_dropVideo = false; // the drag carries video files (else audio)
    bool m_dropClip = false;  // ... .insv clips, for the main track
    bool m_dropTitle = false; // ... a title, for an overlay track
    bool m_dropTransition = false; // ... a transition, for a cut (m_dropIndex: the clip after it)
    bool m_dropMain = false;  // the video files go on the main track (pointer over it, or no clips yet)
    int m_dropIndex = 0;      // the main-track position they'd go in at
    qint64 m_dropMs = 0;

    // In-place rename of a track.
    QLineEdit *m_renameEdit = nullptr;
    int m_renameTrackId = 0;
    bool m_renameCancelled = false;
};
