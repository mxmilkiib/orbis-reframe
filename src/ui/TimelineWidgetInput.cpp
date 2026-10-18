#include "TimelineWidget.h"

#include "Theme.h"
#include "TimelineWidgetLayout.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <climits>
#include <cmath>

#include "../core/MediaPool.h"  // media::extensions
#include "../core/Title.h"      // title::kMimeType
#include "../core/Transition.h" // Transition::kMimeType

// The timeline's input: files dropped on it, the mouse (select, seek, trim,
// move, reorder, scrub, scroll near the edges), renaming a track, tooltips
// and the right-click menus.

using namespace timelinelayout;

QStringList TimelineWidget::droppableFiles(const QMimeData *mime, DropKind kind)
{
    // Flat video for an overlay or the main track (dropTo decides which);
    // 360 clips (the master, not the .lrv proxy) for the main track.
    const QStringList &ext = media::extensions(kind == DropKind::Clip      ? MediaItem::Kind::Clip360
                                               : kind == DropKind::Overlay ? MediaItem::Kind::Video
                                                                           : MediaItem::Kind::Audio);
    QStringList out;
    if (!mime || !mime->hasUrls())
        return out;
    for (const QUrl &u : mime->urls())
        if (u.isLocalFile() && ext.contains(QFileInfo(u.toLocalFile()).suffix().toLower()))
            out << u.toLocalFile();
    return out;
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent *e)
{
    // Audio needs a video track to lie under (the main track sets the
    // length); clips and flat videos can start one.
    const bool audio = !droppableFiles(e->mimeData(), DropKind::Audio).isEmpty();
    const bool video = !droppableFiles(e->mimeData(), DropKind::Overlay).isEmpty();
    const bool clip = !droppableFiles(e->mimeData(), DropKind::Clip).isEmpty();
    // A title, like an overlay video, but only over a timeline.
    const bool title = e->mimeData()->hasFormat(QString::fromLatin1(title::kMimeType));
    // A transition: onto a cut, so two clips at least.
    const bool transition = e->mimeData()->hasFormat(QString::fromLatin1(Transition::kMimeType));
    if (transition ? m_rows.size() < 2
        : title    ? m_rows.isEmpty()
                   : !clip && !video && (m_rows.isEmpty() || !audio)) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    m_dropActive = true;
    m_dropTransition = transition;
    m_dropTitle = title && !transition;
    m_dropClip = clip && !title; // a mix goes by the clip rules, then the audio rules
    m_dropVideo = title || (video && !audio);
    dragMoveEvent(e);
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent *e)
{
    if (!m_dropActive)
        return;
    m_lastDragPos = e->position().toPoint();
    dropTo(m_lastDragPos);
    e->acceptProposedAction();
    if (!edgeScrollStep(m_lastDragPos).isNull() && !m_autoScroll->isActive())
        m_autoScroll->start();
}

void TimelineWidget::dropTo(QPoint pos)
{
    // Flat videos alone go on the main track over it, or when it's empty;
    // elsewhere onto an overlay track. With .insv clips they're overlays.
    m_dropMain = m_dropVideo && !m_dropClip && !m_dropTitle &&
                 (m_rows.isEmpty() || (pos.y() >= mainTop() - kRowGap / 2 &&
                                       pos.y() < mainTop() + kMainH + kRowGap / 2));
    if (m_dropTransition) {
        // The cut nearest the pointer, between two clips.
        m_dropTrack = -1;
        m_dropIndex = std::clamp(insertIndexAt(timelineMsAt(pos.x())), 1,
                                 std::max(1, int(m_rows.size()) - 1));
        m_dropMs = m_dropIndex < m_rows.size() ? m_rows[m_dropIndex].startMs : m_totalMs;
    } else if (m_dropClip || m_dropMain) {
        // Between clips, at the cut nearest the pointer.
        m_dropTrack = -1;
        m_dropIndex = insertIndexAt(timelineMsAt(pos.x()));
        m_dropMs = m_dropIndex < m_rows.size() ? m_rows[m_dropIndex].startMs : m_totalMs;
    } else {
        m_dropTrack = trackAtY(pos.y());
        if (m_dropTrack >= 0 && (m_dropVideo ? !m_tracks[m_dropTrack].video
                                             : m_tracks[m_dropTrack].group != TrackView::Group::Audio))
            m_dropTrack = -1; // not a track for these files: a video's own sound, or the other kind
        m_dropMs = std::min(snapMs(timelineMsAt(pos.x()), -1), m_totalMs);
    }
    update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropActive = false;
    m_autoScroll->stop();
    update();
}

