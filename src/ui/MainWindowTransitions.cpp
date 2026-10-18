// MainWindow's transitions (Transition.h): putting one on a cut from the
// Transitions tab, selecting it on the timeline, the Transition tab's
// settings, and keeping playback's layers and mix in step with them.

#include "MainWindow.h"

#include <QFileInfo>

#include <cmath>
#include <limits>

#ifdef ORBIS_REFRAME_HAVE_AUDIO
#include "AudioPlayer.h"
#endif
#include "EditToolbar.h"
#include "PlaybackController.h"
#include "RenderJobs.h"
#include "SettingsSidebar.h"
#include "TimelineWidget.h"
#include "TransitionsPanel.h"
#include "panels/TransitionPanel.h"
#include "../core/Timeline.h"

namespace {

QString seconds(qint64 ms)
{
    return QCoreApplication::translate("MainWindow", "%1 s").arg(ms / 1000.0, 0, 'f', 1);
}

// A clip by its place on the video track as well as its file: two clips cut
// from one recording share the file name.
QString clipName(const Timeline &timeline, int clipId)
{
    const ClipItem *c = timeline.clipById(clipId);
    if (!c)
        return {};
    return QCoreApplication::translate("MainWindow", "clip %1 (“%2”)")
        .arg(timeline.indexOfId(clipId) + 1)
        .arg(QFileInfo(c->sourcePath).fileName());
}

QString typeName(Transition::Type type)
{
    switch (type) {
    case Transition::Type::Crossfade:
        break;
    case Transition::Type::DipToBlack:
        return QCoreApplication::translate("MainWindow", "Dip to black");
    }
    return QCoreApplication::translate("MainWindow", "Crossfade");
}

// Why transition `t` plays shorter than it's set to (or not at all); empty
// when it plays in full.
QString shortenedNote(const Timeline &timeline, const Transition &t)
{
    const Timeline::TransitionSpan span = timeline.transitionSpan(t);
    if (span.limit == Timeline::TransitionSpan::Limit::None)
        return {};
    if (!timeline.clipById(t.leftClipId) || !timeline.clipById(t.rightClipId))
        return {};
    const auto tr = [](const char *s) { return QCoreApplication::translate("MainWindow", s); };
    QString why;
    switch (span.limit) {
    case Timeline::TransitionSpan::Limit::FootageBefore:
        why = span.plays() ? tr("there isn't more footage past the end of %1, the clip before "
                                "the cut.")
                           : tr("%1, the clip before the cut, runs to the end of its "
                                "recording: trim its end earlier to make room.");
        why = why.arg(clipName(timeline, t.leftClipId));
        break;
    case Timeline::TransitionSpan::Limit::FootageAfter:
        why = span.plays() ? tr("there isn't more footage before the start of %1, the clip "
                                "after the cut.")
                           : tr("%1, the clip after the cut, starts at the start of its "
                                "recording: trim its start later to make room.");
        why = why.arg(clipName(timeline, t.rightClipId));
        break;
    case Timeline::TransitionSpan::Limit::ClipLength:
        why = tr("it can take at most half of the shorter clip.");
        break;
    case Timeline::TransitionSpan::Limit::None:
        break;
    }
    return span.plays() ? tr("Plays %1: %2").arg(seconds(span.durationMs), why)
                        : tr("Doesn't play: %1").arg(why);
}

} // namespace

void MainWindow::connectTransitions()
{
    connect(m_transitionsPanel, &TransitionsPanel::addTransitionRequested, this,
            &MainWindow::addTransitionAtPlayhead);
    connect(m_timelineWidget, &TimelineWidget::transitionDropped, this, &MainWindow::addTransition);
    connect(m_timelineWidget, &TimelineWidget::transitionSelected, this,
            &MainWindow::selectTransition);
    connect(m_timelineWidget, &TimelineWidget::transitionRemoveRequested, m_timeline,
            &Timeline::removeTransition);
    connect(m_transitionPanel, &TransitionPanel::typeEdited, this, [this](Transition::Type type) {
        if (const Transition *t = m_timeline->transitionAfter(m_selectedTransition))
            m_timeline->setTransition(t->leftClipId, type, t->durationMs);
    });
    connect(m_transitionPanel, &TransitionPanel::durationEdited, this, [this](qint64 ms) {
        if (const Transition *t = m_timeline->transitionAfter(m_selectedTransition))
            m_timeline->setTransition(t->leftClipId, t->type, ms);
    });
    connect(m_transitionPanel, &TransitionPanel::removeRequested, this,
            [this] { m_timeline->removeTransition(m_selectedTransition); });

    connect(m_timeline, &Timeline::transitionsChanged, this, [this] {
        refreshTransitions();
        pushInsetsToController(); // the other clips' layers
#ifdef ORBIS_REFRAME_HAVE_AUDIO
        m_audioPlayer->setMix(renderjobs::mixClips(*m_timeline)); // the crossfading sound
#endif
    });
    // A transition's other clip is drawn with its own settings: when one
    // changes, so does the layer. (Trims, speed and moves rebuild the
    // layers with the timeline.)
    const auto layers = [this] {
        if (!m_timeline->transitions().isEmpty())
            pushInsetsToController();
    };
    connect(m_timeline, &Timeline::clipReframeChanged, this, layers);
    connect(m_timeline, &Timeline::clipColorChanged, this, layers);
    connect(m_timeline, &Timeline::clipStabilizationChanged, this, layers);
    connect(m_timeline, &Timeline::clipPrivacyChanged, this, layers);
    connect(m_timeline, &Timeline::clipStitchChanged, this, layers);
    connect(m_timeline, &Timeline::clipSyncChanged, this, layers);
    connect(m_timeline, &Timeline::clipSeamChanged, this, layers);
    refreshTransitions();
}

