#pragma once

#include <limits>
#include "ReframeTrack.h"
#include <BlurAddition.h>   // privacy::BlurAddition (header-only, no deps)
#include <BlurRemoval.h>    // privacy::BlurRemoval (header-only, no deps)
#include <ColorLut.h>       // color::Grade (no Qt)
#include <GyroStabilizer.h> // insv::StabConfig (POD; no FFmpeg/Qt)
#include <IStitchSource.h>  // stitch::PreviewStitchConfig::LensGuard (POD; no FFmpeg)

#include <QString>

#include <cmath>

#include <array>
#include <cstdint>

// Per-clip colour grade (Colour panel): a static adjustment, not animated
// like the reframe path -- there is no colour keyframing. Applied at render
// time (order: the LUT, then temperature, exposure, contrast, saturation) by
// src/render/shaders/color_grade.glsl, which every grading shader includes
// (playback, GPU export), and by its C++ twin gradeRgb() in
// src/export/ReframeRenderer.cpp (CPU export fallback) -- the two must be
// changed together if the formula ever changes.
struct ColorGrade {
    static constexpr float kMinExposureEv = -3.0f;
    static constexpr float kMaxExposureEv = 3.0f;
    static constexpr float kMinPercent = -100.0f;
    static constexpr float kMaxPercent = 100.0f;

    float exposureEv = 0.0f;  // stops (2^ev multiplier), [kMinExposureEv, kMaxExposureEv]
    float contrast = 0.0f;    // percent, [kMinPercent, kMaxPercent], 0 = neutral
    float saturation = 0.0f;  // percent, [kMinPercent, kMaxPercent], -100 = grayscale
    float temperature = 0.0f; // percent, [kMinPercent, kMaxPercent], warm(+)/cool(-)
    // A 3D LUT (.cube) applied before the four above, e.g. a camera maker's
    // log-to-Rec.709 one: absolute path, empty = none. Copied into the
    // project's luts/ folder when picked (ProjectFolder.h); the project file
    // stores it relative to the project like media.
    QString lut;
    float lutStrength = 100.0f; // percent, [0, kMaxPercent]: mix of the LUT's output

    bool operator==(const ColorGrade &o) const
    {
        return exposureEv == o.exposureEv && contrast == o.contrast &&
              saturation == o.saturation && temperature == o.temperature && lut == o.lut &&
              (lut.isEmpty() || lutStrength == o.lutStrength);
    }
    bool operator!=(const ColorGrade &o) const { return !(*this == o); }

    // The four adjustments as the renderers take them (shader push constant
    // pc.colorGrade): {exposureEv, contrast, saturation, temperature}.
    std::array<float, 4> packed() const { return {exposureEv, contrast, saturation, temperature}; }
    // What the renderers apply: packed() plus the LUT, loaded (once per file,
    // color::cachedCube). A LUT that is missing or can't be read is left out;
    // *lutError then says why.
    color::Grade render(std::string *lutError = nullptr) const;

    // Clamp every field into its valid range, in place.
    void clamp();
};

// One clip on the timeline: a reference to a source .insv -- or a flat video
// (`flat`) -- plus the edits applied to it: an in/out trim range and, for a
// 360 clip, an authored reframe (pan / tilt / zoom) path.
//
// Plain value type. Timeline owns a QList<ClipItem>, is the only thing that
// mutates one, and turns every successful change into a signal -- so the
// mutating methods here just clamp, apply, and report whether anything
// actually changed; they carry no Qt signal/slot machinery. Trivially
// copyable; Project serialises it.
struct ClipItem {
    // Shortest allowed trimmed span -- guards an inverted / zero-width range;
    // ~100 ms is a few frames at any real rate.
    static constexpr qint64 kMinTrimMs = 100;
    // Looser snap for deleting a reframe key than for up-serting one: the
    // playhead rarely lands exactly on a key after a seek.
    static constexpr qint64 kReframeDeleteSnapMs = 150;

    int id = 0; // stable within one Timeline; assigned by Timeline on insert
    QString sourcePath;

    // Source media facts, filled by the caller from stitch::ProbeClip so this
    // stays free of any FFmpeg dependency. Zero until known.
    qint64 sourceDurationMs = 0;
    double fps = 0.0; // > 0 once added (probed) or loaded
    int sourceWidth = 0;
    int sourceHeight = 0;
    // A flat video (a phone's or action cam's mp4 / mov) rather than an
    // .insv: drawn into the output frame as it is (`fit`), not stitched or
    // reframed. Reframe, stabilization, privacy blur and the sync / seam
    // measurements don't apply to it: their setters refuse, so they stay at
    // their defaults.
    bool flat = false;
    // How a flat clip whose shape differs from the output fills it: the
    // whole picture with black bars, or covering the frame with the
    // overhang cropped. Unused on a 360 clip.
    enum class Fit { Bars, Fill };
    Fit fit = Fit::Bars;
    // Set on project load (step 4) when sourcePath can't be found. The clip
    // stays on the timeline so the edit isn't lost.
    bool mediaMissing = false;

    // Edits.
    qint64 trimInMs = 0;
    qint64 trimOutMs = 0;
    // Keyed in absolute source-clip milliseconds (a key is attached to a
    // moment of footage, so trimming doesn't move it). Timeline playback
    // samples it at trimInMs + <clip-local offset>.
    ReframeTrack reframe;

