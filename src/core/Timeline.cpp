#include "Timeline.h"

#include <QFileInfo>

#include <limits>
#include <cmath>
#include <algorithm>

Timeline::Timeline(QObject *parent)
    : QObject(parent)
{
}

int Timeline::indexOfId(int id) const
{
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].id == id)
            return i;
    }
    return -1;
}

const ClipItem *Timeline::clipById(int id) const
{
    const int i = indexOfId(id);
    return i < 0 ? nullptr : &m_clips[i];
}

ClipItem *Timeline::mutableClipById(int id)
{
    const int i = indexOfId(id);
    return i < 0 ? nullptr : &m_clips[i];
}

qint64 Timeline::totalDurationMs() const
{
    qint64 total = 0;
    for (const ClipItem &c : m_clips)
        total += c.timelineDurationMs();
    return total;
}

Timeline::Point Timeline::timelineToClip(qint64 timelineMs) const
{
    if (m_clips.isEmpty())
        return {-1, 0};

    timelineMs = std::clamp<qint64>(timelineMs, 0, totalDurationMs());
    qint64 acc = 0;
    for (int i = 0; i < m_clips.size(); ++i) {
        const qint64 span = m_clips[i].timelineDurationMs();
        // The boundary between two clips belongs to the later one; the very
        // end of the timeline resolves to the last clip at its full span.
        if (timelineMs < acc + span || i == m_clips.size() - 1)
            return {i, std::clamp<qint64>(timelineMs - acc, 0, span)};
        acc += span;
    }
    return {-1, 0}; // unreachable: the loop always returns on its last iteration
}

qint64 Timeline::clipToTimeline(int clipIndex, qint64 clipLocalMs) const
{
    if (clipIndex < 0 || clipIndex >= m_clips.size())
        return 0;
    qint64 acc = 0;
    for (int i = 0; i < clipIndex; ++i)
        acc += m_clips[i].timelineDurationMs();
    return acc + std::clamp<qint64>(clipLocalMs, 0, m_clips[clipIndex].timelineDurationMs());
}

namespace {

double framePeriodMs(const ClipItem &c)
{
    return 1000.0 / c.fps;
}

// A paused seek to source ms t shows the first frame at or after t (the
// decoders keep a frame whose pts + 1 ms reaches the target), so frame k's
// start rounded up is the one ms that shows it on either side of the
// rounding, and the frame on screen at t is the first one starting at or
// after t - 1.
qint64 frameStartMs(double periodMs, qint64 k)
{
    return static_cast<qint64>(std::ceil(static_cast<double>(k) * periodMs - 1e-6));
}

qint64 frameShownAt(double periodMs, qint64 sourceMs)
{
    return static_cast<qint64>(std::ceil((sourceMs - 1) / periodMs - 1e-6));
}

// Source ms -> clip-local timeline ms, rounded up so the controller's
// sourceAtLocalMs() of the result doesn't land before `sourceMs`.
qint64 localAtOrAfter(const ClipItem &c, qint64 sourceMs)
{
    return static_cast<qint64>(std::ceil((sourceMs - c.trimInMs) / c.speed - 1e-6));
}

} // namespace

qint64 Timeline::stepFrames(qint64 timelineMs, int frames) const
{
    const Point pt = timelineToClip(timelineMs);
    if (pt.clipIndex < 0 || frames == 0)
        return std::clamp<qint64>(timelineMs, 0, totalDurationMs());

    const ClipItem &c = m_clips[pt.clipIndex];
    const double period = framePeriodMs(c);
    const qint64 k = frameShownAt(period, c.sourceAtLocalMs(pt.clipLocalMs)) + frames;
    const qint64 targetSrc = frameStartMs(period, k);

    if (targetSrc >= c.trimOutMs) {
        if (pt.clipIndex + 1 < m_clips.size())
            return clipToTimeline(pt.clipIndex + 1, 0);
        return totalDurationMs();
    }
    if (targetSrc < c.trimInMs - 1) { // trimIn - 1 still shows the first frame
        if (pt.clipIndex == 0)
            return 0;
        // The previous clip's last frame: the last one starting before its out.
        const ClipItem &p = m_clips[pt.clipIndex - 1];
        const double pp = framePeriodMs(p);
        const qint64 last = frameShownAt(pp, p.trimOutMs + 1) - 1;
        const qint64 s = std::max(frameStartMs(pp, last), p.trimInMs);
        return clipToTimeline(pt.clipIndex - 1,
                              std::min(localAtOrAfter(p, s), p.timelineDurationMs() - 1));
    }
    return clipToTimeline(pt.clipIndex, std::max<qint64>(localAtOrAfter(c, targetSrc), 0));
}

qint64 Timeline::adjacentEditPoint(qint64 timelineMs, int direction) const
{
    const qint64 total = totalDurationMs();
    std::vector<qint64> points{0, total};
    qint64 acc = 0;
    for (const ClipItem &c : m_clips)
        points.push_back(acc += c.timelineDurationMs());
    for (const Track &t : m_tracks)
        for (const TrackClip &c : t.clips) {
            points.push_back(c.startMs);
            points.push_back(c.endMs());
        }

    qint64 best = timelineMs;
    for (qint64 p : points) {
        if (p < 0 || p > total)
            continue;
        if (direction > 0 && p > timelineMs && (best == timelineMs || p < best))
            best = p;
        else if (direction < 0 && p < timelineMs && (best == timelineMs || p > best))
            best = p;
    }
    return best;
}

qint64 Timeline::adjacentKey(qint64 timelineMs, int direction, int overlayClipId) const
{
    // Mapping a key to the timeline and back can be off by a rounded ms.
    constexpr qint64 kOnKeyMs = 5;
    std::vector<qint64> points;
    if (overlayClipId > 0) {
        if (const TrackClip *c = trackClipById(overlayClipId))
            for (const InsetKey &k : c->insetKeys.keys())
                if (k.timeMs >= c->trimInMs && k.timeMs <= c->trimOutMs)
                    points.push_back(c->startMs +
                                     std::llround((k.timeMs - c->trimInMs) / c->speed));
    } else {
        qint64 acc = 0;
        for (const ClipItem &c : m_clips) {
            // A key on the trim-out point would be the next clip's first frame.
            for (const ReframeKey &k : c.reframe.keys())
                if (k.timeMs >= c.trimInMs && k.timeMs < c.trimOutMs)
                    points.push_back(acc + c.localAtSourceMs(k.timeMs));
            acc += c.timelineDurationMs();
        }
    }

    qint64 best = timelineMs;
    for (qint64 p : points) {
        if (direction > 0 && p > timelineMs + kOnKeyMs && (best == timelineMs || p < best))
            best = p;
        else if (direction < 0 && p < timelineMs - kOnKeyMs && (best == timelineMs || p > best))
            best = p;
    }
    return best;
}

