#include "TimelineWidget.h"

#include "Theme.h"
#include "TimelineWidgetLayout.h"

#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

using namespace timelinelayout;

TimelineWidget::TimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    // Takes the height it's given (the window's splitter); rows that don't
    // fit scroll under the ruler.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    m_vscroll = new QScrollBar(Qt::Vertical, this);
    m_vscroll->setObjectName(QStringLiteral("timelineVScroll"));
    m_vscroll->hide();
    connect(m_vscroll, &QScrollBar::valueChanged, this, [this](int v) {
        if (v == m_viewTopPx)
            return;
        m_viewTopPx = v;
        rebuildLayout();
        update();
    });
    m_hscroll = new QScrollBar(Qt::Horizontal, this);
    m_hscroll->setObjectName(QStringLiteral("timelineHScroll"));
    m_hscroll->setEnabled(false);
    connect(m_hscroll, &QScrollBar::valueChanged, this, [this](int v) {
        m_viewStartMs = v;
        rebuildLayout();
        update();
    });
    setAcceptDrops(true);
    m_autoScroll = new QTimer(this);
    m_autoScroll->setInterval(30);
    connect(m_autoScroll, &QTimer::timeout, this, &TimelineWidget::autoScrollTick);
    layoutRows();
}

int TimelineWidget::mainTop() const
{
    return kRulerH - m_viewTopPx;
}

void TimelineWidget::layoutRows()
{
    // Under the main track, a gap wherever a new group starts (the main
    // track's own sound is its group). Folded-away sound tracks take no
    // room; their rows sit at their video track's, out of sight.
    m_trackY.resize(m_tracks.size());
    m_shown.resize(m_tracks.size());
    int y = kMainH + kRowGap;
    auto prev = TrackView::Group::Main;
    int video = -1; // the overlay track the next audio track belongs to
    for (int i = 0; i < m_tracks.size(); ++i) {
        const TrackView &t = m_tracks[i];
        if (t.group != prev)
            y += kGroupGap;
        prev = t.group;
        if (t.video)
            video = i;
        const bool own = t.group == TrackView::Group::Main || (t.overlayAudio && video >= 0);
        const int owner = t.group == TrackView::Group::Main ? -1 : video;
        m_shown[i] = !(own && isSoundCollapsed(videoTrackIdAt(owner)));
        if (!m_shown[i]) {
            m_trackY[i] = owner < 0 ? 0 : m_trackY[owner];
            continue;
        }
        m_trackY[i] = y;
        y += kTrackH + kRowGap;
    }
    m_rowsEndY = y;
}

bool TimelineWidget::hasOwnSound(int trackIndex) const
{
    if (trackIndex < 0)
        return !m_tracks.isEmpty() && m_tracks.first().group == TrackView::Group::Main;
    return m_tracks[trackIndex].video && unitEnd(trackIndex) > trackIndex + 1;
}

QRect TimelineWidget::foldBox(int trackIndex) const
{
    const int top = trackIndex < 0 ? mainTop() : trackTop(trackIndex);
    const int h = trackIndex < 0 ? kMainH : kTrackH;
    return QRect(2, top + (h - 14) / 2, kNameX - 4, 14);
}

int TimelineWidget::overlayLayer(int trackIndex, int *count) const
{
    int layer = 0, n = 0;
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].video) {
            ++n;
            if (i == trackIndex)
                layer = n;
        }
    if (count)
        *count = n;
    return layer;
}

QRect TimelineWidget::layerBox(int trackIndex) const
{
    const QRect mb = muteBox(trackTop(trackIndex), kTrackH);
    return QRect(mb.left() - 4 - kLayerBoxW, mb.top(), kLayerBoxW, mb.height());
}

QString TimelineWidget::layerToolTip(int trackIndex) const
{
    int count = 0;
    const int layer = overlayLayer(trackIndex, &count);
    if (count <= 1)
        return tr("Layer 1: the only overlay track, drawn over the main video.");
    if (layer == 1)
        return tr("Layer 1 of %1: drawn in front of the other overlays. Send it backward from "
                  "the right-click menu or by dragging this header down.").arg(count);
    if (layer == count)
        return tr("Layer %1 of %1: drawn behind the other overlays. Bring it forward from the "
                  "right-click menu or by dragging this header up.").arg(count);
    return tr("Layer %1 of %2: drawn behind the overlay tracks above it and in front of those "
              "below. Lower numbers are in front.").arg(layer).arg(count);
}

