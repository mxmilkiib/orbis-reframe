#include "TimelineWidget.h"

#include "Theme.h"
#include "TimelineWidgetLayout.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

// Painting the timeline: the ruler, the track headers, the clips with their
// labels, keyframes and waveforms, the drop and reorder marks, the playhead.

using namespace timelinelayout;

namespace {

// "mm:ss", or "mm:ss.t" when the ruler's ticks are closer than a second.
QString formatTime(qint64 ms, bool tenths = false)
{
    if (ms < 0)
        ms = 0;
    const qint64 s = ms / 1000;
    QString t = QStringLiteral("%1:%2")
                    .arg(s / 60, 2, 10, QLatin1Char('0'))
                    .arg(s % 60, 2, 10, QLatin1Char('0'));
    if (tenths)
        t += QStringLiteral(".%1").arg((ms % 1000) / 100);
    return t;
}

// A "nice" tick interval (ms) so the ruler shows ~4-8 labels.
qint64 tickIntervalMs(qint64 totalMs, int usableW)
{
    if (totalMs <= 0 || usableW <= 0)
        return 1000;
    const int wantTicks = std::clamp(usableW / 90, 3, 10);
    const double raw = double(totalMs) / wantTicks;
    static const qint64 steps[] = {100,    200,    500,    1000,   2000,   5000,
                                   10000,  15000,  30000,  60000,  120000, 300000,
                                   600000, 900000, 1800000, 3600000};
    for (qint64 s : steps)
        if (double(s) >= raw)
            return s;
    return steps[sizeof(steps) / sizeof(steps[0]) - 1];
}

// The theme's clip colours; the selected (or active) one a little lighter.
QColor lit(const QColor &c, bool on) { return on ? c.lighter(125) : c; }

// Video clips alternate between two blues, so neighbours stay apart.
QColor clipFill(int index, bool active, bool missing)
{
    const theme::Colors &t = theme::colors();
    if (missing)
        return lit(t.clipMissing, active);
    return lit(index % 2 ? t.clipVideoAlt : t.clipVideo, active);
}

QColor audioFill(bool selected, bool muted, bool missing)
{
    const theme::Colors &t = theme::colors();
    return lit(missing ? t.clipMissing : muted ? t.clipMuted : t.clipAudio, selected);
}

QColor overlayFill(bool selected, bool hidden, bool missing)
{
    const theme::Colors &t = theme::colors();
    return lit(missing ? t.clipMissing : hidden ? t.clipMuted : t.clipOverlay, selected);
}

// A token at `alpha` (0-255), for marks drawn over clips.
QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

// A video track's fold triangle, in `box`: pointing down when its sound
// tracks show, right when they're folded away.
void drawFoldTriangle(QPainter &p, const QRect &box, bool collapsed, const QColor &color)
{
    const QPointF c = QRectF(box).center();
    const QPointF open[3] = {{c.x() - 4, c.y() - 2}, {c.x() + 4, c.y() - 2}, {c.x(), c.y() + 3}};
    const QPointF closed[3] = {{c.x() - 2, c.y() - 4}, {c.x() + 3, c.y()}, {c.x() - 2, c.y() + 4}};
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPolygon(collapsed ? closed : open, 3);
    p.restore();
}

} // namespace