int Timeline::insertClip(int index, const QString &sourcePath, qint64 sourceDurationMs,
                         double fps, int sourceWidth, int sourceHeight, int audioStreams,
                         bool flat)
{
    index = std::clamp(index, 0, static_cast<int>(m_clips.size()));

    ClipItem c;
    c.id = m_nextId++;
    c.sourcePath = sourcePath;
    c.sourceDurationMs = std::max<qint64>(0, sourceDurationMs);
    c.fps = fps;
    c.sourceWidth = sourceWidth;
    c.sourceHeight = sourceHeight;
    c.flat = flat;
    c.resetEditsToFullSource();
    if (!flat) { // the nearest 360 clip's stabilization and stitch: before it, else after it
        int from = -1;
        for (int k = index - 1; k >= 0 && from < 0; --k)
            if (!m_clips.at(k).flat)
                from = k;
        for (int k = index; k < m_clips.size() && from < 0; ++k)
            if (!m_clips.at(k).flat)
                from = k;
        if (from >= 0) {
            c.stabilization = m_clips.at(from).stabilization;
            c.lensGuard = m_clips.at(from).lensGuard;
            c.depthSeam = m_clips.at(from).depthSeam;
        }
    }
    for (const ClipItem &o : m_clips) // the stitch and fine-tunes belong to the source file
        if (o.sourcePath == sourcePath) {
            c.lensGuard = o.lensGuard;
            c.depthSeam = o.depthSeam;
            if (!std::isnan(o.syncAdjustMs))
                c.syncAdjustMs = o.syncAdjustMs;
            c.seamAdjustDeg = o.seamAdjustDeg;
            c.seamAdjustGuard = o.seamAdjustGuard;
            break;
        }
    m_clips.insert(index, c);

    emit clipInserted(index);
    emit durationChanged(totalDurationMs());
    if (relinkAudio()) // clips after it moved
        emit tracksChanged();
    if (pruneTransitions()) // it went into a cut
        emit transitionsChanged();
    addClipSound(c.id, audioStreams);
    return c.id;
}

int Timeline::appendClip(const QString &sourcePath, qint64 sourceDurationMs, double fps,
                         int sourceWidth, int sourceHeight, int audioStreams, bool flat)
{
    return insertClip(static_cast<int>(m_clips.size()), sourcePath, sourceDurationMs, fps,
                      sourceWidth, sourceHeight, audioStreams, flat);
}

bool Timeline::removeClip(int clipId)
{
    const int i = indexOfId(clipId);
    if (i < 0)
        return false;
    const bool audio = removeLinkedAudio(clipId); // its sound goes with it
    m_clips.removeAt(i);
    emit clipRemoved(clipId);
    emit durationChanged(totalDurationMs());
    if (relinkAudio() || audio)
        emit tracksChanged();
    if (pruneTransitions())
        emit transitionsChanged();
    return true;
}

bool Timeline::moveClip(int clipId, int toIndex)
{
    const int from = indexOfId(clipId);
    if (from < 0)
        return false;
    toIndex = std::clamp(toIndex, 0, static_cast<int>(m_clips.size()) - 1);
    if (toIndex == from)
        return false;
    m_clips.move(from, toIndex);
    emit clipsReordered();
    if (relinkAudio())
        emit tracksChanged();
    if (pruneTransitions())
        emit transitionsChanged();
    return true;
}

int Timeline::splitClip(int clipId, qint64 atSourceMs)
{
    const int i = indexOfId(clipId);
    if (i < 0)
        return 0;
    ClipItem &left = m_clips[i];
    if (atSourceMs <= left.trimInMs + ClipItem::kMinTrimMs
        || atSourceMs >= left.trimOutMs - ClipItem::kMinTrimMs)
        return 0;

    ClipItem right = left; // same source / metadata / reframe track
    right.id = m_nextId++;
    right.trimInMs = atSourceMs;
    left.trimOutMs = atSourceMs;
    const int leftId = left.id;
    const qint64 leftIn = left.trimInMs;
    const qint64 leftOut = left.trimOutMs;

    m_clips.insert(i + 1, right); // may reallocate -- don't touch `left` after this
    const bool audio = splitLinkedAudio(leftId, right.id, atSourceMs);
    // The cut after it is the right part's now.
    const bool moved = moveTransitionLeft(leftId, right.id);

    emit clipInserted(i + 1);
    emit clipTrimChanged(leftId, leftIn, leftOut);
    emit durationChanged(totalDurationMs());
    if (relinkAudio() || audio)
        emit tracksChanged();
    if (moved)
        emit transitionsChanged();
    return right.id;
}

int Timeline::duplicateClip(int clipId)
{
    const int i = indexOfId(clipId);
    if (i < 0)
        return 0;
    ClipItem copy = m_clips.at(i);
    copy.id = m_nextId++;
    m_clips.insert(i + 1, copy);
    const bool moved = moveTransitionLeft(clipId, copy.id); // the cut after it is the copy's now
    bool audio = false;
    for (Track &t : m_tracks) {
        QList<TrackClip> added;
        for (const TrackClip &a : t.clips) {
            if (a.linkedClipId != clipId)
                continue;
            TrackClip d = a; // placed after the copy by relinkAudio
            d.id = m_nextId++;
            d.linkedClipId = copy.id;
            added.push_back(d);
        }
        if (!added.isEmpty()) {
            t.clips.append(added);
            audio = true;
        }
    }

    emit clipInserted(i + 1);
    emit durationChanged(totalDurationMs());
    if (relinkAudio() || audio)
        emit tracksChanged();
    if (moved)
        emit transitionsChanged();
    return copy.id;
}

void Timeline::clear()
{
    if (!m_transitions.isEmpty()) {
        m_transitions.clear();
        emit transitionsChanged();
    }
    if (!m_tracks.isEmpty()) {
        m_tracks.clear();
        emit tracksChanged();
    }
    if (m_clips.isEmpty())
        return;
    m_clips.clear();
    emit clipsReordered();
    emit durationChanged(0);
}

const Transition *Timeline::transitionAfter(int leftClipId) const
{
    for (const Transition &t : m_transitions)
        if (t.leftClipId == leftClipId)
            return &t;
    return nullptr;
}

Timeline::TransitionSpan Timeline::transitionSpan(const Transition &t) const
{
    TransitionSpan span;
    const int i = indexOfId(t.leftClipId);
    if (i < 0 || i + 1 >= m_clips.size() || m_clips.at(i + 1).id != t.rightClipId)
        return span;
    const ClipItem &a = m_clips.at(i);
    const ClipItem &b = m_clips.at(i + 1);
    span.cutMs = clipToTimeline(i + 1, 0);
    // Each half, in timeline ms: what's asked for, the footage past the trim
    // on that side (at the clip's speed), and half the clip.
    qint64 half = t.durationMs / 2;
    const auto cap = [&half, &span](qint64 limitMs, TransitionSpan::Limit why) {
        if (limitMs < half) {
            half = std::max<qint64>(0, limitMs);
            span.limit = why;
        }
    };
    if (t.usesHandles()) {
        cap(static_cast<qint64>(std::floor((a.sourceDurationMs - a.trimOutMs) / a.speed)),
            TransitionSpan::Limit::FootageBefore);
        cap(static_cast<qint64>(std::floor(b.trimInMs / b.speed)),
            TransitionSpan::Limit::FootageAfter);
    }
    cap(std::min(a.timelineDurationMs(), b.timelineDurationMs()) / 2,
        TransitionSpan::Limit::ClipLength);
    span.durationMs = 2 * half;
    return span;
}