void TimelineWidget::setSoundCollapsed(int videoTrackId, bool collapsed)
{
    if (collapsed == isSoundCollapsed(videoTrackId))
        return;
    if (collapsed)
        m_collapsedSound.insert(videoTrackId);
    else
        m_collapsedSound.remove(videoTrackId);
    layoutRows();
    rebuildLayout();
    updateGeometry();
    update();
}

void TimelineWidget::toggleSound(int trackIndex)
{
    const int id = videoTrackIdAt(trackIndex);
    const bool collapsed = !isSoundCollapsed(id);
    setSoundCollapsed(id, collapsed);
    emit soundCollapsedToggled(id, collapsed);
}

int TimelineWidget::trackTop(int trackIndex) const
{
    return mainTop() + (trackIndex < m_trackY.size() ? m_trackY[trackIndex] : m_rowsEndY);
}

void TimelineWidget::groupSpan(int index, int *first, int *end) const
{
    *first = *end = index;
    while (*first > 0 && m_tracks[*first - 1].group == m_tracks[index].group)
        --*first;
    while (*end < m_tracks.size() && m_tracks[*end].group == m_tracks[index].group)
        ++*end;
}

int TimelineWidget::unitEnd(int index) const
{
    ++index;
    while (index < m_tracks.size() && m_tracks[index].overlayAudio)
        ++index;
    return index;
}

int TimelineWidget::rowsHeight() const
{
    // All rows, unscrolled, plus a little air above the scrollbar.
    return trackTop(static_cast<int>(m_tracks.size())) + m_viewTopPx - kRulerH + 2;
}

int TimelineWidget::scrollBarTop() const
{
    return std::max(kRulerH, height() - kScrollH);
}

int TimelineWidget::maxViewTop() const
{
    return std::max(0, rowsHeight() - (scrollBarTop() - kRulerH));
}

QSize TimelineWidget::sizeHint() const
{
    return QSize(480, kRulerH + rowsHeight() + kScrollH + 6);
}

QSize TimelineWidget::minimumSizeHint() const
{
    // The ruler, the main track and the scrollbar; the rest scrolls.
    return QSize(200, kRulerH + kMainH + kRowGap + 2 + kScrollH);
}

void TimelineWidget::setClips(const QVector<Clip> &clips)
{
    m_clips = clips;
    rebuildLayout();
    update();
}

void TimelineWidget::setTransitions(const QVector<TransitionView> &transitions)
{
    m_transitions = transitions;
    update();
}

void TimelineWidget::setSelectedTransition(int leftClipId)
{
    if (leftClipId == m_selectedTransition)
        return;
    m_selectedTransition = leftClipId;
    update();
}

QRect TimelineWidget::transitionRect(int index) const
{
    const TransitionView &t = m_transitions[index];
    for (int i = 0; i + 1 < m_rows.size(); ++i) {
        if (m_rows[i].clip.id != t.leftClipId)
            continue;
        const qint64 cut = m_rows[i + 1].startMs;
        const int x0 = xForTimelineMs(cut - t.durationMs / 2);
        const int x1 = xForTimelineMs(cut + t.durationMs / 2);
        const int cx = xForTimelineMs(cut);
        // Never thinner than something to click; in the top half of the
        // row, clear of the trim edges' lower half and the key diamonds.
        const int half = std::max(kTransitionMinW / 2, (x1 - x0) / 2);
        return QRect(cx - half, mainTop() + 3, 2 * half, kTransitionH);
    }
    return {};
}

void TimelineWidget::setTracks(const QVector<TrackView> &tracks)
{
    const bool resized = tracks.size() != m_tracks.size();
    m_tracks = tracks;
    layoutRows();
    if (m_drag == Drag::AudioMove || m_drag == Drag::AudioTrimIn || m_drag == Drag::AudioTrimOut)
        m_drag = Drag::None; // the model changed under the drag
    rebuildLayout();
    if (resized)
        updateGeometry();
    update();
}