void TimelineWidget::dropEvent(QDropEvent *e)
{
    const QStringList audio = droppableFiles(e->mimeData(), DropKind::Audio);
    const QStringList video = droppableFiles(e->mimeData(), DropKind::Overlay);
    const QStringList clips = droppableFiles(e->mimeData(), DropKind::Clip);
    const bool active = m_dropActive;
    m_dropActive = false;
    m_autoScroll->stop();
    update();
    if (active && m_dropTransition) {
        if (m_dropIndex < 1 || m_dropIndex >= m_rows.size())
            return;
        e->acceptProposedAction();
        emit transitionDropped(
            m_rows[m_dropIndex - 1].clip.id,
            QString::fromLatin1(e->mimeData()->data(QString::fromLatin1(Transition::kMimeType))));
        return;
    }
    if (active && m_dropTitle) {
        e->acceptProposedAction();
        const int trackId =
            m_dropTrack >= 0 && m_dropTrack < m_tracks.size() ? m_tracks[m_dropTrack].id : 0;
        emit titleDropped(trackId, m_dropMs);
        return;
    }
    if (!active || (audio.isEmpty() && video.isEmpty() && clips.isEmpty()))
        return;
    e->acceptProposedAction();
    const int trackId = m_dropTrack >= 0 && m_dropTrack < m_tracks.size() ? m_tracks[m_dropTrack].id : 0;
    // Clips first: audio and overlays dropped with them start where they do.
    if (!clips.isEmpty())
        emit clipFilesDropped(clips, m_dropIndex);
    if (!video.isEmpty() && m_dropMain)
        emit clipFilesDropped(video, m_dropIndex);
    if (!audio.isEmpty())
        emit audioFilesDropped(audio, m_dropVideo ? 0 : trackId, m_dropMs);
    if (!video.isEmpty() && !m_dropMain)
        emit videoFilesDropped(video, m_dropVideo ? trackId : 0, m_dropMs);
}

void TimelineWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_pressPos = m_lastDragPos = e->pos();
    m_pressMs = timelineMsAt(e->pos().x());
    m_dragMoved = false;
    m_dragRow = -1;
    m_reorderTarget = -1;
    m_drag = Drag::None;

    const Hit h = hitTest(e->pos());
    switch (h.kind) {
    case Hit::Header:
        if (h.track < 0) { // the main video track: folds its sound tracks away / back
            if (hasOwnSound(-1))
                toggleSound(-1);
            return;
        }
        if (hasOwnSound(h.track) && foldBox(h.track).contains(e->pos())) {
            toggleSound(h.track);
            return;
        }
        if (muteBox(trackTop(h.track), kTrackH).contains(e->pos())) {
            emit trackMuteToggled(m_tracks[h.track].id, !m_tracks[h.track].muted);
            return;
        }
        if (m_tracks[h.track].overlayAudio)
            return; // it moves with its overlay track
        m_drag = Drag::TrackMove; // a drag up / down reorders
        m_dragTrackIndex = h.track;
        m_trackGap = -1;
        return;
    case Hit::Transition:
        emit transitionSelected(m_transitions[h.row].leftClipId);
        return;
    case Hit::MainClip: {
        m_dragRow = h.row;
        emit clipSelected(m_rows[h.row].clip.id);
        const QRect &rc = m_rows[m_dragRow].rect;
        if (std::abs(e->pos().x() - rc.left()) <= kEdgeGrabPx) {
            m_drag = Drag::TrimIn;
            m_provTrimMs = m_rows[m_dragRow].clip.trimInMs;
            return;
        }
        if (std::abs(e->pos().x() - rc.right()) <= kEdgeGrabPx) {
            m_drag = Drag::TrimOut;
            m_provTrimMs = m_rows[m_dragRow].clip.trimOutMs;
            return;
        }
        m_drag = Drag::Reorder; // a click without a drag just selects (no seek)
        return;
    }
    case Hit::TrackClip: {
        const TrackClipView &c = m_tracks[h.track].clips[h.clip];
        emit trackClipSelected(c.id);
        m_dragAudioOrig = m_dragAudio = c;
        m_dragAudioTrack = m_provTrack = h.track;
        m_provStartMs = c.startMs;
        const QRect rc = trackClipRect(h.track, c);
        if (std::abs(e->pos().x() - rc.left()) <= kEdgeGrabPx)
            m_drag = Drag::AudioTrimIn;
        else if (std::abs(e->pos().x() - rc.right()) <= kEdgeGrabPx)
            m_drag = Drag::AudioTrimOut;
        else
            m_drag = Drag::AudioMove;
        return;
    }
    case Hit::Ruler: // the only place that moves the playhead
        // The exact frame at once: a scrub would show a keyframe first.
        m_drag = Drag::Seek;
        emit seekRequested(std::min(timelineMsAt(e->pos().x()), m_totalMs));
        return;
    case Hit::MainEmpty:
    case Hit::TrackEmpty: // just deselect
        emit trackClipSelected(-1);
        emit clipSelected(0);
        return;
    default:
        return;
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_drag == Drag::None) {
        // Hover cursor hint over an edge.
        const Hit h = hitTest(e->pos());
        Qt::CursorShape shape = Qt::ArrowCursor;
        QRect rc;
        if (h.kind == Hit::MainClip)
            rc = m_rows[h.row].rect;
        else if (h.kind == Hit::TrackClip)
            rc = trackClipRect(h.track, m_tracks[h.track].clips[h.clip]);
        if (rc.isValid() && (std::abs(e->pos().x() - rc.left()) <= kEdgeGrabPx ||
                             std::abs(e->pos().x() - rc.right()) <= kEdgeGrabPx))
            shape = Qt::SizeHorCursor;
        else if (h.kind == Hit::Header &&
                 (h.track < 0 ? hasOwnSound(-1)
                              : hasOwnSound(h.track) && foldBox(h.track).contains(e->pos())))
            shape = Qt::PointingHandCursor; // fold its sound tracks
        else if (h.kind == Hit::Transition)
            shape = Qt::PointingHandCursor;
        else if (h.kind == Hit::Header && h.track >= 0 &&
                 !muteBox(trackTop(h.track), kTrackH).contains(e->pos()))
            shape = Qt::OpenHandCursor; // drag to reorder, double-click to rename
        setCursor(shape);
        return;
    }

    if ((e->pos() - m_pressPos).manhattanLength() > kDragThresholdPx)
        m_dragMoved = true;
    m_lastDragPos = e->pos();
    dragTo(e->pos());

    // Dragging past an edge of the view: scroll while it's held there.
    if (!edgeScrollStep(e->pos()).isNull() && !m_autoScroll->isActive())
        m_autoScroll->start();
}

QPoint TimelineWidget::edgeScrollStep(QPoint pos) const
{
    QPoint step;
    // Sideways when zoomed in, past the clips' left or right edge (a header
    // drag moves vertically only).
    const bool sideways = m_dropActive || (m_drag != Drag::None && m_drag != Drag::TrackMove);
    const int left = contentLeft(), right = contentLeft() + contentWidth();
    if (sideways && zoomed() && (pos.x() < left || pos.x() > right)) {
        // Faster the further past the edge, up to ~a fifth of the view a second.
        const int over = pos.x() < left ? pos.x() - left : pos.x() - right;
        const int px = std::clamp(std::abs(over), 4, 40) * (over < 0 ? -1 : 1);
        const qint64 end = std::max<qint64>(0, m_extentMs - visibleSpanMs());
        if ((px < 0 && m_viewStartMs > 0) || (px > 0 && m_viewStartMs < end))
            step.setX(px);
    }
    // Up / down through the rows when they don't all fit, near the top or
    // bottom of the rows (the pointer may not get past them: the ruler is
    // thin, the window may end there): for a moved clip that can change
    // track, a header drag, and a file drag that lands on a track. Not on a
    // press's jitter: a click near the bottom shouldn't scroll.
    const bool canChangeTrack = m_drag == Drag::AudioMove && !m_dragAudioOrig.moveTargets.isEmpty();
    const bool vertical = m_dropActive ? !m_dropClip && !m_dropTransition
                                       : m_dragMoved && (canChangeTrack || m_drag == Drag::TrackMove);
    const int top = kRulerH + kEdgeScrollBand, bottom = scrollBarTop() - kEdgeScrollBand;
    if (vertical && maxViewTop() > 0 && (pos.y() < top || pos.y() > bottom)) {
        // From a couple of pixels a tick at the band's inner edge to most of
        // a track a tick well past it.
        const int over = pos.y() < top ? pos.y() - top : pos.y() - bottom;
        const int py = std::clamp(std::abs(over) / 2, 2, 24) * (over < 0 ? -1 : 1);
        if ((py < 0 && m_viewTopPx > 0) || (py > 0 && m_viewTopPx < maxViewTop()))
            step.setY(py);
    }
    return step;
}