bool Timeline::shownSourceRange(int clipId, qint64 &fromMs, qint64 &toMs) const
{
    const int i = indexOfId(clipId);
    if (i < 0)
        return false;
    const ClipItem &c = m_clips.at(i);
    fromMs = c.trimInMs;
    toMs = c.trimOutMs;
    if (i > 0)
        if (const Transition *t = transitionAfter(m_clips.at(i - 1).id); t && t->usesHandles())
            if (const TransitionSpan s = transitionSpan(*t); s.plays())
                fromMs -= std::llround(s.durationMs / 2 * c.speed);
    if (const Transition *t = transitionAfter(clipId); t && t->usesHandles())
        if (const TransitionSpan s = transitionSpan(*t); s.plays())
            toMs += std::llround(s.durationMs / 2 * c.speed);
    return true;
}

bool Timeline::setTransition(int leftClipId, Transition::Type type, qint64 durationMs)
{
    const int i = indexOfId(leftClipId);
    if (i < 0 || i + 1 >= m_clips.size())
        return false;
    Transition t;
    t.leftClipId = leftClipId;
    t.rightClipId = m_clips.at(i + 1).id;
    t.type = type;
    t.durationMs = std::clamp(durationMs, Transition::kMinMs, Transition::kMaxMs);
    for (Transition &o : m_transitions)
        if (o.leftClipId == leftClipId) {
            if (o == t)
                return false;
            o = t;
            emit transitionsChanged();
            return true;
        }
    m_transitions.append(t);
    emit transitionsChanged();
    return true;
}

bool Timeline::removeTransition(int leftClipId)
{
    const qsizetype n = m_transitions.removeIf(
        [leftClipId](const Transition &t) { return t.leftClipId == leftClipId; });
    if (n == 0)
        return false;
    emit transitionsChanged();
    return true;
}

bool Timeline::pruneTransitions()
{
    return m_transitions.removeIf([this](const Transition &t) {
        const int i = indexOfId(t.leftClipId);
        return i < 0 || i + 1 >= m_clips.size() || m_clips.at(i + 1).id != t.rightClipId;
    }) > 0;
}

bool Timeline::moveTransitionLeft(int fromLeftId, int toLeftId)
{
    for (Transition &t : m_transitions)
        if (t.leftClipId == fromLeftId) {
            t.leftClipId = toLeftId;
            return true;
        }
    return false;
}

std::vector<Timeline::ClipPlan> Timeline::playbackPlan() const
{
    std::vector<ClipPlan> plan;
    plan.reserve(static_cast<size_t>(m_clips.size()));
    for (const ClipItem &c : m_clips)
        plan.push_back({c.sourcePath.toStdString(), c.trimInMs, c.trimOutMs});
    return plan;
}

void Timeline::setClipTrimIn(int clipId, qint64 positionMs)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimIn(positionMs)) {
        const bool audio = followTrim(clipId, oldIn, oldOut);
        emit clipTrimChanged(c->id, c->trimInMs, c->trimOutMs);
        emit durationChanged(totalDurationMs());
        if (relinkAudio() || audio)
            emit tracksChanged();
    }
}

void Timeline::setClipTrimOut(int clipId, qint64 positionMs)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimOut(positionMs)) {
        const bool audio = followTrim(clipId, oldIn, oldOut);
        emit clipTrimChanged(c->id, c->trimInMs, c->trimOutMs);
        emit durationChanged(totalDurationMs());
        if (relinkAudio() || audio)
            emit tracksChanged();
    }
}

void Timeline::clearClipTrim(int clipId)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->clearTrim()) {
        const bool audio = followTrim(clipId, oldIn, oldOut);
        emit clipTrimChanged(c->id, c->trimInMs, c->trimOutMs);
        emit durationChanged(totalDurationMs());
        if (relinkAudio() || audio)
            emit tracksChanged();
    }
}

void Timeline::upsertClipReframeKey(int clipId, qint64 sourceMs, ReframePose pose)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->upsertReframeKey(sourceMs, pose))
        emit clipReframeChanged(c->id);
}

void Timeline::removeClipReframeKeyNear(int clipId, qint64 sourceMs)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->removeReframeKeyNear(sourceMs))
        emit clipReframeChanged(c->id);
}

void Timeline::setClipReframeEase(int clipId, qint64 sourceMs, keytrack::Ease ease)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->reframe.setEase(sourceMs, ClipItem::kReframeDeleteSnapMs, ease))
        emit clipReframeChanged(c->id);
}

void Timeline::replaceClipReframeSpan(int clipId, qint64 fromMs, qint64 toMs,
                                      const QList<ReframeKey> &keys)
{
    ClipItem *c = mutableClipById(clipId);
    if (!c || c->flat)
        return;
    c->reframe.replaceSpan(fromMs, toMs, keys);
    emit clipReframeChanged(c->id);
}

void Timeline::clearClipReframe(int clipId)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->clearReframe())
        emit clipReframeChanged(c->id);
}

void Timeline::setClipSpeed(int clipId, double speed)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setSpeed(speed)) {
        emit clipSpeedChanged(c->id, c->speed);
        emit durationChanged(totalDurationMs());
        if (relinkAudio()) // its linked audio takes the speed; later clips move
            emit tracksChanged();
    }
}

void Timeline::setClipFit(int clipId, ClipItem::Fit fit)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->flat && c->setFit(fit))
        emit clipFitChanged(c->id, fit);
}

void Timeline::setClipColor(int clipId, const ColorGrade &grade)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setColorGrade(grade))
        emit clipColorChanged(c->id, c->color);
}

void Timeline::setClipStabilization(int clipId, const insv::StabConfig &config)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setStabilization(config))
        emit clipStabilizationChanged(c->id, c->stabilization);
}

void Timeline::setClipBlurPlates(int clipId, bool on)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setBlurPlates(on))
        emit clipPrivacyChanged(c->id, on);
}

void Timeline::setClipPlateScan(int clipId, const QString &key)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setPlateScan(key))
        emit clipPrivacyChanged(c->id, c->blurPlates);
}

void Timeline::setClipBlurAdditions(int clipId, const std::vector<privacy::BlurAddition> &additions)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setBlurAdditions(additions))
        emit clipPrivacyChanged(c->id, c->blurPlates);
}

void Timeline::setClipBlurRemovals(int clipId, const std::vector<privacy::BlurRemoval> &removals)
{
    ClipItem *c = mutableClipById(clipId);
    if (c && c->setBlurRemovals(removals))
        emit clipPrivacyChanged(c->id, c->blurPlates);
}