void TimelineWidget::setPlayhead(qint64 timelineMs)
{
    timelineMs = std::clamp<qint64>(timelineMs, 0, m_totalMs);
    if (timelineMs == m_playheadMs)
        return;
    m_playheadMs = timelineMs;
    // Zoomed in and the playhead ran off screen (playback): page to it. Not
    // mid-drag -- the edge auto-scroll owns the view then.
    const qint64 span = visibleSpanMs();
    if (zoomed() && m_drag == Drag::None &&
        (timelineMs < m_viewStartMs || timelineMs > m_viewStartMs + span))
        scrollToMs(timelineMs - span / 10);
    update();
}

void TimelineWidget::setCanPasteGrade(bool canPaste)
{
    m_canPasteGrade = canPaste;
}

void TimelineWidget::setCanPasteStabilization(bool canPaste)
{
    m_canPasteStab = canPaste;
}

void TimelineWidget::setActiveClip(int clipId)
{
    if (clipId == m_activeClipId)
        return;
    m_activeClipId = clipId;
    rebuildLayout();
    update();
}

void TimelineWidget::setSelectedTrackClip(int clipId)
{
    if (clipId == m_selectedAudioId)
        return;
    m_selectedAudioId = clipId;
    update();
}

void TimelineWidget::resizeEvent(QResizeEvent *)
{
    rebuildLayout();
}

void TimelineWidget::rebuildLayout()
{
    m_rows.clear();
    m_totalMs = 0;
    for (const Clip &c : m_clips)
        m_totalMs += c.timelineSpanMs();
    m_extentMs = m_totalMs;
    for (const TrackView &t : m_tracks)
        for (const TrackClipView &c : t.clips)
            m_extentMs = std::max(m_extentMs, c.endMs());
    m_viewTopPx = std::clamp(m_viewTopPx, 0, maxViewTop());
    clampView();
    updateScrollBar();

    qint64 acc = 0;
    for (const Clip &c : m_clips) {
        Row r;
        r.clip = c;
        r.startMs = acc;
        r.durMs = c.timelineSpanMs();
        const int x0 = xForTimelineMs(acc);
        const int x1 = xForTimelineMs(acc + r.durMs);
        r.rect = QRect(x0, mainTop(), std::max(1, x1 - x0), kMainH);
        m_rows.push_back(r);
        acc += r.durMs;
    }
}

int TimelineWidget::contentLeft() const
{
    return kHeaderW + kMarginX;
}

int TimelineWidget::contentWidth() const
{
    const int vscrollW = maxViewTop() > 0 ? kScrollH : 0;
    return std::max(1, width() - contentLeft() - kMarginX - vscrollW);
}

qint64 TimelineWidget::visibleSpanMs() const
{
    const qint64 ext = std::max<qint64>(m_extentMs, 1);
    return m_visibleMs > 0 && m_visibleMs < ext ? m_visibleMs : ext;
}

int TimelineWidget::xForTimelineMs(qint64 ms) const
{
    if (m_extentMs <= 0)
        return contentLeft();
    const double x = contentLeft() + double(ms - m_viewStartMs) * contentWidth() / visibleSpanMs();
    return int(std::clamp(x, -1.0e6, 1.0e6)); // far off screen when zoomed in; keep it an int
}

qint64 TimelineWidget::timelineMsAt(int x) const
{
    const double ms = m_viewStartMs + double(x - contentLeft()) * visibleSpanMs() / contentWidth();
    return std::clamp<qint64>(std::llround(ms), 0, m_extentMs);
}

void TimelineWidget::clampView()
{
    const qint64 ext = std::max<qint64>(m_extentMs, 1);
    if (m_visibleMs >= ext)
        m_visibleMs = 0; // zoomed out to (or past) the whole: fit
    m_viewStartMs = std::clamp<qint64>(m_viewStartMs, 0, std::max<qint64>(0, ext - visibleSpanMs()));
    if (!zoomed())
        m_viewStartMs = 0;
}

