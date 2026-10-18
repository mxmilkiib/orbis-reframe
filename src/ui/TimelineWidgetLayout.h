#pragma once

// TimelineWidget's measures, shared by its source files: the layout
// (TimelineWidget.cpp), painting (TimelineWidgetPaint.cpp) and input
// (TimelineWidgetInput.cpp). Private: only those include it.

#include <QRect>
#include <QtGlobal>

namespace timelinelayout {

constexpr int kHeaderW = 180; // track headers, left of the clips
constexpr int kMarginX = 3;
constexpr int kRulerH = 14;
constexpr int kMainH = 40;    // the main video track's row
constexpr int kTrackH = 28;   // an audio track's row
constexpr int kRowGap = 2;
constexpr int kGroupGap = 8; // more air between two groups of tracks
constexpr int kNameX = 20;     // a header's name, right of the fold triangle
constexpr int kOwnSoundX = 32; // a video's own sound tracks' names, indented under it
constexpr int kEdgeGrabPx = 6;
constexpr int kTransitionH = 16;    // a transition's box, at the top of the main row
constexpr int kTransitionMinW = 14; // ... and its least width
constexpr int kDragThresholdPx = 4;
constexpr int kSnapPx = 6;
constexpr int kMuteBoxW = 16;
constexpr int kLayerBoxW = 16; // an overlay track's layer number, left of its hide box
constexpr qint64 kMinTrimMs = 100; // belt-and-braces; Timeline does the real clamp
constexpr int kScrollH = 12;          // the horizontal scrollbar under the tracks
constexpr qint64 kMinVisibleMs = 1000; // the furthest zoom in: 1 s across the view
constexpr double kWheelZoom = 1.25;    // per wheel notch / zoomIn()
constexpr int kEdgeScrollBand = 16;    // a drag this close to the rows' top / bottom scrolls them

// A track header's mute (or, on an overlay track, hide) box.
inline QRect muteBox(int rowTop, int rowH)
{
    return QRect(kHeaderW - kMuteBoxW - 5, rowTop + (rowH - 14) / 2, kMuteBoxW, 14);
}

} // namespace timelinelayout