void Timeline::setSourceStitch(const QString &sourcePath,
                               stitch::PreviewStitchConfig::LensGuard lensGuard, bool depthSeam)
{
    for (ClipItem &c : m_clips) {
        if (c.sourcePath != sourcePath || (c.lensGuard == lensGuard && c.depthSeam == depthSeam))
            continue;
        c.lensGuard = lensGuard;
        c.depthSeam = depthSeam;
        emit clipStitchChanged(c.id);
    }
}

void Timeline::setSourceSyncAdjust(const QString &sourcePath, double ms)
{
    for (ClipItem &c : m_clips) {
        if (c.sourcePath != sourcePath || c.syncAdjustMs == ms)
            continue;
        c.syncAdjustMs = ms;
        emit clipSyncChanged(c.id, ms);
    }
}

void Timeline::setSourceSeamAdjust(const QString &sourcePath, const std::array<double, 3> &deg,
                                   int lensGuard)
{
    for (ClipItem &c : m_clips) {
        if (c.sourcePath != sourcePath ||
            (c.seamAdjustDeg == deg && c.seamAdjustGuard == lensGuard))
            continue;
        c.seamAdjustDeg = deg;
        c.seamAdjustGuard = lensGuard;
        emit clipSeamChanged(c.id);
    }
}

double Timeline::sourceSyncAdjust(const QString &sourcePath) const
{
    for (const ClipItem &c : m_clips)
        if (c.sourcePath == sourcePath)
            return c.syncAdjustMs;
    return std::nan("");
}

bool Timeline::sourceSeamAdjust(const QString &sourcePath, int lensGuard,
                                std::array<double, 3> &deg) const
{
    for (const ClipItem &c : m_clips) {
        if (c.sourcePath == sourcePath && c.seamAdjustGuard == lensGuard) {
            deg = c.seamAdjustDeg;
            return true;
        }
    }
    deg = {0.0, 0.0, 0.0};
    return false;
}

bool Timeline::sourceSeamAdjust(const QString &sourcePath, std::array<double, 3> &deg) const
{
    for (const ClipItem &c : m_clips)
        if (c.sourcePath == sourcePath)
            return sourceSeamAdjust(sourcePath, static_cast<int>(c.lensGuard), deg);
    deg = {0.0, 0.0, 0.0};
    return false;
}

void Timeline::setClipMediaMissing(int clipId, bool missing)
{
    ClipItem *c = mutableClipById(clipId);
    if (c)
        c->mediaMissing = missing;
}

int Timeline::relinkSource(const QString &from, const QString &to)
{
    if (from == to)
        return 0;
    const bool missing = !QFileInfo::exists(to);
    int mainCount = 0, trackCount = 0;
    for (ClipItem &c : m_clips)
        if (c.sourcePath == from) {
            c.sourcePath = to;
            c.mediaMissing = missing;
            ++mainCount;
        }
    for (Track &t : m_tracks)
        for (TrackClip &c : t.clips)
            if (c.sourcePath == from) {
                c.sourcePath = to;
                c.mediaMissing = missing;
                ++trackCount;
            }
    if (mainCount)
        emit clipsReordered(); // the wholesale-change notice: consumers rebuild
    if (trackCount)
        emit tracksChanged();
    return mainCount + trackCount;
}

QStringList Timeline::setSourcesMissing(const QHash<QString, bool> &missing)
{
    QStringList changed;
    const auto apply = [&](const QString &path, bool &flag) {
        const auto it = missing.constFind(path);
        if (it == missing.cend() || flag == *it)
            return;
        flag = *it;
        if (!changed.contains(path))
            changed << path;
    };
    for (ClipItem &c : m_clips)
        apply(c.sourcePath, c.mediaMissing);
    for (Track &t : m_tracks)
        for (TrackClip &c : t.clips)
            if (!c.isTitle()) // no file
                apply(c.sourcePath, c.mediaMissing);
    if (!changed.isEmpty())
        emit mediaMissingChanged();
    return changed;
}

// ---------------------------------------------------------------------------
// Extra tracks
// ---------------------------------------------------------------------------

int Timeline::trackIndexOfId(int trackId) const
{
    for (int i = 0; i < m_tracks.size(); ++i)
        if (m_tracks[i].id == trackId)
            return i;
    return -1;
}

const Track *Timeline::trackById(int trackId) const
{
    const int i = trackIndexOfId(trackId);
    return i < 0 ? nullptr : &m_tracks[i];
}

Track *Timeline::mutableTrackById(int trackId)
{
    const int i = trackIndexOfId(trackId);
    return i < 0 ? nullptr : &m_tracks[i];
}

const TrackClip *Timeline::trackClipById(int clipId, int *trackId) const
{
    for (const Track &t : m_tracks)
        for (const TrackClip &c : t.clips)
            if (c.id == clipId) {
                if (trackId)
                    *trackId = t.id;
                return &c;
            }
    return nullptr;
}

TrackClip *Timeline::mutableTrackClipById(int clipId, Track **track)
{
    for (Track &t : m_tracks)
        for (TrackClip &c : t.clips)
            if (c.id == clipId) {
                if (track)
                    *track = &t;
                return &c;
            }
    return nullptr;
}

qint64 Timeline::extraTracksEndMs() const
{
    qint64 end = 0;
    for (const Track &t : m_tracks)
        for (const TrackClip &c : t.clips)
            end = std::max(end, c.endMs());
    return end;
}

int Timeline::addTrack(Track::Kind kind, const QString &name, Track::Role role, int ownerTrackId,
                       int id)
{
    Track t;
    t.kind = kind;
    t.role = kind == Track::Kind::Audio ? role : Track::Role::Free;
    t.name = name;
    if (t.role == Track::Role::OverlayAudio) {
        const Track *owner = trackById(ownerTrackId);
        if (!owner || owner->kind != Track::Kind::Video)
            return 0;
        t.ownerTrackId = ownerTrackId;
        if (t.name.isEmpty())
            t.name = tr("%1 audio").arg(owner->name);
    }
    if (id > 0 && !trackById(id)) {
        t.id = id;
        m_nextTrackId = std::max(m_nextTrackId, id + 1);
    } else {
        t.id = m_nextTrackId++;
    }
    if (t.name.isEmpty() && t.role == Track::Role::CameraMic)
        t.name = tr("Camera mic");
    else if (t.name.isEmpty() && t.role == Track::Role::ExternalMic)
        t.name = tr("External mic");
    else if (t.name.isEmpty()) {
        // The lowest number no track of its kind has (tracks get renamed,
        // removed and reordered, so counting them can repeat a name).
        const auto nameFor = [kind](int n) {
            return kind == Track::Kind::Video ? tr("Overlay %1").arg(n) : tr("Audio %1").arg(n);
        };
        int n = 1;
        const auto taken = [&](const QString &name) {
            return std::any_of(m_tracks.begin(), m_tracks.end(),
                               [&](const Track &o) { return o.name == name; });
        };
        while (taken(nameFor(n)))
            ++n;
        t.name = nameFor(n);
    }
    placeTrack(t);
    emit tracksChanged();
    return t.id;
}