void TimelineWidget::updateScrollBar()
{
    const QSignalBlocker block(m_hscroll);
    const qint64 span = visibleSpanMs();
    const qint64 max = std::max<qint64>(0, m_extentMs - span);
    m_hscroll->setRange(0, int(std::min<qint64>(max, INT_MAX)));
    m_hscroll->setPageStep(int(std::min<qint64>(span, INT_MAX)));
    m_hscroll->setSingleStep(int(std::max<qint64>(1, span / 20)));
    m_hscroll->setValue(int(std::min<qint64>(m_viewStartMs, INT_MAX)));
    m_hscroll->setEnabled(zoomed());
    m_hscroll->setGeometry(contentLeft(), scrollBarTop(), contentWidth(), kScrollH);

    const QSignalBlocker blockV(m_vscroll);
    const int rowsView = scrollBarTop() - kRulerH;
    m_vscroll->setRange(0, maxViewTop());
    m_vscroll->setPageStep(std::max(1, rowsView));
    m_vscroll->setSingleStep(kTrackH + kRowGap);
    m_vscroll->setValue(m_viewTopPx);
    m_vscroll->setGeometry(width() - kScrollH, kRulerH, kScrollH, rowsView);
    m_vscroll->setVisible(maxViewTop() > 0);
}

void TimelineWidget::scrollRowsBy(int dy)
{
    const int top = std::clamp(m_viewTopPx + dy, 0, maxViewTop());
    if (top == m_viewTopPx)
        return;
    m_viewTopPx = top;
    rebuildLayout();
    update();
}

void TimelineWidget::scrollToMs(qint64 viewStartMs)
{
    m_viewStartMs = viewStartMs;
    rebuildLayout(); // clamps, moves the scrollbar
    update();
}

void TimelineWidget::zoomBy(double factor, int anchorX)
{
    if (m_extentMs <= 0 || factor <= 0.0)
        return;
    const double anchorMs =
        m_viewStartMs + double(anchorX - contentLeft()) * visibleSpanMs() / contentWidth();
    const qint64 ext = std::max<qint64>(m_extentMs, 1);
    const qint64 span = std::clamp<qint64>(std::llround(visibleSpanMs() / factor),
                                           std::min(kMinVisibleMs, ext), ext);
    m_visibleMs = span >= ext ? 0 : span;
    m_viewStartMs = std::llround(anchorMs - double(anchorX - contentLeft()) * span / contentWidth());
    rebuildLayout();
    update();
}

void TimelineWidget::zoomIn()
{
    const int px = xForTimelineMs(m_playheadMs);
    const bool inView = px >= contentLeft() && px <= contentLeft() + contentWidth();
    zoomBy(kWheelZoom, inView ? px : contentLeft() + contentWidth() / 2);
}

void TimelineWidget::zoomOut()
{
    const int px = xForTimelineMs(m_playheadMs);
    const bool inView = px >= contentLeft() && px <= contentLeft() + contentWidth();
    zoomBy(1.0 / kWheelZoom, inView ? px : contentLeft() + contentWidth() / 2);
}

void TimelineWidget::zoomToFit()
{
    m_visibleMs = 0;
    m_viewStartMs = 0;
    rebuildLayout();
    update();
}

void TimelineWidget::wheelEvent(QWheelEvent *e)
{
    const QPoint d = e->angleDelta();
    const int x = int(e->position().x());
    if (e->modifiers() & Qt::ControlModifier) {
        if (d.y() != 0)
            zoomBy(std::pow(kWheelZoom, d.y() / 120.0), x);
    } else if (x < kHeaderW || (e->modifiers() & Qt::ShiftModifier)) {
        // Over the track headers, or with Shift: up / down through the
        // tracks, a track per notch.
        const int notch = d.y() != 0 ? d.y() : d.x();
        scrollRowsBy(-std::lround(notch / 120.0 * (kTrackH + kRowGap)));
    } else if (zoomed()) {
        // Either wheel axis pans; a notch moves a tenth of the view.
        const int notch = d.x() != 0 ? d.x() : d.y();
        scrollToMs(m_viewStartMs - std::llround(notch / 120.0 * visibleSpanMs() / 10.0));
    }
    e->accept();
}

