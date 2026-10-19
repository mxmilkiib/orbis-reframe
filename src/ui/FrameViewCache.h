#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "PlaybackController.h" // PlaybackController::ViewProvider

#include "../core/ReframeTrack.h"

class Timeline;

// Each main-track clip's reframe path and colour grade, copied for the
// playback thread: provider() gives a presented frame the view of its own
// clip at its own source time, and that clip's grade, so they change with
// the picture at a cut (EquirectViewportWindow::FrameView). MainWindow
// refreshes the copy whenever a clip's path or grade, or the clips, change.
class FrameViewCache {
public:
    FrameViewCache();

    // GUI thread: copy every main-track clip's path and grade.
    void update(const Timeline &timeline);
    // While the user drags the view, frames leave it alone (a clip with
    // keys would otherwise pull it back every frame).
    void setDragging(bool dragging);

    // For PlaybackController::setViewProvider. A clip without keys keeps
    // the view as it is (free look); one not in the copy keeps both.
    PlaybackController::ViewProvider provider() const;

private:
    struct Clip {
        ReframeTrack reframe;
        color::Grade grade;
    };
    struct Shared {
        std::mutex mx;
        std::map<int, Clip> clips; // by ClipItem::id
        std::atomic<bool> dragging{false};
    };
    std::shared_ptr<Shared> m_shared;
};