void Timeline::placeTrack(const Track &t)
{
    int at = 0;
    switch (t.group()) {
    case Track::Group::Main: // the camera mic first
        while (at < m_tracks.size() && m_tracks[at].isStreamTrack() &&
               (t.role == Track::Role::ExternalMic || m_tracks[at].role == Track::Role::CameraMic))
            ++at;
        break;
    case Track::Group::Audio:
        while (at < m_tracks.size() && m_tracks[at].group() != Track::Group::Overlay)
            ++at;
        break;
    case Track::Group::Overlay:
        at = static_cast<int>(m_tracks.size());
        if (t.role == Track::Role::OverlayAudio) { // under its overlay and its other sound
            at = trackIndexOfId(t.ownerTrackId) + 1;
            while (at < m_tracks.size() && m_tracks[at].role == Track::Role::OverlayAudio)
                ++at;
        }
        break;
    }
    m_tracks.insert(at, t);
}

int Timeline::streamTrackId(Track::Role role) const
{
    for (const Track &t : m_tracks)
        if (t.kind == Track::Kind::Audio && t.role == role && role != Track::Role::Free)
            return t.id;
    return 0;
}

int Timeline::audioTrackId(int overlayTrackId) const
{
    for (const Track &t : m_tracks)
        if (t.role == Track::Role::OverlayAudio && t.ownerTrackId == overlayTrackId)
            return t.id;
    return 0;
}

int Timeline::freeTrackFor(qint64 startMs, qint64 endMs, bool video,
                           const std::function<bool(const Track &)> &accept) const
{
    for (const Track &t : m_tracks) {
        if ((t.kind == Track::Kind::Video) != video)
            continue;
        const bool free = std::none_of(t.clips.begin(), t.clips.end(), [&](const TrackClip &c) {
            return c.startMs < endMs && startMs < c.endMs();
        });
        if (free && accept(t))
            return t.id;
    }
    return 0;
}

int Timeline::linkTargetFor(const TrackClip &a) const
{
    int best = 0;
    qint64 bestDist = std::numeric_limits<qint64>::max();
    auto consider = [&](int id, qint64 startMs, qint64 trimInMs, qint64 trimOutMs, double speed) {
        if (a.trimOutMs <= trimInMs || trimOutMs <= a.trimInMs)
            return;
        const qint64 where = startMs + std::llround((a.trimInMs - trimInMs) / speed);
        const qint64 dist = std::abs(where - a.startMs);
        if (dist < bestDist) {
            bestDist = dist;
            best = id;
        }
    };
    for (int i = 0; i < count(); ++i) {
        const ClipItem &c = clipAt(i);
        if (c.sourcePath == a.sourcePath)
            consider(c.id, clipToTimeline(i, 0), c.trimInMs, c.trimOutMs, c.speed);
    }
    for (const Track &t : m_tracks)
        for (const TrackClip &c : t.clips)
            if (t.kind == Track::Kind::Video && c.sourcePath == a.sourcePath)
                consider(c.id, c.startMs, c.trimInMs, c.trimOutMs, c.speed);
    return best;
}

int Timeline::addOverlaySound(int clipId)
{
    int trackId = 0;
    const TrackClip *o = trackClipById(clipId, &trackId);
    if (!o || trackById(trackId)->kind != Track::Kind::Video || o->isTitle())
        return 0;
    // (Only this clip's: another overlay of the same file has its own.)
    for (const Track &t : m_tracks)
        for (const TrackClip &x : t.clips)
            if (x.linkedClipId == o->id)
                return x.id;
    TrackClip a;
    a.sourcePath = o->sourcePath;
    a.sourceDurationMs = o->sourceDurationMs;
    a.startMs = o->startMs;
    a.trimInMs = o->trimInMs;
    a.trimOutMs = o->trimOutMs;
    a.speed = o->speed;
    a.linkedClipId = o->id; // and follows the overlay's edits
    int audioTrack = audioTrackId(trackId);
    if (!audioTrack)
        audioTrack = addTrack(Track::Kind::Audio, QString(), Track::Role::OverlayAudio, trackId);
    return insertTrackClip(audioTrack, a);
}

QList<int> Timeline::addClipSound(int clipId, int streamCount)
{
    QList<int> ids(std::max(0, streamCount), 0);
    const int index = indexOfId(clipId);
    if (index < 0)
        return ids;
    for (int stream = 0; stream < streamCount; ++stream) {
        const Track::Role role = Track::roleOfStream(stream, streamCount);
        if (role == Track::Role::Free)
            continue; // a third stream: no known kind
        bool linked = false;
        for (const Track &t : m_tracks)
            for (const TrackClip &a : t.clips)
                linked = linked || (a.linkedClipId == clipId && a.stream == stream);
        if (linked)
            continue;
        int trackId = streamTrackId(role);
        if (!trackId)
            trackId = addTrack(Track::Kind::Audio, QString(), role);
        const ClipItem &c = m_clips.at(indexOfId(clipId));
        TrackClip a;
        a.sourcePath = c.sourcePath;
        a.sourceDurationMs = c.sourceDurationMs;
        a.mediaMissing = c.mediaMissing;
        a.stream = stream;
        a.startMs = clipToTimeline(indexOfId(clipId), 0);
        a.trimInMs = c.trimInMs;
        a.trimOutMs = c.trimOutMs;
        a.speed = c.speed;
        a.linkedClipId = clipId;
        ids[stream] = insertTrackClip(trackId, a);
    }
    return ids;
}

bool Timeline::removeTrack(int trackId)
{
    const int i = trackIndexOfId(trackId);
    if (i < 0)
        return false;
    QList<int> gone{trackId};
    for (const Track &t : m_tracks) // an overlay's audio track goes with it
        if (t.role == Track::Role::OverlayAudio && t.ownerTrackId == trackId)
            gone.append(t.id);
    QList<TrackClip> clips;
    for (int id : gone)
        clips += m_tracks.takeAt(trackIndexOfId(id)).clips;
    QList<int> emptied;
    for (const TrackClip &c : clips) // an overlay's sound goes with it
        removeLinkedAudio(c.id, &emptied);
    dropEmptyOverlayAudioTracks(emptied);
    emit tracksChanged();
    return true;
}

bool Timeline::setTrackName(int trackId, const QString &name)
{
    Track *t = mutableTrackById(trackId);
    const QString n = name.trimmed();
    if (!t || n.isEmpty() || n == t->name)
        return false;
    const QString old = t->name;
    t->name = n;
    if (t->kind == Track::Kind::Video)
        for (Track &s : m_tracks)
            if (s.role == Track::Role::OverlayAudio && s.ownerTrackId == trackId &&
                s.name == tr("%1 audio").arg(old))
                s.name = tr("%1 audio").arg(n);
    emit tracksChanged();
    return true;
}