void TimelineWidget::drawWaveform(QPainter &p, const QRect &rc, const Peaks &peaks, qint64 startMs,
                                  qint64 trimInMs, double speed, double gain, qint64 fadeInMs,
                                  qint64 fadeOutMs, qint64 spanMs, const QColor &color) const
{
    if (!peaks || peaks->empty() || rc.height() < 4)
        return;
    // dB scale: -36 dBFS at the centre line, 0 dBFS filling the height -- a
    // quiet mic still shows, and loud, even footage (a motorcycle ride sits
    // around -15..-6 dBFS) still has some shape.
    constexpr double kFloorDb = -36.0;
    const int x0 = std::max(rc.left() + 1, contentLeft());
    const int x1 = std::min(rc.right() - 1, contentLeft() + contentWidth());
    const double cy = rc.center().y() + 0.5;
    const double half = (rc.height() - 4) / 2.0;
    const qint64 n = static_cast<qint64>(peaks->size());
    p.setPen(QPen(color, 1));
    for (int x = x0; x <= x1; ++x) {
        const double l0 = msAtX(x) - startMs, l1 = msAtX(x + 1) - startMs; // clip-local, timeline
        if (l1 <= 0 || l0 >= spanMs)
            continue;
        const double s0 = trimInMs + std::max(0.0, l0) * speed; // source ms
        const double s1 = trimInMs + std::min<double>(spanMs, l1) * speed;
        qint64 i0 = static_cast<qint64>(std::floor(s0 * kPeaksPerSec / 1000.0));
        const qint64 i1 = std::max(i0 + 1, static_cast<qint64>(std::ceil(s1 * kPeaksPerSec / 1000.0)));
        float peak = 0.0f;
        for (i0 = std::max<qint64>(i0, 0); i0 < std::min(i1, n); ++i0)
            peak = std::max(peak, (*peaks)[size_t(i0)]);
        // The clip's level and fades, as they'll be heard.
        const double mid = (l0 + l1) / 2.0;
        double env = gain;
        if (fadeInMs > 0 && mid < fadeInMs)
            env *= std::max(0.0, mid / fadeInMs);
        if (fadeOutMs > 0 && mid > spanMs - fadeOutMs)
            env *= std::max(0.0, (spanMs - mid) / fadeOutMs);
        const double v = peak * env;
        if (v <= 0.0)
            continue;
        const double h = std::clamp((20.0 * std::log10(v) - kFloorDb) / -kFloorDb, 0.0, 1.0) * half;
        if (h >= 0.5)
            p.drawLine(QPointF(x + 0.5, cy - h), QPointF(x + 0.5, cy + h));
    }
}

void TimelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const theme::Colors &tc = theme::colors();
    p.fillRect(rect(), tc.panel);
    const QFontMetrics fm(font());

    // Everything in timeline time is clipped to the content area (a zoomed
    // view has clips running off both sides); the headers are drawn last,
    // unclipped.
    const int cLeft = contentLeft();
    const int usableW = contentWidth();
    const int rowsBottom = scrollBarTop();
    p.setClipRect(QRect(cLeft, 0, usableW + kMarginX, rowsBottom));

    // Ruler ticks, over the visible span (the ruler doesn't scroll).
    p.setPen(tc.textDim);
    if (m_extentMs > 0) {
        const qint64 span = visibleSpanMs();
        const qint64 step = tickIntervalMs(span, usableW);
        for (qint64 t = m_viewStartMs / step * step; t <= std::min(m_extentMs, m_viewStartMs + span);
             t += step) {
            const int x = xForTimelineMs(t);
            p.drawLine(x, kRulerH - 4, x, kRulerH);
            const QString label = formatTime(t, step < 1000);
            int lx = x + 2;
            if (lx + fm.horizontalAdvance(label) > width())
                lx = x - 2 - fm.horizontalAdvance(label);
            p.drawText(lx, kRulerH - 4, label);
        }
    }
    // The rows scroll under the ruler.
    p.setClipRect(QRect(cLeft, kRulerH, usableW + kMarginX, rowsBottom - kRulerH));

    // Row backgrounds.
    const QColor headerBg = tc.panel;
    const QColor rowBg = tc.menuBar;
    p.fillRect(QRect(cLeft, mainTop(), usableW, kMainH), rowBg);
    for (int i = 0; i < m_tracks.size(); ++i)
        if (trackShown(i))
            p.fillRect(QRect(cLeft, trackTop(i), usableW, kTrackH), rowBg);

    // Past the end of the main track: audio there isn't played.
    if (m_extentMs > m_totalMs) {
        const int x = std::max(cLeft, xForTimelineMs(m_totalMs));
        p.fillRect(QRect(x, kRulerH, width() - x, rowsBottom - kRulerH), tc.shade);
    }

    if (m_rows.isEmpty() && m_tracks.isEmpty()) {
        p.setPen(tc.textDim);
        p.drawText(QRect(cLeft, mainTop(), usableW, kMainH), Qt::AlignCenter,
                   tr("No clips — open a clip or add one to the timeline"));
    }

    // Main-track clip blocks.
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &r = m_rows[i];
        QRect rc = r.rect;
        // The clip under the playhead reads as selected -- unless an audio clip
        // is: then that one alone is highlighted.
        const bool active = r.clip.id == m_activeClipId && m_selectedAudioId <= 0;

        // Provisional geometry while an edge of this row is being dragged. In
        // a gapless timeline the clip's left edge stays anchored; both trims
        // just change its length (and everything after would shift on commit).
        if ((m_drag == Drag::TrimIn || m_drag == Drag::TrimOut) && m_dragRow == i) {
            const qint64 newSrcDur = m_drag == Drag::TrimIn ? r.clip.trimOutMs - m_provTrimMs
                                                            : m_provTrimMs - r.clip.trimInMs;
            const qint64 newDur = std::llround(newSrcDur / r.clip.speed);
            rc.setLeft(xForTimelineMs(r.startMs));
            rc.setRight(xForTimelineMs(r.startMs + std::max<qint64>(0, newDur)));
        }

        p.setBrush(clipFill(i, active, r.clip.missing));
        p.setPen(QPen(active ? tc.accent : tc.menuBar,
                      active ? 2 : 1));
        p.drawRect(rc.adjusted(0, 0, -1, -1));

        // Label.
        p.setPen(tc.text);
        // A hollow diamond marks a clip without reframe keys: it exports a
        // static forward view, whatever the viewport showed.
        QString label = (r.clip.missing ? QStringLiteral("⚠ ") : QString()) +
                        (r.clip.keySourceMs.isEmpty() ? QStringLiteral("◇ ") : QString()) +
                        r.clip.label;
        if (r.clip.speed != 1.0)
            label += QStringLiteral("  %1×").arg(r.clip.speed);
        // On the visible part, so a clip that starts off screen keeps its name.
        const QRect vis = rc.intersected(QRect(cLeft, 0, usableW, height()));
        p.drawText(vis.adjusted(5, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideMiddle, vis.width() - 10));

        // Keyframe diamonds.
        if (!r.clip.keySourceMs.isEmpty() && r.durMs > 0) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(tc.text);
            p.setPen(Qt::NoPen);
            const int cy = rc.bottom() - 5;
            for (qint64 ks : r.clip.keySourceMs) {
                if (ks < r.clip.trimInMs || ks > r.clip.trimOutMs)
                    continue;
                const int x = xForTimelineMs(r.startMs + std::llround((ks - r.clip.trimInMs) /
                                                                       r.clip.speed));
                const QPointF d[4] = {{double(x), double(cy - 3)},
                                      {double(x + 3), double(cy)},
                                      {double(x), double(cy + 3)},
                                      {double(x - 3), double(cy)}};
                p.drawPolygon(d, 4);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }

    // Transitions, over their cuts: a box as long as the blend, crossed like
    // the two pictures' levels (one fading out as the other fades in), or a
    // V for a dip to black (down to nothing at the cut and up again). One
    // that can't play (no footage past a trim) is a small box in the
    // warning colour.
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_transitions.size(); ++i) {
        const QRectF rc = QRectF(transitionRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
        if (rc.isEmpty())
            continue;
        const TransitionView &t = m_transitions[i];
        const bool sel = t.leftClipId == m_selectedTransition;
        const bool plays = t.durationMs > 0;
        p.setBrush(withAlpha(plays ? tc.waveform : tc.warning, sel ? 110 : 70));
        p.setPen(QPen(sel ? tc.accent : plays ? tc.waveform : tc.warning, sel ? 2 : 1));
        p.drawRoundedRect(rc, 3, 3);
        p.setPen(QPen(withAlpha(sel ? tc.accent : tc.waveform, 200), 1));
        if (plays && t.dip) {
            const QPointF bottom(rc.center().x(), rc.bottom() - 2);
            p.drawLine(rc.topLeft() + QPointF(2, 2), bottom);
            p.drawLine(bottom, rc.topRight() + QPointF(-2, 2));
        } else if (plays) {
            p.drawLine(rc.topLeft() + QPointF(2, 2), rc.bottomRight() - QPointF(2, 2));
            p.drawLine(rc.bottomLeft() + QPointF(2, -2), rc.topRight() + QPointF(-2, 2));
        } else {
            p.drawText(rc, Qt::AlignCenter, QStringLiteral("!"));
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    // Audio clips.
    const bool audioDrag = m_drag == Drag::AudioMove || m_drag == Drag::AudioTrimIn ||
                           m_drag == Drag::AudioTrimOut;
    const QIcon linkIcon = theme::icon(QStringLiteral("link"));
    auto drawAudio = [&](int trackIndex, const TrackClipView &c, bool ghost) {
        const QRect rc = trackClipRect(trackIndex, c);
        const bool sel = c.id == m_selectedAudioId;
        QColor fill = m_tracks[trackIndex].video
                          ? overlayFill(sel, m_tracks[trackIndex].muted, c.missing)
                          : audioFill(sel, c.muted || m_tracks[trackIndex].muted, c.missing);
        if (ghost)
            fill.setAlpha(170);
        p.setBrush(fill);
        p.setPen(QPen(sel ? tc.accent : tc.menuBar,
                      sel ? 2 : 1));
        p.drawRect(rc.adjusted(0, 0, -1, -1));
        drawWaveform(p, rc.adjusted(0, 1, 0, -1), c.peaks, c.startMs, c.trimInMs, c.speed,
                     c.muted ? 0.0 : c.gain, c.fadeInMs, c.fadeOutMs, c.spanMs(),
                     withAlpha(tc.waveform, 90));
        // Fade ramps: a line from silence (transparent) at the edge up to full.
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(withAlpha(tc.waveform, 150), 1));
        if (c.fadeInMs > 0)
            p.drawLine(rc.left(), rc.bottom(), xForTimelineMs(c.startMs + c.fadeInMs), rc.top() + 1);
        if (c.fadeOutMs > 0)
            p.drawLine(xForTimelineMs(c.endMs() - c.fadeOutMs), rc.top() + 1, rc.right(), rc.bottom());
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(tc.text);
        QString label = (c.missing ? QStringLiteral("⚠ ") : QString()) + c.label;
        if (c.speed != 1.0)
            label += QStringLiteral("  %1×").arg(c.speed);
        QRect vis = rc.intersected(QRect(cLeft, 0, usableW, height()));
        // A linked clip (the video's own sound) starts with a link icon: an
        // icon, not an emoji, which needs an emoji font to show.
        if (c.linked && !vis.isEmpty()) {
            const int s = std::min(fm.height(), vis.height() - 2);
            if (s > 0 && vis.width() > s + 10) {
                linkIcon.paint(&p, QRect(vis.left() + 5, vis.top() + (vis.height() - s) / 2, s, s));
                vis.setLeft(vis.left() + s + 3);
            }
        }
        p.drawText(vis.adjusted(5, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideMiddle, std::max(0, vis.width() - 10)));
        // An overlay's keyframe diamonds, like the main track's reframe keys.
        if (!c.keySourceMs.isEmpty()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(tc.text);
            p.setPen(Qt::NoPen);
            const int cy = rc.bottom() - 4;
            for (qint64 ks : c.keySourceMs) {
                if (ks < c.trimInMs || ks > c.trimOutMs)
                    continue;
                const int x = xForTimelineMs(c.startMs + std::llround((ks - c.trimInMs) / c.speed));
                const QPointF d[4] = {{double(x), double(cy - 3)},
                                      {double(x + 3), double(cy)},
                                      {double(x), double(cy + 3)},
                                      {double(x - 3), double(cy)}};
                p.drawPolygon(d, 4);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    };
    for (int t = 0; t < m_tracks.size(); ++t)
        for (const TrackClipView &c : m_tracks[t].clips)
            if (trackShown(t) && !(audioDrag && m_dragMoved && c.id == m_dragAudio.id))
                drawAudio(t, c, false);
    if (audioDrag && m_dragMoved && m_provTrack >= 0 && m_provTrack < m_tracks.size())
        drawAudio(m_provTrack, m_dragAudio, true);

    // Reorder insertion caret.
    if (m_drag == Drag::Reorder && m_dragMoved && m_reorderTarget >= 0 && !m_rows.isEmpty()) {
        const int x = m_reorderTarget < m_rows.size()
                          ? m_rows[m_reorderTarget].rect.left()
                          : m_rows.last().rect.right();
        p.setPen(QPen(tc.accent, 2));
        p.drawLine(x, mainTop(), x, mainTop() + kMainH);
    }

    // Playhead, over the ruler too.
    p.setClipRect(QRect(cLeft, 0, usableW + kMarginX, rowsBottom));
    const int px = xForTimelineMs(m_playheadMs);
    p.setPen(QPen(tc.playhead, 1));
    p.drawLine(px, 0, px, rowsBottom);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(tc.playhead);
    p.setPen(Qt::NoPen);
    const QPointF tri[3] = {{double(px - 4), 0.0}, {double(px + 4), 0.0}, {double(px), 6.0}};
    p.drawPolygon(tri, 3);
    p.setRenderHint(QPainter::Antialiasing, false);

    // A file drag: where it would land.
    if (m_dropActive) {
        const int x = xForTimelineMs(m_dropMs);
        const QColor hl = tc.accent;
        int top = mainTop() + kMainH, bottom = rowsBottom;
        if (m_dropClip || m_dropMain || m_dropTransition) { // into the main track
            top = kRulerH;
            bottom = mainTop() + kMainH;
        }
        if (m_dropTrack >= 0) {
            top = trackTop(m_dropTrack);
            bottom = top + kTrackH;
            QColor band = hl;
            band.setAlpha(50);
            p.fillRect(QRect(cLeft, top, usableW, kTrackH), band);
        }
        p.setPen(QPen(hl, 2));
        p.drawLine(x, top, x, bottom);
    }

    // Track headers, over anything scrolled under them, and under the ruler
    // when the rows scroll.
    p.setClipRect(QRect(0, kRulerH, width(), rowsBottom - kRulerH));
    const auto drawMuteBox = [&](const QRect &mb, bool on, const QString &letter) {
        p.setBrush(on ? tc.accent : Qt::transparent);
        p.setPen(tc.border);
        p.drawRect(mb.adjusted(0, 0, -1, -1));
        p.setPen(on ? tc.textOnAccent : tc.text);
        p.drawText(mb, Qt::AlignCenter, letter);
    };
    p.fillRect(QRect(0, mainTop(), kHeaderW, kMainH), headerBg);
    if (hasOwnSound(-1))
        drawFoldTriangle(p, foldBox(-1), isSoundCollapsed(0), tc.textDim);
    p.setPen(tc.text);
    p.drawText(QRect(kNameX, mainTop(), kHeaderW - kNameX - 6, kMainH),
               Qt::AlignVCenter | Qt::AlignLeft, tr("Video"));
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (!trackShown(i))
            continue;
        const TrackView &t = m_tracks[i];
        const int top = trackTop(i);
        const bool dragged = m_drag == Drag::TrackMove && m_dragMoved && i == m_dragTrackIndex;
        p.fillRect(QRect(0, top, kHeaderW, kTrackH),
                   dragged ? tc.accentSoft : headerBg);
        const QRect mb = muteBox(top, kTrackH);
        // A video's own sound is indented under it: the main track's mics,
        // an overlay track's audio track.
        const bool ownAudio = t.overlayAudio || t.group == TrackView::Group::Main;
        const int indent = ownAudio ? kOwnSoundX : kNameX;
        if (hasOwnSound(i))
            drawFoldTriangle(p, foldBox(i), isSoundCollapsed(t.id), tc.textDim);
        // An overlay track's layer: 1 in front.
        int nameRight = mb.left() - 2;
        if (t.video) {
            const QRect lb = layerBox(i);
            p.setBrush(Qt::NoBrush);
            p.setPen(tc.clipOverlay);
            p.drawRoundedRect(QRectF(lb).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            p.setPen(tc.text);
            p.drawText(lb, Qt::AlignCenter, QString::number(overlayLayer(i)));
            nameRight = lb.left() - 2;
        }
        p.setPen(tc.text);
        p.drawText(QRect(indent, top, nameRight - indent, kTrackH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(t.name, Qt::ElideRight, nameRight - indent));
        drawMuteBox(mb, t.muted, t.video ? QStringLiteral("H") : QStringLiteral("M"));
    }
    // Where a dragged track header would land.
    if (m_drag == Drag::TrackMove && m_dragMoved && m_trackGap >= 0 &&
        m_dragTrackIndex >= 0 && m_dragTrackIndex < m_tracks.size()) {
        int first = 0, end = 0;
        groupSpan(m_dragTrackIndex, &first, &end);
        const int y = m_trackGap >= end ? trackTop(end - 1) + kTrackH + kRowGap / 2 // the group's end
                                        : trackTop(m_trackGap) - kRowGap / 2 - 1;
        p.setPen(QPen(tc.accent, 2));
        p.drawLine(0, y, width(), y);
    }
    p.setClipping(false);
    p.fillRect(QRect(0, 0, kHeaderW + kMarginX, kRulerH), tc.panel);
}