QRect TimelineWidget::trackClipRect(int trackIndex, const TrackClipView &c) const
{
    const int x0 = xForTimelineMs(c.startMs);
    const int x1 = xForTimelineMs(c.endMs());
    return QRect(x0, trackTop(trackIndex), std::max(2, x1 - x0), kTrackH);
}

int TimelineWidget::trackAtY(int y) const
{
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (!trackShown(i))
            continue;
        const int top = trackTop(i);
        if (y >= top - kRowGap / 2 && y < top + kTrackH + kRowGap)
            return i;
    }
    return -1;
}

TimelineWidget::Hit TimelineWidget::hitTest(QPoint pos) const
{
    Hit h;
    if (pos.y() < kRulerH) {
        h.kind = pos.x() < kHeaderW ? Hit::None : Hit::Ruler;
        return h;
    }
    if (pos.y() >= scrollBarTop()) // rows scrolled under the scrollbar
        return h;
    const bool inHeader = pos.x() < kHeaderW;
    if (pos.y() >= mainTop() - kRowGap / 2 && pos.y() < mainTop() + kMainH + kRowGap / 2) {
        if (inHeader) {
            h.kind = Hit::Header;
            return h;
        }
        for (int i = 0; i < m_transitions.size(); ++i) // over the clips
            if (transitionRect(i).contains(pos)) {
                h.kind = Hit::Transition;
                h.row = i;
                return h;
            }
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows[i].rect.adjusted(0, 0, 1, 0).contains(pos)) {
                h.kind = Hit::MainClip;
                h.row = i;
                return h;
            }
        h.kind = Hit::MainEmpty;
        return h;
    }
    const int t = trackAtY(pos.y());
    if (t < 0)
        return h;
    h.track = t;
    if (inHeader) {
        h.kind = Hit::Header;
        return h;
    }
    // Topmost (last drawn) first.
    const QVector<TrackClipView> &clips = m_tracks[t].clips;
    for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i)
        if (trackClipRect(t, clips[i]).adjusted(-1, 0, 1, 0).contains(pos.x(), trackTop(t) + 1)) {
            h.kind = Hit::TrackClip;
            h.clip = i;
            return h;
        }
    h.kind = Hit::TrackEmpty;
    return h;
}

int TimelineWidget::insertIndexAt(qint64 ms) const
{
    int index = 0;
    while (index < m_rows.size() && m_rows[index].startMs + m_rows[index].durMs / 2 <= ms)
        ++index;
    return index;
}

int TimelineWidget::trackGapAtY(int y) const
{
    int gap = 0;
    for (int i = 0; i < m_tracks.size(); ++i)
        if (y > trackTop(i) + kTrackH / 2)
            gap = i + 1;
    return gap;
}

int TimelineWidget::dropIndexForX(int x) const
{
    int gap = 0;
    for (const Row &r : m_rows) {
        if (x >= (r.rect.left() + r.rect.right()) / 2)
            ++gap;
        else
            break;
    }
    return gap;
}

qint64 TimelineWidget::snapMs(qint64 ms, int excludeClipId) const
{
    const int x = xForTimelineMs(ms);
    qint64 best = ms;
    int bestPx = kSnapPx + 1;
    auto consider = [&](qint64 t) {
        const int d = std::abs(xForTimelineMs(t) - x);
        if (d < bestPx) {
            bestPx = d;
            best = t;
        }
    };
    consider(0);
    consider(m_playheadMs);
    for (const Row &r : m_rows)
        consider(r.startMs + r.durMs);
    for (const TrackView &t : m_tracks)
        for (const TrackClipView &c : t.clips)
            if (c.id != excludeClipId) {
                consider(c.startMs);
                consider(c.endMs());
            }
    return best;
}

double TimelineWidget::msAtX(double x) const
{
    return m_viewStartMs + (x - contentLeft()) * double(visibleSpanMs()) / contentWidth();
}