bool Timeline::moveTrack(int trackId, int toIndex)
{
    const int from = trackIndexOfId(trackId);
    if (from < 0 || m_tracks[from].role == Track::Role::OverlayAudio)
        return false;
    const Track::Group group = m_tracks[from].group();
    int first = 0, end = 0; // the group's span
    while (first < m_tracks.size() && m_tracks[first].group() != group)
        ++first;
    end = first;
    while (end < m_tracks.size() && m_tracks[end].group() == group)
        ++end;
    toIndex = std::clamp(toIndex, first, end - 1);
    if (group != Track::Group::Overlay) {
        if (toIndex == from)
            return false;
        m_tracks.move(from, toIndex);
    } else {
        // Whole overlay tracks, each with its audio tracks.
        const auto unitEnd = [this](int i) {
            ++i;
            while (i < m_tracks.size() && m_tracks[i].role == Track::Role::OverlayAudio)
                ++i;
            return i;
        };
        while (m_tracks[toIndex].role == Track::Role::OverlayAudio)
            --toIndex; // its overlay
        if (toIndex == from)
            return false;
        const int len = unitEnd(from) - from;
        const int at = toIndex < from ? toIndex : unitEnd(toIndex) - len;
        QList<Track> unit = m_tracks.mid(from, len);
        m_tracks.remove(from, len);
        for (int k = 0; k < len; ++k)
            m_tracks.insert(at + k, unit[k]);
    }
    emit tracksChanged();
    return true;
}

void Timeline::setTrackAudio(int trackId, double gainDb, bool muted)
{
    Track *t = mutableTrackById(trackId);
    if (!t)
        return;
    gainDb = std::clamp(gainDb, audiolimits::kMinGainDb, audiolimits::kMaxGainDb);
    if (t->gainDb == gainDb && t->muted == muted)
        return;
    t->gainDb = gainDb;
    t->muted = muted;
    emit tracksChanged();
}

int Timeline::insertTrackClip(int trackId, const TrackClip &clip)
{
    Track *t = mutableTrackById(trackId);
    if (!t)
        return 0;
    TrackClip c = clip;
    c.id = m_nextId++;
    c.sourceDurationMs = std::max<qint64>(0, c.sourceDurationMs);
    c.speed = std::clamp(c.speed > 0.0 ? c.speed : 1.0, ClipItem::kMinSpeed, ClipItem::kMaxSpeed);
    c.startMs = std::max<qint64>(0, c.startMs);
    // Trims into range with the min gap (the saved pair, or the whole source).
    const qint64 srcEnd = std::max<qint64>(c.sourceDurationMs, TrackClip::kMinTrimMs);
    if (c.trimOutMs <= 0)
        c.trimOutMs = srcEnd;
    c.trimOutMs = std::clamp<qint64>(c.trimOutMs, TrackClip::kMinTrimMs, srcEnd);
    c.trimInMs = std::clamp<qint64>(c.trimInMs, 0, c.trimOutMs - TrackClip::kMinTrimMs);
    c.gainDb = std::clamp(c.gainDb, audiolimits::kMinGainDb, audiolimits::kMaxGainDb);
    c.setFades(c.fadeInMs, c.fadeOutMs);
    c.look = c.look.clamped();
    c.frame = c.frame.clamped();
    if (t->kind == Track::Kind::Video) { // no audio link on an overlay
        c.linkedClipId = 0;
        c.stream = -1;
    } else {
        c.title.reset(); // a title is an overlay
    }
    if (c.isTitle()) {
        c.sourcePath.clear();
        c.mediaMissing = false;
    }
    if (c.linkedClipId && !linkAnchor(c.linkedClipId))
        c.linkedClipId = 0;
    t->clips.push_back(c);
    t->sortClips();
    relinkAudio();
    emit tracksChanged();
    return c.id;
}

int Timeline::addAudioClip(int trackId, const QString &sourcePath, qint64 sourceDurationMs,
                           qint64 startMs, int stream)
{
    const Track *t = trackById(trackId);
    if (!t || t->kind != Track::Kind::Audio)
        return 0;
    TrackClip c;
    c.sourcePath = sourcePath;
    c.sourceDurationMs = sourceDurationMs;
    c.stream = stream;
    c.startMs = startMs;
    c.resetToFullSource();
    return insertTrackClip(trackId, c);
}

int Timeline::addVideoClip(int trackId, const QString &sourcePath, qint64 sourceDurationMs,
                           qint64 startMs, int sourceWidth, int sourceHeight)
{
    const Track *t = trackById(trackId);
    if (!t || t->kind != Track::Kind::Video)
        return 0;
    TrackClip c;
    c.sourcePath = sourcePath;
    c.sourceDurationMs = sourceDurationMs;
    c.startMs = startMs;
    c.sourceWidth = sourceWidth;
    c.sourceHeight = sourceHeight;
    c.resetToFullSource();
    return insertTrackClip(trackId, c);
}

int Timeline::addTitleClip(int trackId, qint64 startMs, const title::Style &style,
                           qint64 spanMs)
{
    const Track *t = trackById(trackId);
    if (!t || t->kind != Track::Kind::Video)
        return 0;
    TrackClip c;
    c.title = style;
    c.sourceDurationMs = title::kSourceMs;
    c.startMs = startMs;
    c.trimInMs = title::kSourceMs / 2;
    c.trimOutMs = c.trimInMs + std::clamp<qint64>(spanMs, TrackClip::kMinTrimMs, title::kSourceMs / 2);
    c.look.centerX = 0.5f;
    c.look.centerY = 0.5f;
    c.look.width = title::kDefaultSize;
    return insertTrackClip(trackId, c);
}

void Timeline::setTrackClipTitle(int clipId, const title::Style &style)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c || !c->isTitle() || *c->title == style)
        return;
    c->title = style;
    c->title->rgb &= 0xffffff;
    emit tracksChanged();
}

bool Timeline::removeTrackClip(int clipId)
{
    for (Track &t : m_tracks)
        for (int i = 0; i < t.clips.size(); ++i)
            if (t.clips[i].id == clipId) {
                t.clips.removeAt(i);
                QList<int> emptied{t.id};
                removeLinkedAudio(clipId, &emptied); // an overlay's sound goes with it
                dropEmptyOverlayAudioTracks(emptied);
                emit tracksChanged();
                return true;
            }
    return false;
}

Track::Role Timeline::streamRole(const TrackClip &a) const
{
    if (a.stream < 0)
        return Track::Role::Free;
    int count = m_audioStreamCounter ? m_audioStreamCounter(a.sourcePath) : 0;
    if (count <= 0) // as many as its clips use
        for (const Track &t : m_tracks)
            for (const TrackClip &o : t.clips)
                if (o.sourcePath == a.sourcePath)
                    count = std::max(count, o.stream + 1);
    return Track::roleOfStream(a.stream, std::max(count, a.stream + 1));
}

