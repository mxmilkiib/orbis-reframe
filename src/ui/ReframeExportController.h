#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QString>

#include <AudioMixer.h>     // insv::MixClip (Request::mix)
#include <InsetCompositor.h> // exportpipe::InsetClipJob (Request::insets)
#include <GyroStabilizer.h> // insv::GyroStabilizer (ClipJob::stab)
#include <IStitchSource.h>  // stitch::CaptureTiming

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <cstdint>
#include <vector>

// Runs a whole-timeline flat reframe export off the GUI thread. MainWindow
// creates one, moves it onto a worker QThread, connects the signals to a
// QProgressDialog, and invokes run() queued onto that thread.
//
// For each clip on the timeline it opens a stitch::NativeStitchSource, seeks to
// the clip's in-point and reads its trimmed range frame by frame (full-res
// equirect, in-process, no intermediate file), feeding each frame + its baked
// pose straight into exportpipe::FlattenWriter (CPU reproject + NVENC). Every
// clip goes through one open encoder so the output is a single continuous file.
//
// Audio: if anything is on the audio tracks (Request::mix -- the video clips'
// own streams are linked clips there too), the output gets one AAC track.
// An insv::AudioMixer sums them at the output timeline's frames, fed after
// every video frame up to that frame's end -- so the audio spans exactly the
// exported video and rounding never accumulates across clips -- and the
// whole mix is limited: the same code playback runs (AudioPlayer).
//
// Overlays (Request::insets): an exportpipe::InsetCompositor blends the
// insets showing at each output frame's timeline time into that frame, on
// the NV12 picture just before encoding (FlattenWriter's frame hook), so the
// GPU and CPU reprojection paths get the same overlay.
//
// The per-clip pose arrays are baked on the GUI side from each clip's
// ReframeTrack (that state lives on the GUI thread, in the Timeline) and handed
// in via Request, so this class never touches the edit model.
class ReframeExportController : public QObject {
    Q_OBJECT

public:
    explicit ReframeExportController(QObject *parent = nullptr);

    // The stitch's capture-time correction for a clip stabilized by `stab`:
    // every sensor row of both lenses brought to the instant the stab matrix
    // is computed for (the X5's rolling shutter, `stab->readoutSec()`).
    // Inactive when `stab` is null / not ready. Shared by playback
    // (MainWindow) and export.
    static stitch::CaptureTiming captureTimingFor(std::shared_ptr<const insv::GyroStabilizer> stab);
    // A stabilizer for clip file `path` (frame rate `fps`) under `config`,
    // with the source's sync fine-tune `syncMs`: built from its IMU track
    // (reads the trailer). Null when it has none.
    static std::shared_ptr<const insv::GyroStabilizer>
    buildStabilizer(const QString &path, double fps, const insv::StabConfig &config, double syncMs);

    // One timeline clip's contribution to the export.
    struct ClipJob {
        // A flat video (ClipItem::flat): nothing is stitched; its span is
        // black frames that the insets (Request::insets, which carry the
        // clip itself as a full-frame layer) are drawn over.
        bool flat = false;
        QString clipPath;
        qint64 trimInMs = 0;
        qint64 trimOutMs = 0;
        // Playback speed (ClipItem::speed): the clip fills (trimOut - trimIn) /
        // speed of the output. Audio is time-stretched to match (pitch kept).
        double speed = 1.0;
        // The view {yaw, pitch, roll, fovY} at a source time (ms), sampled per
        // decoded frame at that frame's own timestamp; empty => static forward
        // view. Must be safe to call from the export thread (capture copies).
        std::function<void(qint64 sourceMs, float pose[4])> poseAt;
        // The gyro de-rotation, evaluated per decoded frame at its own
        // timestamp (never at trimIn + i/fps: the first decoded frame is the
        // one at-or-after trimIn, and a frame of phase error undoes most of
        // the stick shake correction) and applied after the pose in the sphere
        // frame, like the viewport does. Null => no stabilization.
        std::shared_ptr<const insv::GyroStabilizer> stab;
        // {exposureEv, contrast%, saturation%, temperature%} -- the Colour
        // panel grade, static for the whole clip (no colour keyframing).
        // All-zero (the default) => neutral.
        float colorGrade[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        // The clip's stitch settings (ClipItem::lensGuard / depthSeam) --
        // same geometry as the preview.
        stitch::PreviewStitchConfig::LensGuard lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
        bool depthSeam = true;
        // Measured seam fine-tune (lens-1 yaw/pitch/roll, deg).
        std::array<double, 3> seamAdjustDeg{0.0, 0.0, 0.0};
        // Privacy blur: the plate regions per source time, mosaicked in the
        // stitch (IStitchSource::SetBlurQuads); empty => none. Safe to call
        // from the export thread (captures its scan).
        stitch::BlurQuadsFn blur;
    };

    struct Request {
        QString outPath;
        double fps = 30.0; // one output rate for the whole timeline
        int outWidth = 1920;
        int outHeight = 1080;
        bool h265 = true;
        qint64 bitrateKbps = 40000;
        // Equirect intermediate resolution (2:1). 0 => auto, per clip:
        // stitch::ExportStitchWidth (the full source sphere, >= 2*outWidth).
        int stitchWidth = 0;
        int stitchHeight = 0;
        QList<ClipJob> clips;
        // The extra audio tracks' clips, timeline frames at 48 kHz.
        std::vector<insv::MixClip> mix;
        // The overlay tracks' clips, bottom-most first: composited into every
        // output frame at its timeline time.
        std::vector<exportpipe::InsetClipJob> insets;
    };

    // One still frame of `clip` at `sourceMs` (the first source frame at or
    // after it, same as playback's GetFrame), rendered the way run() renders
    // it: the fused GPU pass straight from the fisheyes (else a full-sphere
    // equirect + CPU reproject), the clip's stabilization / seam / grade, to
    // outW x outH through `pose` ({yaw, pitch, roll, fovY} radians;
    // clip.poseAt is ignored). RGB, no chroma subsampling -- for the
    // player bar's screenshot button. Blocks (opens its own stitch source);
    // run it off the GUI thread. Null image + `error` on failure.
    static QImage renderStill(const ClipJob &clip, qint64 sourceMs, const float pose[4],
                              int outW, int outH, QString &error);

    // Blocks the calling thread for the whole export (invoke it queued onto the
    // worker thread). Emits progress along the way and finished() at the end.
    void run(const Request &request);

    // Ask a running export to stop at the next frame boundary. Safe to call
    // from any thread -- it just sets an atomic flag. run() then deletes the
    // partial output file and emits finished(false, QString(), outPath): an
    // empty error string on !ok means "cancelled by the user", not a failure.
    void cancel() { m_cancel.store(true, std::memory_order_relaxed); }

signals:
    // overallPct: 0..100 across the whole timeline. label: what's happening now
    // (e.g. "Stitching clip 2 of 3…"), ready to drop straight into the dialog.
    void progress(int overallPct, const QString &label);
    void finished(bool ok, const QString &error, const QString &outPath);

private:
    std::atomic<bool> m_cancel{false};
};
