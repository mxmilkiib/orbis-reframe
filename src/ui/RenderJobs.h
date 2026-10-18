#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include <AudioMixer.h>      // insv::MixClip
#include <InsetCompositor.h> // exportpipe::InsetClipJob

#include "InsetFeed.h"               // InsetFeed::Clip
#include "ReframeExportController.h" // ReframeExportController::ClipJob

struct ClipItem;
class PrivacyBlur;
class Timeline;

// The edit model as the renderers want it: what playback, the export and a
// screenshot are handed. Pure functions of the Timeline (and the privacy
// blur), on the GUI thread; what they return is safe to use on another.
namespace renderjobs {

// The audio tracks as the mixer wants them (muted tracks / clips and missing
// files left out). At a transition (Transition.h) the sound linked to its
// two clips crossfades over it: the clip before's runs on past the cut and
// fades out, the clip after's starts early and fades in. Playback and export
// both use this.
std::vector<insv::MixClip> mixClips(const Timeline &timeline);

// The overlay clips (hidden tracks and missing files left out), bottom-most
// first: for playback's InsetFeed, and for the export / a screenshot. Under
// them, the flat main-track clips (ClipItem::flat) as full-frame layers,
// fitted into an output of outW x outH, and over those each transition's
// other clip (transitionLayers): a 360 clip's view through `privacy`'s
// blur, or a flat clip.
std::vector<InsetFeed::Clip> insetFeed(const Timeline &timeline, int outW, int outH,
                                       const PrivacyBlur &privacy);
std::vector<exportpipe::InsetClipJob> insetJobs(const Timeline &timeline, int outW, int outH,
                                                const PrivacyBlur &privacy);

// What a transition shows besides the main track, in timeline order. A
// crossfade: over the first half of it the clip after the cut, from before
// its trim-in, blending in to half; over the second half the clip before
// the cut, past its trim-out, blending out from half. The main track plays
// on as it is, so with the layer over it the two clips blend through the
// cut. A dip to black: black, coming in up to the cut and going after it. A
// transition that doesn't play (Timeline::transitionSpan) has none.
struct TransitionLayer {
    int id = 0;          // unique among the layers, and not a clip's id (< 0)
    int clipIndex = -1;  // the main-track clip it shows; -1: black
    int overClipId = 0;  // the main-track clip it lies over (ClipItem::id)
    qint64 startMs = 0;  // timeline
    qint64 trimInMs = 0; // its source
    qint64 trimOutMs = 0;
    float rampFrom = 0.0f; // opacity at its start and end
    float rampTo = 0.0f;
};
std::vector<TransitionLayer> transitionLayers(const Timeline &timeline);

// One main-track clip as the export renders it: trim, speed, grade, the
// privacy blur, its stitch settings and the seam fine-tune measured under
// them, a stabilizer built from its IMU track (when its stabilization is on;
// reads the trailer), and its reframe path.
ReframeExportController::ClipJob clipJob(const ClipItem &c, const Timeline &timeline,
                                         const PrivacyBlur &privacy);

// The source files an export needs that are missing (ClipItem::mediaMissing,
// TrackClip::mediaMissing as last checked), each once, in timeline order:
// main-track clips first, then overlay and audio clips. Clips on hidden or
// muted tracks and muted audio clips aren't exported, so they don't count.
QStringList missingSources(const Timeline &timeline);

// Where an export of the project in `projectFile` is suggested: its
// exports/ folder (made if need be; else the project file's own folder),
// named after the project, with -2, -3, ... if that's taken; ~/timeline.mp4
// for an unsaved project.
QString suggestedExportPath(const QString &projectFile);
// Where a screenshot at `timelineMs` is suggested: the project's
// screenshots/ folder (made if need be; else the project file's own
// folder), named after the project; Pictures (else home) while the project
// is unsaved, named after `clipSource`. With the time ("_01m02s003") and a
// -2, -3, ... if the name is taken.
QString suggestedScreenshotPath(const QString &projectFile, const QString &clipSource,
                                qint64 timelineMs);

} // namespace renderjobs