bool Timeline::canHoldClip(int trackId, int clipId) const
{
    int fromId = 0;
    const TrackClip *a = trackClipById(clipId, &fromId);
    const Track *to = trackById(trackId);
    if (!a || !to)
        return false;
    const Track &from = *trackById(fromId);
    if (to->kind != from.kind)
        return false;
    if (to->id == from.id || to->kind == Track::Kind::Video)
        return true;
    // An overlay's sound: a file on that overlay track (the clip it's linked to).
    const auto overlaySound = [&] {
        if (to->role != Track::Role::OverlayAudio)
            return false;
        if (a->linkedClipId) {
            int overlayTrackId = 0;
            return trackClipById(a->linkedClipId, &overlayTrackId) &&
                   overlayTrackId == to->ownerTrackId;
        }
        const Track *owner = trackById(to->ownerTrackId);
        return a->stream < 0 && owner &&
               std::any_of(owner->clips.begin(), owner->clips.end(),
                           [&](const TrackClip &o) { return o.sourcePath == a->sourcePath; });
    };
    // The main video's sound: a stream track of its mic.
    const auto streamSound = [&] {
        if (!to->isStreamTrack() || streamRole(*a) != to->role)
            return false;
        return !a->linkedClipId || indexOfId(a->linkedClipId) >= 0;
    };
    switch (to->group()) {
    case Track::Group::Audio:
        return !a->linkedClipId;
    case Track::Group::Main:
        return streamSound();
    case Track::Group::Overlay:
        return overlaySound();
    }
    return false;
}

void Timeline::transferTrackClip(int clipId, Track *from, Track *to)
{
    const auto at = std::find_if(from->clips.begin(), from->clips.end(),
                                 [&](const TrackClip &a) { return a.id == clipId; });
    const TrackClip a = *at;
    from->clips.erase(at);
    to->clips.push_back(a);
    to->sortClips();
}

void Timeline::moveTrackClip(int clipId, int toTrackId, qint64 startMs)
{
    Track *from = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &from);
    Track *to = mutableTrackById(toTrackId);
    if (!c || !to || !canHoldClip(toTrackId, clipId))
        return;
    if (c->linkedClipId) // locked in time to its video clip
        startMs = c->startMs;
    if (from == to) {
        if (c->setStartMs(startMs)) {
            from->sortClips();
            relinkAudio(); // an overlay's sound moves with it
            emit tracksChanged();
        }
        return;
    }
    c->setStartMs(startMs);
    transferTrackClip(clipId, from, to);
    QList<int> emptied{from->id};
    relinkAudio();
    dropEmptyOverlayAudioTracks(emptied);
    emit tracksChanged();
}

int Timeline::splitTrackClip(int clipId, qint64 atMs)
{
    Track *t = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &t);
    if (!c)
        return 0;
    const qint64 atSrc = c->sourceAtLocalMs(atMs - c->startMs);
    if (atSrc <= c->trimInMs + TrackClip::kMinTrimMs ||
        atSrc >= c->trimOutMs - TrackClip::kMinTrimMs)
        return 0;
    TrackClip right = *c;
    right.id = m_nextId++;
    const int leftId = c->id;
    right.startMs = c->startMs + std::llround((atSrc - c->trimInMs) / c->speed);
    right.trimInMs = atSrc;
    right.fadeInMs = 0;
    c->trimOutMs = atSrc;
    c->fadeOutMs = 0;
    c->setFades(c->fadeInMs, 0);     // clamp to the shorter span
    right.setFades(0, right.fadeOutMs);
    t->clips.push_back(right); // may reallocate -- don't touch `c` after this
    t->sortClips();
    if (t->kind == Track::Kind::Video) // an overlay's sound splits with it
        splitLinkedAudio(leftId, right.id, atSrc);
    relinkAudio();
    emit tracksChanged();
    return right.id;
}

void Timeline::setTrackClipTrimIn(int clipId, qint64 sourceMs)
{
    Track *t = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &t);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimIn(sourceMs)) {
        t->sortClips();
        if (t->kind == Track::Kind::Video) // an overlay's sound follows the trim
            followTrim(clipId, oldIn, oldOut);
        relinkAudio(); // a linked clip's start is derived from its in-point
        emit tracksChanged();
    }
}

void Timeline::setTrackClipTrimOut(int clipId, qint64 sourceMs)
{
    Track *t = nullptr;
    TrackClip *c = mutableTrackClipById(clipId, &t);
    if (!c)
        return;
    const qint64 oldIn = c->trimInMs, oldOut = c->trimOutMs;
    if (c->setTrimOut(sourceMs)) {
        if (t->kind == Track::Kind::Video) {
            followTrim(clipId, oldIn, oldOut);
            relinkAudio();
        }
        emit tracksChanged();
    }
}

void Timeline::setAudioClipNoise(int clipId, const insv::NoiseReduction &nr)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (c && c->setNoise(nr))
        emit tracksChanged();
}

void Timeline::setAudioClipLevel(int clipId, double gainDb, bool muted)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c)
        return;
    const bool g = c->setGainDb(gainDb);
    const bool m = c->setMuted(muted);
    if (g || m)
        emit tracksChanged();
}

void Timeline::setTrackClipFades(int clipId, qint64 fadeInMs, qint64 fadeOutMs)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (c && c->setFades(fadeInMs, fadeOutMs))
        emit tracksChanged();
}

void Timeline::setTrackClipInset(int clipId, const inset::Look &look, const inset::Frame &frame)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c)
        return;
    const inset::Look l = look.clamped();
    const inset::Frame f = frame.clamped();
    if (l == c->look && f == c->frame)
        return;
    c->look = l;
    c->frame = f;
    emit tracksChanged();
}

void Timeline::upsertTrackClipInsetKey(int clipId, qint64 sourceMs, const inset::Look &look)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c)
        return;
    const InsetKeyTrack before = c->insetKeys;
    c->insetKeys.upsert(sourceMs, look, kInsetKeySnapMs);
    if (!(c->insetKeys == before))
        emit tracksChanged();
}

void Timeline::removeTrackClipInsetKeyNear(int clipId, qint64 sourceMs)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c || c->insetKeys.count() == 0)
        return;
    const inset::Look last = c->insetKeys.sample(sourceMs);
    if (!c->insetKeys.removeNear(sourceMs, kInsetKeyDeleteSnapMs))
        return;
    if (c->insetKeys.isEmpty())
        c->look = last; // the last key's look stays where it was
    emit tracksChanged();
}

void Timeline::setTrackClipInsetEase(int clipId, qint64 sourceMs, keytrack::Ease ease)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (c && c->insetKeys.setEase(sourceMs, kInsetKeyDeleteSnapMs, ease))
        emit tracksChanged();
}

void Timeline::clearTrackClipInsetKeys(int clipId, qint64 sourceMs)
{
    TrackClip *c = mutableTrackClipById(clipId);
    if (!c || c->insetKeys.isEmpty())
        return;
    c->look = c->insetKeys.sample(sourceMs);
    c->insetKeys.clear();
    emit tracksChanged();
}

// ---------------------------------------------------------------------------
// Linked audio (to main-track or overlay clips)
// ---------------------------------------------------------------------------

bool Timeline::linkAnchor(int clipId, LinkAnchor *anchor) const
{
    LinkAnchor a;
    const int idx = indexOfId(clipId);
    if (idx >= 0) {
        const ClipItem &c = m_clips[idx];
        a = {clipToTimeline(idx, 0), c.trimInMs, c.trimOutMs, c.speed};
    } else {
        int trackId = 0;
        const TrackClip *c = trackClipById(clipId, &trackId);
        if (!c || trackById(trackId)->kind != Track::Kind::Video)
            return false;
        a = {c->startMs, c->trimInMs, c->trimOutMs, c->speed};
    }
    if (anchor)
        *anchor = a;
    return true;
}