    // A clip has no sound of its own: each of its source's audio streams is
    // a linked clip on an audio track (Timeline::addClipSound).

    // Colour grade (Colour panel). See ColorGrade's own comment.
    ColorGrade color;

    // Gyro stabilization (Stabilization panel: Off / Lean follow / Horizon
    // lock / Custom). Applied at reprojection time, composed with the reframe
    // pose; a cut between clips with different settings switches at the cut.
    // On (any mode but Off) also turns on the stitch's capture-time
    // (rolling-shutter) correction, which is baked into the stitched frames.
    // A new clip takes the setting of its neighbour (Timeline::insertClip).
    insv::StabConfig stabilization;

    // Privacy blur (Privacy panel): mosaic the number plates this clip's plate
    // scan found (src/privacy/PlateScanData.h -- the scan itself is cached,
    // not stored here). Applied in playback and export; an export with this
    // on refuses a clip whose trim range hasn't been scanned.
    bool blurPlates = false;
    // Which scan of the source is this clip's (PlateScanCache): every scan
    // makes a new one, so a copy or a split half shares its clip's until one
    // of them is scanned again. Empty = the scan kept per source file before
    // scans were per clip (a project from then; none for a new clip).
    QString plateScan;
    // Blur tracks the user removed from this clip (false hits, e.g. on the
    // rider's own bike). Kept as each track's path on the sphere, so they stay
    // removed after a rescan (BlurRemoval.h).
    std::vector<privacy::BlurRemoval> blurRemovals;
    // Plates blurred by hand ("Blur a plate…": followed from where the user
    // boxed them). Applied whether blurPlates is on or not.
    std::vector<privacy::BlurAddition> blurAdditions;

    // Playback speed: 1 = real time, 0.25 = 4x slow motion, 2 = double speed.
    // Stretches the clip's span on the timeline (timelineDurationMs = trimmed /
    // speed); trims, reframe keys, stabilization and sync all stay in source
    // time. Audio is time-stretched to match, pitch unchanged (ClipAudioReader).
    static constexpr double kMinSpeed = 0.1;
    static constexpr double kMaxSpeed = 4.0;
    double speed = 1.0;

    // How the source's two lens pictures are joined (Stitching panel): what
    // was fitted over the lenses when it was filmed, and the depth-aware seam
    // (see stitch::PreviewStitchConfig). A property of the source file, so
    // clips cut from the same file share it (Timeline::setSourceStitch); a
    // clip of a new file takes the nearest 360 clip's (Timeline::insertClip).
    // Unused on a flat clip.
    stitch::PreviewStitchConfig::LensGuard lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
    bool depthSeam = true;

    // Measured video<->IMU sync fine-tune for this source, ms, added to the
    // trailer-derived offset (stitch::MeasureClipSync). NaN = not measured yet.
    // A property of the source file, so clips cut from the same file share it.
    double syncAdjustMs = std::numeric_limits<double>::quiet_NaN();

    // Measured seam alignment fine-tune for this source
    // (stitch::MeasureClipSeam): lens 1's extra yaw / pitch / roll,
    // degrees. Measured under a lens-guard profile, recorded in
    // seamAdjustGuard (stitch::PreviewStitchConfig::LensGuard as int; -1 = not
    // measured) -- a different guard setting needs a new measurement. Shared
    // by clips of the same source, like syncAdjustMs.
    std::array<double, 3> seamAdjustDeg{0.0, 0.0, 0.0};
    int seamAdjustGuard = -1;

    qint64 trimmedDurationMs() const { return trimOutMs - trimInMs; }
    // The clip's span on the timeline: the trimmed source span at `speed`.
    qint64 timelineDurationMs() const { return std::llround(trimmedDurationMs() / speed); }
    // Timeline-local ms (from the clip's start on the timeline) <-> source ms.
    qint64 sourceAtLocalMs(qint64 localMs) const { return trimInMs + std::llround(localMs * speed); }
    qint64 localAtSourceMs(qint64 sourceMs) const
    {
        return std::llround((sourceMs - trimInMs) / speed);
    }

    // Snap tolerance for up-serting a reframe key ("the same playhead frame
    // updates in place"): one frame.
    qint64 frameDurationMs() const;

    // Reset the trim to the whole source and clear the reframe path. Call
    // right after sourceDurationMs is set.
    void resetEditsToFullSource();

    // --- mutations: clamp, apply, return true iff something changed ---------
    bool setTrimIn(qint64 positionMs);
    bool setTrimOut(qint64 positionMs);
    bool clearTrim();
    bool upsertReframeKey(qint64 sourceMs, const ReframePose &pose);
    bool removeReframeKeyNear(qint64 sourceMs);
    bool clearReframe();
    bool setColorGrade(const ColorGrade &grade);
    bool setStabilization(const insv::StabConfig &config);
    bool setBlurPlates(bool on);
    bool setPlateScan(const QString &key);
    bool setBlurRemovals(const std::vector<privacy::BlurRemoval> &removals);
    bool setBlurAdditions(const std::vector<privacy::BlurAddition> &additions);
    bool setSpeed(double s); // clamped to [kMinSpeed, kMaxSpeed]
    bool setFit(Fit f);

private:
    // Clamp [inMs, outMs] into [0, sourceDuration] with the min-gap rule (the
    // opposite edge never moves), apply, return whether it changed. (Was
    // ClipModel::setRange.)
    bool setRange(qint64 inMs, qint64 outMs);
};