void MainWindow::selectTransition(int leftClipId)
{
    if (leftClipId && !m_timeline->transitionAfter(leftClipId))
        leftClipId = 0;
    const bool changed = leftClipId != m_selectedTransition;
    m_selectedTransition = leftClipId;
    m_timelineWidget->setSelectedTransition(leftClipId);
    if (leftClipId) { // one selection at a time
        if (m_selectedTrackClipId > 0)
            selectTrackClip(-1);
        if (m_selectedClipId > 0)
            selectVideoClip(0);
        if (changed)
            m_sidebar->setCurrentIndex(SettingsSidebar::TransitionTab);
    }
    refreshTransitions();
}

void MainWindow::addTransition(int leftClipId, const QString &type)
{
    // On a cut that has one: that one becomes this type, its duration kept.
    const Transition *had = m_timeline->transitionAfter(leftClipId);
    if (!m_timeline->setTransition(leftClipId, Transition::typeFromName(type.toLatin1().constData()),
                                   had ? had->durationMs : Transition::kDefaultMs) &&
        !had)
        return; // no cut there
    selectTransition(leftClipId);
    m_sidebar->setCurrentIndex(SettingsSidebar::TransitionTab);
    m_sidebar->setOpen(true);
    const Transition *t = m_timeline->transitionAfter(leftClipId);
    if (t && !m_timeline->transitionSpan(*t).plays())
        m_editToolbar->showNotice(shortenedNote(*m_timeline, *t), false);
}

void MainWindow::addTransitionAtPlayhead(const QString &type)
{
    if (m_timeline->count() < 2) {
        m_editToolbar->showNotice(tr("A transition goes on a cut between two clips: add another "
                                     "clip to the timeline first"),
                                  false);
        return;
    }
    // The cut nearest the playhead.
    int best = 1;
    qint64 bestDist = std::numeric_limits<qint64>::max();
    for (int i = 1; i < m_timeline->count(); ++i) {
        const qint64 d = std::llabs(m_timeline->clipToTimeline(i, 0) - m_lastTimelineMs);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    addTransition(m_timeline->clipAt(best - 1).id, type);
}

void MainWindow::refreshTransitions()
{
    if (m_selectedTransition && !m_timeline->transitionAfter(m_selectedTransition)) {
        m_selectedTransition = 0; // its cut went (or an undo gave the clips new ids)
        m_timelineWidget->setSelectedTransition(0);
    }
    QVector<TimelineWidget::TransitionView> views;
    for (const Transition &t : m_timeline->transitions()) {
        const Timeline::TransitionSpan span = m_timeline->transitionSpan(t);
        TimelineWidget::TransitionView v;
        v.leftClipId = t.leftClipId;
        v.durationMs = span.plays() ? span.durationMs : 0;
        v.dip = t.type == Transition::Type::DipToBlack;
        v.toolTip = tr("%1, %2").arg(typeName(t.type), seconds(t.durationMs));
        const QString note = shortenedNote(*m_timeline, t);
        if (!note.isEmpty())
            v.toolTip += QLatin1Char('\n') + note;
        views.append(v);
    }
    m_timelineWidget->setTransitions(views);

    const Transition *t = m_timeline->transitionAfter(m_selectedTransition);
    if (t && m_timeline->clipById(t->leftClipId) && m_timeline->clipById(t->rightClipId))
        m_transitionPanel->setTransition(*t,
                                         tr("%1 and %2")
                                             .arg(clipName(*m_timeline, t->leftClipId),
                                                  clipName(*m_timeline, t->rightClipId)),
                                         shortenedNote(*m_timeline, *t));
    else
        m_transitionPanel->setNoTransition(QString());
}