bool Timeline::linkAudioClip(int audioClipId, int videoClipId)
{
    Track *t = nullptr;
    TrackClip *a = mutableTrackClipById(audioClipId, &t);
    if (!a || t->kind != Track::Kind::Audio)
        return false;
    if (videoClipId == 0) {
        if (!a->linkedClipId)
            return true;
        a->linkedClipId = 0;
        a->syncOffsetMs = 0;
        emit tracksChanged();
        return true;
    }
    LinkAnchor v;
    if (!linkAnchor(videoClipId, &v))
        return false;
    a->linkedClipId = videoClipId;
    a->speed = v.speed;
    // Where the link alone would put it; the difference is the sync offset,
    // so linking doesn't move anything in time.
    a->syncOffsetMs =
        a->startMs - (v.startMs + std::llround((a->trimInMs - v.trimInMs) / v.speed));
    // The main video's sound back onto its stream track (an overlay's:
    // relinkAudio).
    if (const Track::Role role = streamRole(*a);
        indexOfId(videoClipId) >= 0 && role != Track::Role::Free && t->role != role) {
        const QList<int> emptied{t->id}; // `t` and `a` don't survive addTrack
        int home = streamTrackId(role);
        if (!home)
            home = addTrack(Track::Kind::Audio, QString(), role);
        transferTrackClip(audioClipId, mutableTrackById(emptied[0]), mutableTrackById(home));
        dropEmptyOverlayAudioTracks(emptied);
    }
    relinkAudio();
    emit tracksChanged();
    return true;
}

bool Timeline::relinkAudio()
{
    bool changed = false;
    for (Track &t : m_tracks) {
        bool moved = false;
        for (TrackClip &a : t.clips) {
            if (!a.linkedClipId)
                continue;
            LinkAnchor v;
            if (!linkAnchor(a.linkedClipId, &v)) { // its clip is gone (shouldn't
                a.linkedClipId = 0;                // happen: removal takes the audio)
                a.syncOffsetMs = 0;
                changed = true;
                continue;
            }
            if (a.speed != v.speed) {
                a.speed = v.speed;
                a.setFades(a.fadeInMs, a.fadeOutMs); // the span changed
                changed = true;
            }
            const qint64 start = std::max<qint64>(
                0, v.startMs + std::llround((a.trimInMs - v.trimInMs) / v.speed) + a.syncOffsetMs);
            if (start != a.startMs) {
                a.startMs = start;
                moved = changed = true;
            }
        }
        if (moved)
            t.sortClips();
    }
    return tidyOverlayAudio() || changed;
}

bool Timeline::tidyOverlayAudio()
{
    struct Move {
        int clipId, fromTrackId, overlayTrackId;
    };
    QList<Move> moves;
    for (const Track &t : m_tracks) {
        if (t.kind != Track::Kind::Audio)
            continue;
        for (const TrackClip &a : t.clips) {
            int overlayTrackId = 0;
            if (!a.linkedClipId || !trackClipById(a.linkedClipId, &overlayTrackId))
                continue; // free, or the main video's sound
            if (t.role != Track::Role::OverlayAudio || t.ownerTrackId != overlayTrackId)
                moves.append({a.id, t.id, overlayTrackId});
        }
    }
    QList<int> emptied;
    for (const Move &m : moves) {
        int to = audioTrackId(m.overlayTrackId);
        if (!to) {
            const Track &owner = *trackById(m.overlayTrackId);
            Track t;
            t.id = to = m_nextTrackId++;
            t.role = Track::Role::OverlayAudio;
            t.ownerTrackId = owner.id;
            t.name = tr("%1 audio").arg(owner.name);
            placeTrack(t);
        }
        transferTrackClip(m.clipId, mutableTrackById(m.fromTrackId), mutableTrackById(to));
        emptied.append(m.fromTrackId);
    }
    dropEmptyOverlayAudioTracks(emptied);
    return !moves.isEmpty();
}

void Timeline::dropEmptyOverlayAudioTracks(const QList<int> &trackIds)
{
    for (int id : trackIds)
        if (const Track *t = trackById(id);
            t && t->role == Track::Role::OverlayAudio && t->clips.isEmpty())
            m_tracks.removeAt(trackIndexOfId(id));
}

bool Timeline::followTrim(int videoClipId, qint64 oldIn, qint64 oldOut)
{
    LinkAnchor v;
    if (!linkAnchor(videoClipId, &v))
        return false;
    bool changed = false;
    for (Track &t : m_tracks) {
        for (int i = t.clips.size() - 1; i >= 0; --i) {
            TrackClip &a = t.clips[i];
            if (a.linkedClipId != videoClipId)
                continue;
            qint64 in = a.trimInMs == oldIn ? v.trimInMs : std::max(a.trimInMs, v.trimInMs);
            qint64 out = a.trimOutMs == oldOut ? v.trimOutMs : std::min(a.trimOutMs, v.trimOutMs);
            in = std::max<qint64>(0, in);
            if (a.sourceDurationMs > 0)
                out = std::min(out, a.sourceDurationMs);
            if (in == a.trimInMs && out == a.trimOutMs)
                continue;
            changed = true;
            if (out - in < TrackClip::kMinTrimMs) { // trimmed away with the video
                t.clips.removeAt(i);
                continue;
            }
            a.trimInMs = in;
            a.trimOutMs = out;
            a.setFades(a.fadeInMs, a.fadeOutMs);
        }
    }
    return changed;
}

bool Timeline::splitLinkedAudio(int leftId, int rightId, qint64 atSourceMs)
{
    bool changed = false;
    for (Track &t : m_tracks) {
        QList<TrackClip> added;
        for (TrackClip &a : t.clips) {
            if (a.linkedClipId != leftId || a.trimOutMs - atSourceMs < TrackClip::kMinTrimMs)
                continue; // (all or nearly all before the cut: stays with the left clip)
            changed = true;
            if (atSourceMs - a.trimInMs < TrackClip::kMinTrimMs) {
                a.linkedClipId = rightId; // all after the cut
                continue;
            }
            TrackClip r = a;
            r.id = m_nextId++;
            r.linkedClipId = rightId;
            r.trimInMs = atSourceMs;
            r.setFades(0, a.fadeOutMs);
            a.trimOutMs = atSourceMs;
            a.setFades(a.fadeInMs, 0);
            added.push_back(r);
        }
        t.clips.append(added);
    }
    return changed;
}

bool Timeline::removeLinkedAudio(int videoClipId, QList<int> *tracks)
{
    bool changed = false;
    for (Track &t : m_tracks)
        if (t.clips.removeIf([videoClipId](const TrackClip &a) {
                return a.linkedClipId == videoClipId;
            }) > 0) {
            changed = true;
            if (tracks)
                tracks->append(t.id);
        }
    return changed;
}
