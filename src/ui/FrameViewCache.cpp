#include "FrameViewCache.h"

#include "../core/Timeline.h"

FrameViewCache::FrameViewCache() : m_shared(std::make_shared<Shared>()) {}

void FrameViewCache::update(const Timeline &timeline)
{
    std::map<int, Clip> clips;
    for (int i = 0; i < timeline.count(); ++i) {
        const ClipItem &c = timeline.clipAt(i);
        clips[c.id] = {c.reframe, c.color.render()};
    }
    std::lock_guard<std::mutex> lk(m_shared->mx);
    m_shared->clips = std::move(clips);
}

void FrameViewCache::setDragging(bool dragging)
{
    m_shared->dragging = dragging;
}

PlaybackController::ViewProvider FrameViewCache::provider() const
{
    return [shared = m_shared](int clipId, qint64 sourceMs,
                               EquirectViewportWindow::FrameView &view) {
        std::lock_guard<std::mutex> lk(shared->mx);
        const auto it = shared->clips.find(clipId);
        if (it == shared->clips.end())
            return;
        view.hasGrade = true;
        view.grade = it->second.grade;
        if (it->second.reframe.isEmpty() || shared->dragging)
            return;
        const ReframePose p = it->second.reframe.sample(sourceMs);
        view.hasPose = true;
        view.pose[0] = p.yaw;
        view.pose[1] = p.pitch;
        view.pose[2] = p.roll;
        view.pose[3] = p.fovY;
    };
}