void TimelineWidget::autoScrollTick()
{
    const QPoint pos = m_lastDragPos;
    const QPoint step = (m_drag != Drag::None || m_dropActive) ? edgeScrollStep(pos) : QPoint();
    if (step.isNull()) {
        m_autoScroll->stop();
        return;
    }
    m_viewStartMs += std::llround(double(step.x()) * visibleSpanMs() / contentWidth());
    m_viewTopPx += step.y();
    rebuildLayout(); // clamps, moves the scrollbars
    update();
    if (m_dropActive)
        dropTo(pos);
    else
        dragTo(pos);
}

void TimelineWidget::dragTo(QPoint pos)
{
    switch (m_drag) {
    case Drag::Seek:
        if (m_dragMoved) // a click's jitter isn't a drag
            emit scrubRequested(std::min(timelineMsAt(pos.x()), m_totalMs));
        break;
    case Drag::TrimIn: {
        const Row &r = m_rows[m_dragRow];
        const qint64 newStart = timelineMsAt(pos.x());
        const qint64 deltaSrc = std::llround((newStart - r.startMs) * r.clip.speed);
        m_provTrimMs = std::clamp<qint64>(r.clip.trimInMs + deltaSrc, 0,
                                          r.clip.trimOutMs - kMinTrimMs);
        update();
        break;
    }
    case Drag::TrimOut: {
        const Row &r = m_rows[m_dragRow];
        const qint64 newDur = std::max<qint64>(
            kMinTrimMs, std::llround((timelineMsAt(pos.x()) - r.startMs) * r.clip.speed));
        m_provTrimMs = std::clamp<qint64>(r.clip.trimInMs + newDur,
                                          r.clip.trimInMs + kMinTrimMs, r.clip.sourceDurationMs);
        update();
        break;
    }
    case Drag::Reorder:
        if (m_dragMoved) {
            m_reorderTarget = dropIndexForX(pos.x());
            update();
        }
        break;
    case Drag::AudioMove: {
        if (!m_dragMoved)
            break;
        const TrackClipView &o = m_dragAudioOrig;
        const qint64 delta = o.linked ? 0 // locked in time to its video clip
                                      : timelineMsAt(pos.x()) - m_pressMs;
        qint64 start = std::max<qint64>(0, o.startMs + delta);
        // Snap whichever edge lands closer to something.
        const qint64 s0 = o.linked ? start : snapMs(start, o.id);
        const qint64 s1 = o.linked ? start : snapMs(start + o.spanMs(), o.id) - o.spanMs();
        if (s0 != start && (s1 == start || std::abs(s0 - start) <= std::abs(s1 - start)))
            start = s0;
        else if (s1 != start)
            start = std::max<qint64>(0, s1);
        m_provStartMs = start;
        m_dragAudio.startMs = start;
        const int t = trackAtY(pos.y());
        if (t >= 0 && (t == m_dragAudioTrack || o.moveTargets.contains(m_tracks[t].id)))
            m_provTrack = t; // only onto a track that may hold it
        update();
        break;
    }
    case Drag::AudioTrimIn: {
        const TrackClipView &o = m_dragAudioOrig;
        const qint64 newStart = snapMs(timelineMsAt(pos.x()), o.id);
        const qint64 lo = std::max<qint64>(0, o.trimInMs - std::llround(o.startMs * o.speed));
        const qint64 in = std::clamp<qint64>(
            o.trimInMs + std::llround((newStart - o.startMs) * o.speed), lo, o.trimOutMs - kMinTrimMs);
        m_dragAudio.trimInMs = in;
        m_dragAudio.startMs = o.startMs + std::llround((in - o.trimInMs) / o.speed);
        update();
        break;
    }
    case Drag::AudioTrimOut: {
        const TrackClipView &o = m_dragAudioOrig;
        const qint64 newEnd = snapMs(timelineMsAt(pos.x()), o.id);
        const qint64 hi = o.sourceDurationMs > 0 ? o.sourceDurationMs : o.trimOutMs;
        m_dragAudio.trimOutMs = std::clamp<qint64>(
            o.trimInMs + std::llround((newEnd - o.startMs) * o.speed), o.trimInMs + kMinTrimMs,
            std::max(hi, o.trimInMs + kMinTrimMs));
        update();
        break;
    }
    case Drag::TrackMove:
        if (m_dragMoved) {
            // Only within its group; an overlay track not between another
            // one and its sound.
            int first = 0, end = 0;
            groupSpan(m_dragTrackIndex, &first, &end);
            m_trackGap = std::clamp(trackGapAtY(pos.y()), first, end);
            while (m_trackGap < end && m_tracks[m_trackGap].overlayAudio)
                ++m_trackGap;
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        break;
    default:
        break;
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    const Drag drag = m_drag;
    const int row = m_dragRow;
    m_drag = Drag::None;
    m_autoScroll->stop();

    switch (drag) {
    case Drag::Seek:
        // A drag may have shown keyframes; land on the exact frame. A click
        // already sought on press.
        if (m_dragMoved)
            emit seekRequested(std::min(timelineMsAt(e->pos().x()), m_totalMs));
        break;
    case Drag::TrimIn:
        if (m_dragMoved && row >= 0)
            emit clipTrimInRequested(m_rows[row].clip.id, m_provTrimMs);
        break;
    case Drag::TrimOut:
        if (m_dragMoved && row >= 0)
            emit clipTrimOutRequested(m_rows[row].clip.id, m_provTrimMs);
        break;
    case Drag::Reorder:
        if (m_dragMoved && row >= 0) {
            const int gap = dropIndexForX(e->pos().x());
            int to = gap > row ? gap - 1 : gap;
            to = std::clamp(to, 0, int(m_rows.size()) - 1);
            if (to != row)
                emit clipMoved(m_rows[row].clip.id, to);
        }
        break;
    case Drag::AudioMove:
        if (m_dragMoved && m_provTrack >= 0 && m_provTrack < m_tracks.size() &&
            (m_provTrack != m_dragAudioTrack || m_provStartMs != m_dragAudioOrig.startMs))
            emit trackClipMoved(m_dragAudioOrig.id, m_tracks[m_provTrack].id, m_provStartMs);
        break;
    case Drag::AudioTrimIn:
        if (m_dragMoved && m_dragAudio.trimInMs != m_dragAudioOrig.trimInMs)
            emit trackClipTrimInRequested(m_dragAudioOrig.id, m_dragAudio.trimInMs);
        break;
    case Drag::AudioTrimOut:
        if (m_dragMoved && m_dragAudio.trimOutMs != m_dragAudioOrig.trimOutMs)
            emit trackClipTrimOutRequested(m_dragAudioOrig.id, m_dragAudio.trimOutMs);
        break;
    case Drag::TrackMove:
        setCursor(Qt::ArrowCursor);
        if (m_dragMoved && m_dragTrackIndex >= 0 && m_dragTrackIndex < m_tracks.size() &&
            m_trackGap >= 0) {
            const int to = m_trackGap > m_dragTrackIndex ? m_trackGap - 1 : m_trackGap;
            if (to != m_dragTrackIndex)
                emit trackMoved(m_tracks[m_dragTrackIndex].id, to);
        }
        break;
    default:
        break;
    }
    m_reorderTarget = -1;
    m_trackGap = -1;
    update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    const Hit h = hitTest(e->pos());
    if (e->button() == Qt::LeftButton && h.kind == Hit::Header && h.track >= 0 &&
        !muteBox(trackTop(h.track), kTrackH).contains(e->pos()) &&
        !(hasOwnSound(h.track) && foldBox(h.track).contains(e->pos()))) {
        m_drag = Drag::None;
        startRename(h.track);
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TimelineWidget::startRename(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size())
        return;
    if (!m_renameEdit) {
        m_renameEdit = new QLineEdit(this);
        // Enter / Escape are this box's own (MainWindow's app-wide filter
        // otherwise turns them into "leave the field").
        m_renameEdit->setProperty("ownsReturnEscape", true);
        m_renameEdit->installEventFilter(this);
        connect(m_renameEdit, &QLineEdit::editingFinished, this, &TimelineWidget::finishRename);
    }
    m_renameTrackId = m_tracks[trackIndex].id;
    m_renameCancelled = false;
    m_renameEdit->setText(m_tracks[trackIndex].name);
    m_renameEdit->setGeometry(2, trackTop(trackIndex) + 1, std::max(kHeaderW + 80, 170), kTrackH - 2);
    m_renameEdit->show();
    m_renameEdit->raise();
    m_renameEdit->setFocus();
    m_renameEdit->selectAll();
}

void TimelineWidget::finishRename()
{
    const int id = m_renameTrackId;
    if (!id)
        return; // already finished (hiding the box loses focus: a second editingFinished)
    m_renameTrackId = 0;
    const QString name = m_renameEdit->text();
    m_renameEdit->hide();
    if (!m_renameCancelled)
        emit trackRenameRequested(id, name);
}

bool TimelineWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        const auto *he = static_cast<QHelpEvent *>(e);
        const Hit h = hitTest(he->pos());
        if (h.kind == Hit::Header &&
            (h.track < 0 ? hasOwnSound(-1)
                         : hasOwnSound(h.track) && foldBox(h.track).contains(he->pos())))
            QToolTip::showText(he->globalPos(),
                               tr("Click to fold its sound tracks away or show them again. "
                                  "Folded tracks still play and export."),
                               this);
        else if (h.kind == Hit::Header && h.track >= 0 && m_tracks[h.track].video &&
                 !muteBox(trackTop(h.track), kTrackH).contains(he->pos()))
            QToolTip::showText(he->globalPos(), layerToolTip(h.track), this);
        else if (h.kind == Hit::Transition)
            QToolTip::showText(he->globalPos(), m_transitions[h.row].toolTip, this);
        else if (h.kind == Hit::MainClip && m_rows[h.row].clip.keySourceMs.isEmpty())
            QToolTip::showText(he->globalPos(),
                               tr("No reframe keyframes: this clip exports looking straight "
                                  "forward. Arm reframe authoring and set a key to frame it."),
                               this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

bool TimelineWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_renameEdit && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        m_renameCancelled = true;
        finishRename();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent *e)
{
    const Hit h = hitTest(e->pos());
    QMenu menu(this);
    const qint64 atMs = timelineMsAt(e->pos().x());

    if (h.kind == Hit::Transition) {
        const int leftClipId = m_transitions[h.row].leftClipId;
        emit transitionSelected(leftClipId);
        QAction *remove = menu.addAction(tr("Remove transition"));
        remove->setObjectName(QStringLiteral("transitionRemoveAction"));
        if (menu.exec(e->globalPos()) == remove)
            emit transitionRemoveRequested(leftClipId);
        return;
    }

    if (h.kind == Hit::MainClip) {
        emit clipSelected(m_rows[h.row].clip.id);
        const Row &r = m_rows[h.row];
        const qint64 splitSrc = r.clip.trimInMs + std::llround((atMs - r.startMs) * r.clip.speed);
        QAction *split = menu.addAction(tr("Split here"));
        split->setEnabled(splitSrc > r.clip.trimInMs + kMinTrimMs
                          && splitSrc < r.clip.trimOutMs - kMinTrimMs);

        // Playback speed. Slow motion is the main use: 4K 120 fps at 0.25x
        // shows every source frame at a 30 fps output.
        QMenu *speedMenu = menu.addMenu(tr("Speed"));
        QList<QAction *> speedActions;
        for (double sp : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0}) {
            QAction *a = speedMenu->addAction(sp == 1.0 ? tr("1× (normal)")
                                                        : QStringLiteral("%1×").arg(sp));
            a->setCheckable(true);
            a->setChecked(std::abs(r.clip.speed - sp) < 1e-6);
            a->setData(sp);
            speedActions << a;
        }

        QAction *duplicate = menu.addAction(tr("Duplicate"));
        duplicate->setObjectName(QStringLiteral("clipDuplicateAction"));
        duplicate->setToolTip(tr("A copy of the clip right after it, with all its settings"));
        QAction *insertBefore = menu.addAction(tr("Insert clip before…"));
        QAction *insertAfter = menu.addAction(tr("Insert clip after…"));

        menu.addSeparator();
        QAction *copyGrade = menu.addAction(tr("Copy colour grade"));
        QAction *pasteGrade = menu.addAction(tr("Paste colour grade"));
        QAction *pasteGradeAll = menu.addAction(tr("Paste colour grade to all clips"));
        pasteGrade->setEnabled(m_canPasteGrade);
        pasteGradeAll->setEnabled(m_canPasteGrade);
        menu.addSeparator();
        QAction *copyStab = menu.addAction(tr("Copy stabilization"));
        QAction *pasteStab = menu.addAction(tr("Paste stabilization"));
        QAction *pasteStabAll = menu.addAction(tr("Paste stabilization to all clips"));
        pasteStab->setEnabled(m_canPasteStab);
        pasteStabAll->setEnabled(m_canPasteStab);
        menu.addSeparator();

        QAction *moveL = menu.addAction(tr("Move left"));
        moveL->setEnabled(h.row > 0);
        QAction *moveR = menu.addAction(tr("Move right"));
        moveR->setEnabled(h.row < m_rows.size() - 1);
        menu.addSeparator();
        QAction *remove = menu.addAction(tr("Remove clip"));

        QAction *chosen = menu.exec(e->globalPos());
        if (!chosen)
            return;
        if (chosen == split)
            emit clipSplitRequested(r.clip.id, splitSrc);
        else if (chosen == duplicate)
            emit clipDuplicateRequested(r.clip.id);
        else if (chosen == moveL)
            emit clipMoved(r.clip.id, h.row - 1);
        else if (chosen == moveR)
            emit clipMoved(r.clip.id, h.row + 1);
        else if (chosen == remove)
            emit clipRemoveRequested(r.clip.id);
        else if (chosen == insertBefore)
            emit addClipRequested(h.row);
        else if (chosen == insertAfter)
            emit addClipRequested(h.row + 1);
        else if (chosen == copyGrade)
            emit gradeCopyRequested(r.clip.id);
        else if (chosen == pasteGrade)
            emit gradePasteRequested(r.clip.id);
        else if (chosen == pasteGradeAll)
            emit gradePasteAllRequested();
        else if (chosen == copyStab)
            emit stabilizationCopyRequested(r.clip.id);
        else if (chosen == pasteStab)
            emit stabilizationPasteRequested(r.clip.id);
        else if (chosen == pasteStabAll)
            emit stabilizationPasteAllRequested();
        else if (speedActions.contains(chosen))
            emit clipSpeedRequested(r.clip.id, chosen->data().toDouble());
        return;
    }

    if (h.kind == Hit::TrackClip) {
        const TrackClipView &c = m_tracks[h.track].clips[h.clip];
        emit trackClipSelected(c.id);
        const qint64 atSrc = c.trimInMs + std::llround((atMs - c.startMs) * c.speed);
        QAction *split = menu.addAction(tr("Split here"));
        split->setEnabled(atSrc > c.trimInMs + kMinTrimMs && atSrc < c.trimOutMs - kMinTrimMs);
        QAction *link = nullptr;
        if (c.linked)
            link = menu.addAction(tr("Unlink from video clip"));
        else if (c.linkable)
            link = menu.addAction(tr("Link to video clip"));
        QAction *sound = nullptr;
        if (c.hasSound)
            sound = menu.addAction(tr("Put its sound on an audio track"));
        const bool wasLinked = c.linked;
        menu.addSeparator();
        QAction *remove = menu.addAction(m_tracks[h.track].video ? tr("Remove overlay clip")
                                                                 : tr("Remove audio clip"));
        const int clipId = c.id; // `c` may not survive the menu's event loop
        QAction *chosen = menu.exec(e->globalPos());
        if (!chosen)
            return;
        if (chosen == split)
            emit trackClipSplitRequested(clipId, atMs);
        else if (chosen == link)
            emit audioClipLinkRequested(clipId, !wasLinked);
        else if (chosen == sound)
            emit overlayAudioRequested(clipId);
        else if (chosen == remove)
            emit trackClipRemoveRequested(clipId);
        return;
    }

    // A video track with its own sound tracks: fold them away / back.
    QAction *fold = nullptr;
    const int foldTrack = h.track;
    if ((h.kind == Hit::Header || h.kind == Hit::TrackEmpty || h.kind == Hit::MainEmpty) &&
        hasOwnSound(foldTrack)) {
        fold = menu.addAction(isSoundCollapsed(videoTrackIdAt(foldTrack))
                                  ? tr("Show sound tracks")
                                  : tr("Fold sound tracks away"));
        menu.addSeparator();
    }

    // On an extra track: files of its kind; elsewhere, both.
    const int trackIndex = h.track;
    const bool onVideo = trackIndex >= 0 && m_tracks[trackIndex].video;
    const bool onAudio = trackIndex >= 0 && !onVideo;
    // Files are added onto the Audio group's tracks (or a new one), not a
    // video's own sound.
    const bool onAudioGroup = onAudio && m_tracks[trackIndex].group == TrackView::Group::Audio;
    QAction *addClip = nullptr, *addFile = nullptr, *addVideo = nullptr;
    if (trackIndex < 0)
        addClip = menu.addAction(tr("Add clip here…"));
    if (!onVideo) {
        addFile = menu.addAction(tr("Add audio file here…"));
        addFile->setEnabled(!m_rows.isEmpty());
    }
    if (!onAudio) {
        addVideo = menu.addAction(tr("Add video overlay here…"));
        addVideo->setEnabled(!m_rows.isEmpty());
    }
    QAction *addTrack = menu.addAction(tr("Add audio track"));
    QAction *addVideoTrack = menu.addAction(tr("Add overlay track"));
    QAction *rename = nullptr, *up = nullptr, *down = nullptr, *removeTrack = nullptr;
    int downTo = -1;
    if (trackIndex >= 0) {
        // Tracks move within their group, an overlay track with its audio
        // track; that one only with it.
        int first = 0, end = 0;
        groupSpan(trackIndex, &first, &end);
        const bool overlayAudio = m_tracks[trackIndex].overlayAudio;
        downTo = m_tracks[trackIndex].video ? unitEnd(trackIndex) : trackIndex + 1;
        menu.addSeparator();
        rename = menu.addAction(tr("Rename track…"));
        // An overlay track's place is its layer: say which way it goes.
        const bool video = m_tracks[trackIndex].video;
        up = menu.addAction(video ? tr("Bring forward") : tr("Move track up"));
        up->setObjectName(QStringLiteral("trackUpAction"));
        up->setEnabled(!overlayAudio && trackIndex > first);
        down = menu.addAction(video ? tr("Send backward") : tr("Move track down"));
        down->setObjectName(QStringLiteral("trackDownAction"));
        down->setEnabled(!overlayAudio && downTo < end);
        removeTrack = menu.addAction(tr("Remove track “%1”").arg(m_tracks[trackIndex].name));
    }
    menu.addSeparator();
    QAction *zin = menu.addAction(tr("Zoom in"));
    QAction *zout = menu.addAction(tr("Zoom out"));
    zout->setEnabled(zoomed());
    QAction *zfit = menu.addAction(tr("Zoom to fit"));
    zfit->setEnabled(zoomed());
    const int trackId = trackIndex >= 0 ? m_tracks[trackIndex].id : 0;
    QAction *chosen = menu.exec(e->globalPos());
    if (!chosen)
        return;
    if (chosen == fold)
        toggleSound(foldTrack);
    else if (chosen == addClip)
        emit addClipRequested(insertIndexAt(atMs));
    else if (chosen == addFile)
        emit addAudioFileRequested(onAudioGroup ? trackId : 0, std::min(atMs, m_totalMs));
    else if (chosen == addVideo)
        emit addVideoFileRequested(onVideo ? trackId : 0, std::min(atMs, m_totalMs));
    else if (chosen == addTrack)
        emit addAudioTrackRequested();
    else if (chosen == addVideoTrack)
        emit addVideoTrackRequested();
    else if (chosen == rename)
        startRename(trackIndex);
    else if (chosen == up)
        emit trackMoved(trackId, trackIndex - 1);
    else if (chosen == down)
        emit trackMoved(trackId, downTo);
    else if (chosen == removeTrack)
        emit trackRemoveRequested(trackId);
    else if (chosen == zin)
        zoomBy(kWheelZoom, e->pos().x() >= contentLeft() ? e->pos().x()
                                                         : contentLeft() + contentWidth() / 2);
    else if (chosen == zout)
        zoomBy(1.0 / kWheelZoom, e->pos().x() >= contentLeft() ? e->pos().x()
                                                               : contentLeft() + contentWidth() / 2);
    else if (chosen == zfit)
        zoomToFit();
}
